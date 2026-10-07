#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""无头端到端验收（FR-11/FR-12 + FR-25 降级为明文通道的连通性）。

判据（对应规约 §10「集成·无头环境端到端」）：
  1. 应用能连上本地 broker 并上送 safe/status（含 fw 版本号）；
  2. 下发 query_status 能拿到 req_id 一致的回执（请求-响应关联）；
  3. kill -9 应用后 broker 侧出现 LWT（online:0）—— 断线可被对端感知。

为什么用**明文**而不是 8883 TLS：
  本验收要验的是「远程通道的端到端闭环」（连接/订阅/指令/回执/LWT），
  不是加密本身——加密由 FR-25 的实机验收与 tls_stream 负责。明文让这条
  用例在 PC 无头环境可确定性运行，不依赖证书、端口转发与 broker 配置。

用法: e2e_headless.py [--app <lvglsim路径>] [--port <broker端口>]
退出码 0 = 全部通过。
"""
import argparse
import json
import os
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def log(msg):
    sys.stdout.write("[e2e] %s\n" % msg)
    sys.stdout.flush()


def start_broker(port):
    conf = "/tmp/e2e_mosq_%d.conf" % port
    with open(conf, "w") as f:
        f.write("listener %d\nallow_anonymous true\n" % port)

    p = subprocess.Popen(["mosquitto", "-c", conf],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(40):
        try:
            subprocess.run(
                ["mosquitto_pub", "-h", "127.0.0.1", "-p", str(port),
                 "-t", "safe/probe", "-m", "x"],
                check=True, capture_output=True, timeout=3)
            log("broker 就绪 port=%d" % port)
            return p
        except Exception:
            time.sleep(0.25)
    log("FAIL: broker 启动超时")
    p.kill()
    return None


def ensure_xvfb():
    disp = os.environ.get("E2E_DISPLAY", ":99")
    r = subprocess.run(["pgrep", "-a", "Xvfb"], capture_output=True, text=True)
    if disp in (r.stdout or ""):
        return disp, None
    p = subprocess.Popen(["Xvfb", disp, "-screen", "0", "1280x800x24"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.5)
    return disp, p


def wait_msg(port, topic, timeout, match=None):
    """订阅直到收到一条（可选 match 过滤），返回 payload 字符串。"""
    deadline = time.time() + timeout
    cmd = ["mosquitto_sub", "-h", "127.0.0.1", "-p", str(port),
           "-t", topic, "-C", "1", "-W", str(int(timeout))]
    while time.time() < deadline:
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout + 2)
            out = (r.stdout or "").strip()
            if out and (match is None or match in out):
                return out
        except Exception:
            pass
        time.sleep(0.3)
    return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--app", default=os.path.join(ROOT, "build_pc", "bin", "lvglsim"))
    ap.add_argument("--port", type=int, default=11883)
    a = ap.parse_args()

    if not os.path.exists(a.app):
        log("FAIL: 找不到应用二进制 %s（先构建 build_pc）" % a.app)
        return 1

    disp, xvfb = ensure_xvfb()
    broker = start_broker(a.port)
    if broker is None:
        return 1

    env = dict(os.environ)
    env["DISPLAY"] = disp
    env["SAFE_MQTT_HOST"] = "127.0.0.1"
    env["SAFE_MQTT_PORT"] = str(a.port)
    env["SAFE_MQTT_TLS"] = "0"
    env.pop("SAFE_MQTT_CAFILE", None)
    env.pop("SAFE_MQTT_CERT", None)
    env.pop("SAFE_MQTT_KEY", None)
    env["SAFE_TEST_PAGE"] = "HOME"
    env["SAFE_PERF_LOG"] = "0"

    fails = []

    # ---- 判据 1：连上并上送 status ----
    app = subprocess.Popen([a.app], env=env, cwd=os.path.dirname(a.app),
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    status = wait_msg(a.port, "safe/status", timeout=20, match="fw")
    if status:
        log("PASS 1 上送状态: %s" % status[:90])
    else:
        fails.append("1 未收到 safe/status（应用未连上 broker）")

    # ---- 判据 2：下发 query_status 拿回执 ----
    time.sleep(1.0)
    rid = "e2eq1"
    sub = subprocess.Popen(
        ["mosquitto_sub", "-h", "127.0.0.1", "-p", str(a.port), "-t", "safe/log", "-v"],
        stdout=open("/tmp/e2e_log.txt", "w"), stderr=subprocess.DEVNULL)
    time.sleep(0.6)
    subprocess.run(["mosquitto_pub", "-h", "127.0.0.1", "-p", str(a.port),
                    "-t", "safe/cmd", "-q", "1",
                    "-m", json.dumps({"cmd": "query_status", "req_id": rid})],
                   capture_output=True, timeout=5)
    time.sleep(3.0)
    sub.terminate()
    try:
        body = open("/tmp/e2e_log.txt").read()
    except Exception:
        body = ""
    if rid in body:
        log("PASS 2 回执关联: req_id=%s" % rid)
    else:
        fails.append("2 未收到 req_id=%s 的回执" % rid)

    # ---- 判据 3：kill -9 后 LWT 出现 ----
    os.kill(app.pid, signal.SIGKILL)
    lwt = wait_msg(a.port, "safe/status", timeout=10, match='"online":0')
    if lwt:
        log("PASS 3 LWT 生效: %s" % lwt[:90])
    else:
        fails.append("3 kill -9 后未观察到 LWT（online:0）")

    # 清理
    try:
        os.kill(app.pid, 0)
        app.kill()
    except Exception:
        pass
    broker.terminate()
    try:
        broker.wait(timeout=5)
    except Exception:
        broker.kill()
    if xvfb is not None:
        xvfb.terminate()

    if fails:
        log("FAIL 项：")
        for f in fails:
            log("  - " + f)
        return 1
    log("全部通过（3 项判据）")
    return 0


if __name__ == "__main__":
    sys.exit(main())

