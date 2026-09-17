#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
tools/fm225_enroll_sim.py —— FM225「假模组 + socat 假串口」协议层自测台。

模组没接在机器上时，用它替代真模组，把「录入人脸」这条链路整条跑一遍：
真后端（backend_fm225.c，跑在 build_pc/bin/test_fm225_enroll 的实时联调模式里）
←→ socat 造的一对 pty ←→ 本脚本扮演的假模组。

跑法（在仓库根目录）：
    python3 tools/fm225_enroll_sim.py                 # 全跑 4 个场景
    python3 tools/fm225_enroll_sim.py timeout silent  # 只跑指定场景

四个场景（对应「录入总是失败」的四类现场）：
  ① ok       模组空闲 → 收到 ENROLL → 正常应答 SUCCESS（含 NOTE 人脸状态上报）
  ② noinsert 录入进行中：主机**不许**插入任何查询（0x24 对账）或重发（第二条 0x13）
  ③ timeout  模组走满 10s 没看见脸 → 应答 FAILED4_TIMEOUT(0x0D)（真机故障现场那个码）
  ④ silent   模组被抢占后静默无应答（一条都不回）→ 主机必须在会话超时点自己兜底，
             不许永久挂起，也不许重发 ENROLL

判据放在本脚本侧（它看得见线上每一个字节）；C 侧只负责「必须拿到 ENROLL_DONE」。
"""

import os
import re
import select
import signal
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST_LINK = "/tmp/fm225_enroll_host"
MOD_LINK = "/tmp/fm225_enroll_mod"
def test_bin():
    """允许用 FM225_ENROLL_BIN 指定二进制位置（build_pc 可能被别的构建清掉）。"""
    return os.environ.get("FM225_ENROLL_BIN") or os.path.join(
        REPO, "build_pc", "bin", "test_fm225_enroll")

SYNC0, SYNC1 = 0xEF, 0xAA
MSGID_REPLY, MSGID_NOTE = 0x00, 0x01

CMD_RESET = 0x10        # MID_RESET：取消在途命令 → STANDBY
CMD_VERIFY = 0x12
CMD_ENROLL = 0x13
CMD_DELETE_USER = 0x20
CMD_FACE_RESET = 0x23   # FACE RESET：终止录入 + 清录入状态
CMD_GET_ALL_USERID = 0x24

CMD_NAME = {
    0x10: "RESET(0x10)", 0x11: "GETSTATUS(0x11)", 0x12: "VERIFY(0x12)",
    0x13: "ENROLL(0x13)", 0x20: "DELETE_USER(0x20)", 0x21: "DELETE_ALL(0x21)",
    0x22: "GETUSERINFO(0x22)", 0x23: "FACE_RESET(0x23)",
    0x24: "GET_ALL_USERID(0x24)",
}


def xor_frame(msgid, data):
    """XOR = MsgID ^ Size高 ^ Size低 ^ 所有 Data（手册：除 SyncWord 外全部字节）。"""
    n = len(data)
    x = msgid ^ ((n >> 8) & 0xFF) ^ (n & 0xFF)
    for b in data:
        x ^= b
    return bytes([SYNC0, SYNC1, msgid, (n >> 8) & 0xFF, n & 0xFF]) + bytes(data) + bytes([x])


class FrameParser:
    """与 fm225_proto.c 同构的最小解析器（只解 H>>M 主发帧）。"""

    def __init__(self):
        self.buf = bytearray()
        self.frames = []          # [(msgid, data_bytes)]

    def feed(self, chunk):
        self.buf.extend(chunk)
        out = []
        while True:
            i = self.buf.find(bytes([SYNC0, SYNC1]))
            if i < 0:
                self.buf.clear()
                return out
            if i > 0:
                del self.buf[:i]
            if len(self.buf) < 5:
                return out
            msgid = self.buf[2]
            size = (self.buf[3] << 8) | self.buf[4]
            if len(self.buf) < 5 + size + 1:
                return out
            data = bytes(self.buf[5:5 + size])
            xor = self.buf[5 + size]
            calc = msgid ^ ((size >> 8) & 0xFF) ^ (size & 0xFF)
            for b in data:
                calc ^= b
            if calc == xor:
                out.append((msgid, data))
                self.frames.append((msgid, data))
            del self.buf[:5 + size + 1]


class FakeModule:
    """假模组：按场景决定「答 / 不答 / 什么时候答」。"""

    def __init__(self, scenario):
        self.scenario = scenario
        self.parser = FrameParser()
        self.log = []                 # [(t, cmd)]
        self.enroll_at = None         # 收到 ENROLL 的时刻
        self.replied = False
        self.last_note = 0.0
        self.note_state = 1           # 1 = 未检测到人脸（与手册 NOTE 语义一致）
        self.start = time.time()

    def elapsed(self):
        return time.time() - self.start

    def note_due(self):
        return (time.time() - self.last_note) >= 0.5

    def on_frame(self, msgid, data):
        cmd = msgid
        self.log.append((self.elapsed(), cmd, bytes(data)))
        if cmd == CMD_ENROLL and self.enroll_at is None:
            self.enroll_at = self.elapsed()
            self.last_note = 0.0

    def maybe_emit(self, fd):
        """按场景产生模组→主机的帧。返回 True 表示本场景已可结束。"""
        now = self.elapsed()

        # 录入进行中每 0.5s 上报一条 NOTE 人脸状态（真模组约 2.1 条/秒）
        if self.enroll_at is not None and not self.replied and self.note_due():
            self.last_note = time.time()
            if self.scenario == "ok" and (now - self.enroll_at) > 1.5:
                self.note_state = 0                 # 看见脸了
            payload = bytes([0x01, self.note_state & 0xFF, 0x00]) + bytes(14)
            os.write(fd, xor_frame(MSGID_NOTE, payload))

        if self.enroll_at is None or self.replied:
            return False

        dt = now - self.enroll_at
        if self.scenario == "ok" and dt >= 2.5:
            os.write(fd, xor_frame(MSGID_REPLY, bytes([CMD_ENROLL, 0x00, 0x00, 0x07])))
            self.replied = True
            return True
        if self.scenario == "noinsert" and dt >= 3.0:
            os.write(fd, xor_frame(MSGID_REPLY, bytes([CMD_ENROLL, 0x00, 0x00, 0x08])))
            self.replied = True
            return True
        if self.scenario == "timeout" and dt >= 10.0:
            # 真机故障现场：MR_FAILED4_TIMEOUT = 0x0D
            os.write(fd, xor_frame(MSGID_REPLY, bytes([CMD_ENROLL, 0x0D])))
            self.replied = True
            return True
        if self.scenario == "silent":
            return False              # 一条都不回：模拟「被抢占后静默无应答」
        return False


def start_socat():
    for p in (HOST_LINK, MOD_LINK):
        if os.path.lexists(p):
            os.unlink(p)
    proc = subprocess.Popen(
        ["socat", "-d", "-d",
         "pty,raw,echo=0,link=%s" % HOST_LINK,
         "pty,raw,echo=0,link=%s" % MOD_LINK],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    for _ in range(200):
        if os.path.lexists(HOST_LINK) and os.path.lexists(MOD_LINK):
            time.sleep(0.1)          # 等 socat 真正进入 data loop
            return proc
        if proc.poll() is not None:  # 提前退出 = 起不来，把原因打出来
            err = proc.stderr.read().decode("utf-8", "replace")
            print("  socat 启动失败：%s" % err.strip())
            return proc
        time.sleep(0.05)
    print("  socat 超时未建立 pty 对")
    return proc


def run_scenario(scenario):
    print("\n" + "=" * 66)
    print("场景 %s" % scenario)
    print("=" * 66)

    socat = start_socat()
    if not (os.path.exists(HOST_LINK) and os.path.exists(MOD_LINK)):
        print("  FAIL socat 未能建立 pty 对（装了 socat 吗？）")
        socat.terminate()
        return False

    env = dict(os.environ)
    env["SAFE_FM225_DEV"] = HOST_LINK
    env["FM225_ENROLL_LIVE"] = "1"
    if not os.path.exists(test_bin()):
        print("  FAIL 找不到 %s（先构建：cmake --build build_pc --target test_fm225_enroll）"
              % test_bin())
        socat.terminate()
        return False
    host = subprocess.Popen([test_bin()], env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    fd = os.open(MOD_LINK, os.O_RDWR | os.O_NOCTTY)
    os.set_blocking(fd, False)

    mod = FakeModule(scenario)
    deadline = time.time() + 30
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try:
                chunk = os.read(fd, 4096)
            except BlockingIOError:
                chunk = b""
            if chunk:
                for msgid, data in mod.parser.feed(chunk):
                    mod.on_frame(msgid, data)
        mod.maybe_emit(fd)
        if host.poll() is not None:
            break

    if host.poll() is None:
        host.send_signal(signal.SIGKILL)
    out = host.stdout.read().decode("utf-8", "replace")
    host.stdout.close()
    host.wait()
    os.close(fd)
    socat.terminate()
    try:
        socat.wait(timeout=5)
    except Exception:
        socat.kill()

    for line in out.strip().splitlines():
        if "[LIVE]" in line or "[fm225]" in line:
            print("  | %s" % line)

    return verdict(scenario, mod, out)


def verdict(scenario, mod, out):
    ok = True
    enroll_cmds = [t for (t, c, _) in mod.log if c == CMD_ENROLL]
    resets = [t for (t, c, _) in mod.log if c == CMD_RESET]
    face_resets = [t for (t, c, _) in mod.log if c == CMD_FACE_RESET]
    queries = [t for (t, c, _) in mod.log if c == CMD_GET_ALL_USERID]

    print("  -- 主机下发帧时序 --")
    for (t, c, _) in mod.log:
        print("     %7.2fs  %s" % (t, CMD_NAME.get(c, "0x%02X" % c)))

    def chk(cond, msg):
        nonlocal ok
        print("  %s %s" % ("PASS" if cond else "FAIL", msg))
        if not cond:
            ok = False

    chk(len(enroll_cmds) == 1, "ENROLL 只下发 1 条（无 REARM 重发），实际 %d 条" % len(enroll_cmds))

    if scenario == "ok":
        chk(re.search(r"ENROLL_DONE err=0 ", out) is not None, "主机上报 ENROLL_DONE(err=0, uid=7)")
        chk(len(face_resets) == 0, "录入成功**未**发 0x23 FACE RESET（避免清掉刚写入的模板）")
    elif scenario == "noinsert":
        if mod.enroll_at is not None:
            win = [t for t in queries if mod.enroll_at <= t <= (mod.enroll_at + 3.0)]
            chk(not win, "录入期间未插入 0x24 对账查询（实际 %d 条）" % len(win))
        chk(re.search(r"ENROLL_DONE err=0 ", out) is not None, "主机上报 ENROLL_DONE(err=0)")
    elif scenario == "timeout":
        chk(re.search(r"ENROLL_DONE err=-12 ", out) is not None,
            "主机把 mr=0x0D(FAILED4_TIMEOUT) 上报成 err=-12 超时")
        chk(len(face_resets) == 1, "录入失败后发了 1 条 0x23 FACE RESET 清残留状态")
    elif scenario == "silent":
        chk(re.search(r"ENROLL_DONE err=-12 ", out) is not None,
            "模组全程不答 → 主机在会话超时点自己兜底上报 err=-12（未永久挂起）")
        chk(len(resets) >= 1, "放弃后发了 0x10 MID_RESET 把模组拉回 STANDBY")
        chk("未挂起" in out, "主机进程正常退出（未卡在等待）")

    print("  ==> 场景 %s：%s" % (scenario, "PASS" if ok else "FAIL"))
    return ok


ALL = ["ok", "noinsert", "timeout", "silent"]


def main():
    if not os.path.exists(test_bin()):
        print("先构建：cmake --build build_pc --target test_fm225_enroll")
        return 2
    scenarios = sys.argv[1:] or ALL
    for s in scenarios:
        if s not in ALL:
            print("未知场景 %s（可选：%s）" % (s, " ".join(ALL)))
            return 2
    results = {s: run_scenario(s) for s in scenarios}
    print("\n" + "=" * 66)
    for s in scenarios:
        print("  %-10s %s" % (s, "PASS" if results[s] else "FAIL"))
    print("=" * 66)
    return 0 if all(results.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
