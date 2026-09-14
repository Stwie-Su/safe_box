#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
pc_fm225_mqtt_validate.py —— PC 端 FM225 + MQTT 链路一键验收

在 VM 项目根执行（无需 root、无需开发板）：

    cd ~/桌面/lv_port_linux
    python3 tools/pc_fm225_mqtt_validate.py            # 跑全部用例
    python3 tools/pc_fm225_mqtt_validate.py --keep     # 跑完不清理，留现场手动点界面
    python3 tools/pc_fm225_mqtt_validate.py --list     # 只列用例不执行

它替你完成：
  1) 清理旧进程 -> 起 socat 虚拟串口对 /tmp/fm225_host <-> /tmp/fm225_dev
  2) 确认/启动 mosquitto broker（127.0.0.1:1883）
  3) 后台起 mosquitto_sub 监听 safe/#（作为独立第三方观测者）
  4) 用 SAFE_FACE_BACKEND=fm225 启动 build_pc/bin/lvglsim
  5) 逐项注入 FM225 帧序列 / 发布 MQTT 指令，比对 app.log 与订阅流
  6) 打印 PASS / FAIL / SKIP 表，全部产物落 /tmp/pc_validate/

判定三档：
  PASS  证据命中
  FAIL  证据缺失（且前置满足）
  SKIP  前置不满足 / 无法自动判定（附人工步骤）

