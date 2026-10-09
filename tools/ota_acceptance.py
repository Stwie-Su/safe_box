#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""FR-29 OTA 期①「接收 / 校验 / 落盘」无头验收（需求规约 §10 集成层）。

为什么要有这个东西：
    OTA 原本只能在真机板子上手测一次（tools/ota_check.sh / ota_apply.sh 把
    /tmp、/var/ota、/usr/share/lvgl、/etc/safe/ota.key 全部写死），PC / VM 上
    跑就会往真实系统路径写文件 —— 于是「验收」这件事实际上**不可复现**，只能
    靠某次上板手测的结论。需求 §10 却把三条验收写成了硬要求。
    2026-10-09 给两个脚本加了路径 env 覆盖（默认值与板端一致，板端行为零变化），
    本脚本据此在临时目录里跑完整链路，把它变成一条任何人都能复跑的验收。

覆盖的验收点（对应需求 §10「OTA 升级状态机与校验（FR-29）」）：
    1. 正常升级：check → CHECKED；apply → INSTALLED，版本文件落对，包已清理；
    2. 摘要错误   → ERR_SHA，且**不留残余**（ota_pkg / ota_x 均已删）；
    3. 签名错误   → ERR_HMAC，同样不留残余；
    4. HMAC 字段缺失（剥字段降级攻击） → ERR_HMAC；
    5. 版本不符（包内 ota_version 与 manifest 声明不一致）→ ERR_VERSION，不留残余；
    6. 断点续传：预置半包残片 → 续传（服务端收到 Range 请求）→ 终验 sha 正确；
    7. 断电模拟：下载中途 kill -9 wget → 残片保留 → 重跑自动续传完成。