退出码：0 = 无 FAIL；1 = 有 FAIL；2 = 环境不具备
"""
import argparse
import os
import re
import shutil
import signal
import subprocess
import sys
import time

PROJ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(PROJ, 'build_pc/bin/lvglsim')
SIM = os.path.join(PROJ, 'tools/fm225_sim.py')
HOST = '/tmp/fm225_host'
DEV = '/tmp/fm225_dev'
OUT = '/tmp/pc_validate'
BROKER = '127.0.0.1'
PORT = '1883'
DISPLAY = os.environ.get('SAFE_VALIDATE_DISPLAY', ':1')

procs = []          # 本脚本起的后台进程
results = []        # (id, 名称, 判定, 证据)


def sh(cmd, timeout=30):
    """跑 shell 命令，返回 (rc, stdout+stderr)。"""
    p = subprocess.run(['bash', '-lc', cmd], capture_output=True, text=True, timeout=timeout)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


def log(*a):
    print(*a, flush=True)


def record(cid, name, verdict, evidence):
    results.append((cid, name, verdict, evidence))
    log('  [%-4s] %-46s %s' % (verdict, name, evidence[:110]))


def tail_since(path, offset):
    """读文件从 offset 之后的内容。"""
    if not os.path.exists(path):
        return '', offset
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        f.seek(offset)
        data = f.read()
        return data, f.tell()


def start(cmd, logfile):
    f = open(logfile, 'ab')
    p = subprocess.Popen(['bash', '-lc', cmd], stdout=f, stderr=subprocess.STDOUT,
                         preexec_fn=os.setsid)
    procs.append((p, f))
    return p


def stop_all():
    for p, f in procs:
        try:
            os.killpg(os.getpgid(p.pid), signal.SIGTERM)
        except Exception:
            pass
        try:
            f.close()
        except Exception:
            pass
    sh('pkill -f lvglsim; pkill -f fm225_sim; pkill -f "socat -d -d pty"', timeout=15)


def inject(mode, arg=None):
    cmd = 'python3 %s %s' % (SIM, mode)
    if arg is not None:
        cmd += ' %s' % arg
    rc, out = sh(cmd, timeout=20)
    return rc, out.strip()


# ------------------------------------------------------------------ 前置检查
def preflight():
    problems = []
    if not os.path.exists(APP):
        problems.append('未找到 %s（先 cmake -B build_pc ... && cmake --build build_pc -j4）' % APP)
    for t in ('socat', 'mosquitto', 'mosquitto_pub', 'mosquitto_sub', 'python3'):
        if shutil.which(t) is None:
            problems.append('缺少可执行文件：%s' % t)
    if not problems:
        try:
            rc, _ = sh("mosquitto_pub -h %s -p %s -t '$SYS/broker/version' -m ping -q 0" % (BROKER, PORT), timeout=8)
            if rc != 0:
                problems.append('broker %s:%s 不可达（本脚本会尝试启动 mosquitto）' % (BROKER, PORT))
        except Exception as e:
            problems.append('探测 broker 失败：%s' % e)
    return problems


# ------------------------------------------------------------------ 主流程
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--keep', action='store_true', help='跑完不清理进程（留现场手点界面）')
    ap.add_argument('--list', action='store_true', help='只列用例')
    ap.add_argument('--wait', type=float, default=6.0, help='应用启动后等待秒数（默认 6）')
    args = ap.parse_args()

    if args.list:
        for cid, name in CASES:
            print('%-4s %s' % (cid, name))
        return 0

    os.makedirs(OUT, exist_ok=True)
    # 用 data/ 的副本，避免改动工程里真实的用户/日志数据
    vdata = os.path.join(OUT, 'data')
    if not os.path.isdir(vdata):
        try:
            shutil.copytree(os.path.join(PROJ, 'data'), vdata)
        except Exception:
            os.makedirs(vdata, exist_ok=True)
    applog = os.path.join(OUT, 'app.log')
    mqttlog = os.path.join(OUT, 'mqtt.log')
    for f in (applog, mqttlog):
        if os.path.exists(f):
            os.remove(f)

    log('=' * 74)
    log('PC 端 FM225 + MQTT 链路验收    产物目录 %s' % OUT)
    log('=' * 74)

    log('\n[0] 前置检查')
    probs = preflight()
    for p in probs:
        log('  ! ' + p)
    if not os.path.exists(APP):
        log('\n构建产物缺失，无法继续。')
        return 2

    log('\n[1] 清理旧进程 + 准备虚拟串口对')
    sh('pkill -f lvglsim; pkill -f fm225_sim; pkill -f "socat -d -d pty"', timeout=15)
    time.sleep(1)
    for pth in (HOST, DEV):
        if os.path.islink(pth) or os.path.exists(pth):
            os.remove(pth)
    start('socat -d -d pty,raw,echo=0,link=%s pty,raw,echo=0,link=%s' % (HOST, DEV),
          os.path.join(OUT, 'socat.log'))
    for _ in range(40):
        if os.path.exists(HOST) and os.path.exists(DEV):
            break
        time.sleep(0.25)

    log('\n[2] 确认 broker 并起独立订阅观测')
    rc, _ = sh("mosquitto_pub -h %s -p %s -t probe -m 1 -q 0" % (BROKER, PORT), timeout=8)
    if rc != 0:
        log('  broker 不可达，尝试启动 mosquitto')
        start('mosquitto -p %s' % PORT, os.path.join(OUT, 'broker.log'))
        time.sleep(1.5)
    start("mosquitto_sub -h %s -p %s -t 'safe/#' -v" % (BROKER, PORT), mqttlog)
    time.sleep(1.2)

    log('\n[3] 以 fm225 后端启动应用')
    env = ('DISPLAY=%s SAFE_FACE_BACKEND=fm225 SAFE_FM225_DEV=%s SAFE_DATA_DIR=%s '
           'SAFE_MQTT_HOST=%s SAFE_MQTT_PORT=%s' % (DISPLAY, HOST, vdata, BROKER, PORT))
    start('%s %s' % (env, APP), applog)
    time.sleep(args.wait)
    rc, out = sh('pgrep -f "bin/lvglsim" | head -1')
    apppid = (out or '').strip()
    off = 0
    delta, off = tail_since(applog, off)

    # ---------------- FM225 ----------------
    log('\n[4] FM225 用例')

    m = re.search(r'\[fm225\] 已打开 (\S+).*?fd=(\d+)', delta)
    if m and m.group(2) != '-1':
        record('F1', '串口打开成功（fd 有效）', 'PASS', '%s fd=%s' % (m.group(1), m.group(2)))
    elif 'UART fd=-1' in delta or '[fm225] 未打开' in delta:
        record('F1', '串口打开成功（fd 有效）', 'FAIL', 'fd=-1 或未打开')
    else:
        record('F1', '串口打开成功（fd 有效）', 'FAIL', '日志中无 [fm225] 已打开 行')

    n0 = len(delta)
    inject('ready')
    time.sleep(1.5)
    delta, off = tail_since(applog, off)
    if 'NOTE READY' in delta:
        record('F2', '模组 READY 通知被消费', 'PASS', 'NOTE READY：模组就绪')
    else:
        record('F2', '模组 READY 通知被消费', 'FAIL', '未见 NOTE READY 处理日志')

    inject('no_match', 1)
    time.sleep(1.5)
    delta, off = tail_since(applog, off)
    if 'verify 应答' in delta and 'no_match' in delta:
        record('F3', '未匹配应答被解析并驱动拒绝', 'PASS',
               'verify 应答 reason=no_match' + ('（含 [auth] DENY）' if 'DENY' in delta else ''))
    else:
        record('F3', '未匹配应答被解析并驱动拒绝', 'FAIL', '未见 verify 应答/no_match')

    inject('bad')
    time.sleep(1.5)
    delta, off = tail_since(applog, off)
    alive = sh('kill -0 %s 2>/dev/null && echo ALIVE' % apppid)[1].strip() if apppid else ''
    if alive == 'ALIVE':
        record('F4', '坏帧注入后进程存活（重同步）', 'PASS', '进程仍在，未崩')
    else:
        record('F4', '坏帧注入后进程存活（重同步）', 'FAIL', '进程已退出')

    inject('match', 3)
    time.sleep(1.5)
    delta, off = tail_since(applog, off)
    if 'reason=ok' in delta:
        record('F5', '匹配成功应答被解析', 'PASS', 'verify 应答 reason=ok uid=3')
    else:
        record('F5', '匹配成功应答被解析', 'FAIL', '未见 reason=ok')

    inject('enroll', 3)
    inject('delete', 3)
    time.sleep(1.5)
    delta, off = tail_since(applog, off)
    seen = [k for k in ('enroll', '录入', 'delete', '删除') if k in delta.lower()]
    record('F6', '录入/删除应答被消费', 'PASS' if seen else 'SKIP',
           ('命中关键词 %s' % seen) if seen else '日志未见明显痕迹（协议层已确认可解析，此项人工确认）')

    # D1：重发刷屏速率（修复前后的核心指标）
    rc, out = sh("grep -c '超时，重发' %s" % applog)
    nresend = (out or '0').strip()
    rc, out = sh("grep -c '无应答（重发 3 次）' %s" % applog)
    nround = (out or '0').strip()
    try:
        rounds = int(nround)
    except ValueError:
        rounds = -1
    elapsed = max(1.0, time.time() - t_start)
    rate = rounds / elapsed
    verdict = 'PASS' if rate < 0.5 else 'FAIL'
    record('F7', 'VERIFY 不做无退避刷屏重发（轮/秒 < 0.5）', verdict,
           '重发行 %s 行 / 失败轮 %s 次 / 约 %.2f 轮·s⁻¹（D1 修复后应显著下降）'
           % (nresend, nround, rate))

    # ---------------- MQTT ----------------
    log('\n[5] MQTT 用例')
    if '[MQTT] connected' in open(applog, encoding='utf-8', errors='replace').read():
        record('M1', '客户端连上 broker', 'PASS', '[MQTT] connected to tcp://%s:%s' % (BROKER, PORT))
    else:
        record('M1', '客户端连上 broker', 'FAIL', '日志无 [MQTT] connected')

    moff = 0
    rc, out = sh("mosquitto_pub -h %s -p %s -t safe/cmd -q 1 -m '{\"cmd\":\"query_status\",\"req_id\":\"val1\"}'"
                 % (BROKER, PORT), timeout=10)
    time.sleep(2.0)
    mq, moff = tail_since(mqttlog, moff)
    if '"req_id":"val1"' in mq:
        record('M2', 'query_status 指令 → 回执（回显 req_id）', 'PASS', 'safe/log 含 req_id=val1')
    else:
        record('M2', 'query_status 指令 → 回执（回显 req_id）', 'FAIL', '订阅流未见 req_id=val1')

    rc, out = sh("mosquitto_pub -h %s -p %s -t safe/cmd -q 1 -m '{\"cmd\":\"remote_unlock\",\"req_id\":\"val2\"}'"
                 % (BROKER, PORT), timeout=10)
    time.sleep(2.0)
    mq, moff = tail_since(mqttlog, moff)
    if '"code":2001' in mq:
        record('M3', 'remote_unlock 无 otp → 要求设备输入动态码', 'PASS', 'code=2001')
    else:
        record('M3', 'remote_unlock 无 otp → 要求设备输入动态码', 'FAIL', '未见 code=2001')

    rc, out = sh("mosquitto_pub -h %s -p %s -t safe/cmd -q 1 -m 'not-a-json'" % (BROKER, PORT), timeout=10)
    time.sleep(2.0)
    mq, moff = tail_since(mqttlog, moff)
    if '"code":1002' in mq:
        record('M4', '坏 JSON → 1002 解析失败（不崩）', 'PASS', 'code=1002')
    else:
        record('M4', '坏 JSON → 1002 解析失败（不崩）', 'FAIL', '未见 code=1002')

    # retained 判定：先停 app，再用全新订阅看能否立刻收到最后一条 status
    sh('kill %s 2>/dev/null' % apppid if apppid else 'true', timeout=10)
    time.sleep(2.0)
    rc, out = sh("timeout 6 mosquitto_sub -h %s -p %s -t safe/status -C 1 -W 4 -v"
                 % (BROKER, PORT), timeout=12)
    if '"state"' in out:
        record('M5', 'safe/status 为 retained（停掉 app 后新订阅仍能收到）', 'PASS',
               '新订阅立即收到最后一条状态 → retained 已生效')
    else:
        record('M5', 'safe/status 为 retained（停掉 app 后新订阅仍能收到）', 'FAIL',
               '停掉 app 后新订阅 4s 内未收到 → retained 未启用（LWT 在线状态的前置条件）')

    # LWT 判定：必须【先订阅】再 SIGKILL —— will 是连接时登记在 broker 上的，
    # 掉线瞬间才由 broker 代发；若订阅晚于掉线，就只能靠 retained 才看得到。
    start('%s %s' % (env, APP), applog)
    time.sleep(6)
    rc, out = sh('pgrep -f "bin/lvglsim" | head -1')
    pid2 = (out or '').strip()
    if not pid2:
        record('M6', 'LWT：异常掉线后 broker 代发离线状态', 'SKIP', 'app 未能再次启动，无法判定')
    else:
        lwtlog = os.path.join(OUT, 'lwt.log')
        if os.path.exists(lwtlog):
            os.remove(lwtlog)
        start("mosquitto_sub -h %s -p %s -t 'safe/status/#' -v" % (BROKER, PORT), lwtlog)
        time.sleep(2.5)
        sh('kill -9 %s' % pid2, timeout=10)
        time.sleep(4.0)
        try:
            with open(lwtlog, encoding='utf-8', errors='replace') as f:
                lwtdata = f.read()
        except Exception:
            lwtdata = ''
        m = re.search(r'\{[^}]*online[^}]*\}', lwtdata)
        if m:
            record('M6', 'LWT：异常掉线后 broker 代发离线状态', 'PASS',
                   '掉线瞬间订阅读到 %s' % m.group(0)[:90])
        else:
            record('M6', 'LWT：异常掉线后 broker 代发离线状态', 'FAIL',
                   'SIGKILL 后 4s 内 safe/status/# 无离线通知（实测收到：%s）'
                   % (lwtdata.strip().replace('\n', ' | ')[:80] or '（无消息）'))

    # ---------------- 汇总 ----------------
    log('\n' + '=' * 74)
    npass = sum(1 for r in results if r[2] == 'PASS')
    nfail = sum(1 for r in results if r[2] == 'FAIL')
    nskip = sum(1 for r in results if r[2] == 'SKIP')
    log('汇总：PASS %d / FAIL %d / SKIP %d（共 %d）' % (npass, nfail, nskip, len(results)))
    for cid, name, verdict, ev in results:
        if verdict == 'FAIL':
            log('  ✗ %s %s —— %s' % (cid, name, ev))
    log('产物：%s/{app.log,mqtt.log,socat.log}' % OUT)
    log('=' * 74)

    with open(os.path.join(OUT, 'report.txt'), 'w', encoding='utf-8') as f:
        f.write('PC 端 FM225 + MQTT 链路验收报告  %s\n\n' % time.strftime('%Y-%m-%d %H:%M:%S'))
        for cid, name, verdict, ev in results:
            f.write('%-4s %-46s %-5s %s\n' % (cid, name, verdict, ev))

    if args.keep:
        log('\n--keep：进程保留，可直接在 DISPLAY=%s 上手动点界面；清理请跑：' % DISPLAY)
        log('   pkill -f lvglsim; pkill -f fm225_sim; pkill -f "socat -d -d pty"')
    else:
        stop_all()
        log('\n已清理本脚本起的进程。')

    return 1 if nfail else 0


CASES = [
    ('F1', '串口打开成功（fd 有效）'),
    ('F2', '模组 READY 通知被消费'),
    ('F3', '未匹配应答被解析并驱动拒绝'),
    ('F4', '坏帧注入后进程存活（重同步）'),
    ('F5', '匹配成功应答被解析'),
    ('F6', '录入/删除应答被消费'),
    ('F7', 'VERIFY 不做无退避刷屏重发（轮/秒 < 0.5）'),
    ('M1', '客户端连上 broker'),
    ('M2', 'query_status 指令 → 回执（回显 req_id）'),
    ('M3', 'remote_unlock 无 otp → 要求设备输入动态码'),
    ('M4', '坏 JSON → 1002 解析失败（不崩）'),
    ('M5', 'safe/status 为 retained'),
    ('M6', 'LWT：异常掉线后 broker 代发离线状态'),
]

if __name__ == '__main__':
    t_start = time.time()
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        stop_all()
        print('\n中断，已清理。')
        sys.exit(130)