只依赖 python3 标准库 + wget + tar + sha256sum（与板端最小工具集对齐）。
退出码 0 = 全部通过。
"""
import hashlib
import hmac as hmac_mod
import json
import os
import shutil
import signal
import subprocess
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
CHECK_SH = os.path.join(HERE, "ota_check.sh")
APPLY_SH = os.path.join(HERE, "ota_apply.sh")
HMAC_PY = os.path.join(HERE, "ota_hmac.py")

WORK = "/tmp/ota_acceptance"
SRV = os.path.join(WORK, "srv")          # HTTP 根：manifest.json + 包
TMPD = os.path.join(WORK, "tmp")         # SAFE_OTA_TMP
OTAD = os.path.join(WORK, "ota")         # SAFE_OTA_DIR
INST = os.path.join(WORK, "install")     # SAFE_OTA_INSTALL_DIR
KEY = os.path.join(WORK, "ota.key")

FAILS = []
RANGE_SEEN = []      # 服务端收到的 Range 请求（证明真的走了续传）
SLOW = {"on": False}


# ---------------- 断言 ----------------
def check(cond, what):
    if cond:
        print("  ok   %s" % what)
    else:
        print("  FAIL %s" % what)
        FAILS.append(what)
    return cond


# ---------------- 带 Range 的静态服务器 ----------------
class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        name = self.path.lstrip("/").split("?")[0]
        path = os.path.join(SRV, name)
        if not os.path.isfile(path):
            self.send_response(404)
            self.end_headers()
            return
        size = os.path.getsize(path)
        rng = self.headers.get("Range")
        start, end = 0, size - 1
        code = 200
        if rng and rng.startswith("bytes="):
            RANGE_SEEN.append(rng)
            try:
                s, _, e = rng[6:].partition("-")
                start = int(s) if s else 0
                end = int(e) if e else size - 1
                code = 206
            except ValueError:
                start, end, code = 0, size - 1, 200
        self.send_response(code)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(end - start + 1))
        if code == 206:
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
        self.end_headers()
        try:
            with open(path, "rb") as f:
                f.seek(start)
                left = end - start + 1
                while left > 0:
                    chunk = f.read(min(65536, left))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    left -= len(chunk)
                    if SLOW["on"]:
                        time.sleep(0.05)
        except (BrokenPipeError, ConnectionResetError):
            pass


def start_server():
    httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    t = __import__("threading").Thread(target=httpd.serve_forever, daemon=True)
    t.start()
    return httpd, "http://127.0.0.1:%d" % httpd.server_address[1]


# ---------------- 造包 ----------------
def make_pkg(version, size, fill=b"X"):
    """造一个纯 tar 包（含 lvglsim + ota_version），返回 (路径, sha256, size, hmac)。"""
    stage = os.path.join(WORK, "stage")
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(stage)
    with open(os.path.join(stage, "lvglsim"), "wb") as f:
        f.write(fill * size)
    with open(os.path.join(stage, "ota_version"), "w") as f:
        f.write(version + "\n")
    pkg = os.path.join(SRV, "safe_ota_%s.tar" % version)
    subprocess.run(["tar", "-cf", pkg, "-C", stage, "lvglsim", "ota_version"], check=True)
    with open(pkg, "rb") as f:
        data = f.read()
    sha = hashlib.sha256(data).hexdigest()
    with open(KEY, "rb") as f:
        k = f.read().strip()
    hm = hmac_mod.new(k, data, hashlib.sha256).hexdigest()
    return pkg, sha, len(data), hm


def write_manifest(version, filename, sha, size, hm, drop_hmac=False):
    m = {"version": version, "file": filename, "sha256": sha, "size": size}
    if not drop_hmac:
        m["hmac"] = hm
    # ★ 必须紧凑格式（无空格）：板端脚本用 `grep -o '"file":"[^"]*"'` 取值，
    #   json.dump 默认在 ":" 后加空格会让它**匹配不到** → 直接 ERR_ARGS。
    #   这里与 tools/make_ota_pkg.sh 的 printf 输出形态保持一致（契约的一部分）。
    with open(os.path.join(SRV, "manifest.json"), "w") as f:
        json.dump(m, f, separators=(",", ":"))


def status():
    """读状态文件（$SAFE_OTA_DIR/status 优先，check 写在 TMP 下）并解析成 dict。"""
    out = {}
    # 顺序有意义：check 写 TMP/ota_status、apply 写 OTAD/status，**后者覆盖前者**。
    # 反过来的话，上一轮 check 留下的 CHECKED 会把本次 apply 的终态盖掉，
    # 表现为「apply 明明失败了，读出来还是 CHECKED」。
    for p in (os.path.join(TMPD, "ota_status"), os.path.join(OTAD, "status")):
        if os.path.isfile(p):
            with open(p) as f:
                for line in f:
                    line = line.strip()
                    if "=" in line:
                        k, _, v = line.partition("=")
                        out[k] = v       # 重写式：后出现覆盖（最后一版语义）
    return out


def run(sh, *args):
    env = dict(os.environ,
               SAFE_OTA_TMP=TMPD, SAFE_OTA_DIR=OTAD,
               SAFE_OTA_HMAC_PY=HMAC_PY, SAFE_OTA_KEY=KEY,
               SAFE_OTA_INSTALL_DIR=INST, SAFE_OTA_NO_RESTART="1")
    return subprocess.run(["sh", sh, *args], env=env,
                          capture_output=True, text=True, timeout=180)


def reset_tmp():
    shutil.rmtree(TMPD, ignore_errors=True)
    os.makedirs(TMPD, exist_ok=True)


# ★ 每个用例的顺序必须是「造包 → 写清单 → check → apply」：
#   apply 的 file / sha256 / size 缺省从 $TMP/ota_manifest.json 读，而那份清单是
#   **check 下载**的。只跑 apply 会读到空（→ ERR_ARGS），更糟的是读到上一轮遗留的
#   旧清单（→ 拿旧 sha 去校新包，验收结论直接失真）。真实链路是 check → apply。

def residual():
    return [p for p in (os.path.join(TMPD, "ota_pkg"), os.path.join(TMPD, "ota_x"))
            if os.path.exists(p)]


# ---------------- 用例 ----------------
def case_ok(url):
    print("[1] 正常升级")
    reset_tmp()
    pkg, sha, size, hm = make_pkg("1.0.7", 300 * 1024)
    write_manifest("1.0.7", os.path.basename(pkg), sha, size, hm)
    run(CHECK_SH, url, "1.0.0")   # ★ 清单就位后再 check：apply 读的就是这份
    st = status()          # 上面刚跑完 check
    check(st.get("STATE") == "CHECKED", "check → CHECKED（实得 %s）" % st.get("STATE"))
    check(st.get("SERVER_VERSION") == "1.0.7", "check 解析出 SERVER_VERSION=1.0.7")
    r = run(APPLY_SH, url)
    st = status()
    check(st.get("STATE") == "INSTALLED", "apply → INSTALLED（实得 %s, rc=%d）" % (st.get("STATE"), r.returncode))
    with open(os.path.join(INST, "ota_version")) as f:
        check(f.read().strip() == "1.0.7", "安装目录版本文件 = 1.0.7")
    check(os.path.isfile(os.path.join(INST, "lvglsim")), "新二进制已落位")
    check(not residual(), "成功后无残余（ota_pkg / ota_x 已清）")


def case_bad_sha(url):
    print("[2] 摘要错误 → ERR_SHA + 无残余")
    reset_tmp()
    pkg, sha, size, hm = make_pkg("1.0.8", 200 * 1024, fill=b"A")
    write_manifest("1.0.8", os.path.basename(pkg), "0" * 64, size, hm)
    run(CHECK_SH, url, "1.0.0")   # ★ 清单就位后再 check：apply 读的就是这份
    run(APPLY_SH, url)
    st = status()
    check(st.get("STATE") == "ERR_SHA", "状态 = ERR_SHA（实得 %s）" % st.get("STATE"))
    check(not residual(), "ERR_SHA 后无残余")


def case_bad_hmac(url):
    print("[3] 签名错误 → ERR_HMAC + 无残余")
    reset_tmp()
    pkg, sha, size, hm = make_pkg("1.0.9", 200 * 1024, fill=b"B")
    write_manifest("1.0.9", os.path.basename(pkg), sha, size, "f" * 64)
    run(CHECK_SH, url, "1.0.0")   # ★ 清单就位后再 check：apply 读的就是这份
    run(APPLY_SH, url)
    st = status()
    check(st.get("STATE") == "ERR_HMAC", "状态 = ERR_HMAC（实得 %s）" % st.get("STATE"))
    check(not residual(), "ERR_HMAC 后无残余")


def case_no_hmac(url):
    print("[4] 剥掉 hmac 字段（降级攻击）→ ERR_HMAC")
    reset_tmp()
    pkg, sha, size, hm = make_pkg("1.1.0", 200 * 1024, fill=b"C")
    write_manifest("1.1.0", os.path.basename(pkg), sha, size, hm, drop_hmac=True)
    run(CHECK_SH, url, "1.0.0")   # ★ 清单就位后再 check：apply 读的就是这份
    run(APPLY_SH, url)
    check(status().get("STATE") == "ERR_HMAC", "状态 = ERR_HMAC（实得 %s）" % status().get("STATE"))


def case_version_mismatch(url):
    print("[5] 版本不符（包内 9.9.9 vs 清单 1.1.1）→ ERR_VERSION + 无残余")
    reset_tmp()
    pkg, sha, size, hm = make_pkg("9.9.9", 200 * 1024, fill=b"D")
    # 清单故意声明成另一个版本：sha / hmac 都基于真包算，故只有版本这一关会被卡住
    write_manifest("1.1.1", os.path.basename(pkg), sha, size, hm)
    run(CHECK_SH, url, "1.0.0")   # ★ 清单就位后再 check：apply 读的就是这份
    run(APPLY_SH, url)
    st = status()
    check(st.get("STATE") == "ERR_VERSION", "状态 = ERR_VERSION（实得 %s）" % st.get("STATE"))
    check(st.get("PKG_VERSION") == "9.9.9", "状态里带出包内版本 9.9.9")
    check(not residual(), "ERR_VERSION 后无残余")


def case_resume(url):
    print("[6] 断点续传（预置半包残片）")
    reset_tmp()
    pkg, sha, size, hm = make_pkg("1.2.0", 400 * 1024, fill=b"E")
    write_manifest("1.2.0", os.path.basename(pkg), sha, size, hm)
    run(CHECK_SH, url, "1.0.0")   # ★ 清单就位后再 check：apply 读的就是这份
    with open(pkg, "rb") as f:
        data = f.read()
    half = os.path.join(TMPD, "ota_pkg")
    with open(half, "wb") as f:
        f.write(data[:size // 2])          # 只剩前半截，且未达包大小
    RANGE_SEEN.clear()
    run(APPLY_SH, url)
    st = status()
    check(st.get("STATE") == "INSTALLED", "续传后成功安装（实得 %s）" % st.get("STATE"))
    check(any(r.startswith("bytes=") for r in RANGE_SEEN),
          "服务端收到 Range 请求（续传而非重下）：%s" % (RANGE_SEEN[:2] or "无"))
    with open(pkg, "rb") as f:
        check(hashlib.sha256(f.read()).hexdigest() == sha, "原包未被续传写坏")


def case_power_cut(url):
    print("[7] 断电模拟（下载中 kill -9 wget → 重跑续传）")
    reset_tmp()
    pkg, sha, size, hm = make_pkg("1.3.0", 8 * 1024 * 1024, fill=b"F")
    write_manifest("1.3.0", os.path.basename(pkg), sha, size, hm)
    run(CHECK_SH, url, "1.0.0")   # ★ 清单就位后再 check：apply 读的就是这份

    SLOW["on"] = True
    env = dict(os.environ, SAFE_OTA_TMP=TMPD, SAFE_OTA_DIR=OTAD,
               SAFE_OTA_HMAC_PY=HMAC_PY, SAFE_OTA_KEY=KEY,
               SAFE_OTA_INSTALL_DIR=INST, SAFE_OTA_NO_RESTART="1")
    p = subprocess.Popen(["sh", APPLY_SH, url], env=env,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2.5)
    killed = 0
    try:
        out = subprocess.run(["ps", "-eo", "pid,args"], capture_output=True, text=True).stdout
        for line in out.splitlines():
            if "ota_pkg" in line and "wget" in line:
                pid = int(line.split()[0])
                os.kill(pid, signal.SIGKILL)
                killed += 1
    except (ValueError, ProcessLookupError, PermissionError):
        pass
    p.wait(timeout=120)
    SLOW["on"] = False

    part = os.path.join(TMPD, "ota_pkg")
    partial = os.path.getsize(part) if os.path.isfile(part) else 0
    check(killed > 0, "确实 kill 掉了下载进程（%d 个）" % killed)
    check(0 < partial < size, "残片保留且未下完：%d / %d 字节" % (partial, size))
    check(status().get("STATE") == "ERR_DOWNLOAD",
          "中断后 = ERR_DOWNLOAD 且**保留残片**（实得 %s）" % status().get("STATE"))

    RANGE_SEEN.clear()
    run(APPLY_SH, url)                     # 重新上电：应自动续传补完
    st = status()
    check(st.get("STATE") == "INSTALLED", "重跑后续传完成并安装（实得 %s）" % st.get("STATE"))
    check(any(r.startswith("bytes=") for r in RANGE_SEEN), "重跑走的是续传（Range）")


def main():
    for f in (CHECK_SH, APPLY_SH, HMAC_PY):
        if not os.path.isfile(f):
            print("缺少脚本: %s" % f)
            return 1

    shutil.rmtree(WORK, ignore_errors=True)
    for d in (SRV, TMPD, OTAD, INST):
        os.makedirs(d, exist_ok=True)
    # 与 make_ota_pkg.sh 同形态的密钥：64 字符 hex 文本
    with open(KEY, "w") as f:
        f.write(hashlib.sha256(b"acceptance").hexdigest() * 2)
    # 安装目录里先放一个「旧版本」，才能验证 INSTALL 真的落了新东西
    with open(os.path.join(INST, "lvglsim"), "w") as f:
        f.write("old")
    with open(os.path.join(INST, "ota_version"), "w") as f:
        f.write("1.0.0\n")

    httpd, url = start_server()
    try:
        case_ok(url)
        case_bad_sha(url)
        case_bad_hmac(url)
        case_no_hmac(url)
        case_version_mismatch(url)
        case_resume(url)
        case_power_cut(url)
    finally:
        httpd.shutdown()

    print("")
    if FAILS:
        print("[FAIL] %d 项未过：" % len(FAILS))
        for f in FAILS:
            print("   - %s" % f)
        return 1
    print("[PASS] OTA 期① 无头验收全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
