#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""板上服务发现：在同一网段找出「能提供本系统远程通道」的代理主机。

为什么需要它
------------
板子与代理主机（PC）都连手机热点 / 校园网 DHCP，地址会漂（实测
10.218.148.51 -> 10.163.32.202 -> 10.163.32.51）。连接字符串写死 IP 时，
换一次网络就永久断线且零报错——表象像「MQTT 崩了」，实际只是连错机器。

判据（比 ping 更强）
--------------------
1. TCP 8883 能握手；
2. 用板端 CA 校验证书链（CERT_REQUIRED + ca_certs）。

两条都满足才认，避免扫到网段里随便一个开 8883 的机器。

用法
----
    find_broker.py --ca <ca.crt> [--cert <device.crt>] [--key <device.key>]
                   [--port 8883] [--hint <上次成功的IP>] [--timeout 1.0]
                   [--no-scan]

stdout 只输出一行 IP（便于 shell 取值），失败无输出并退 1。

★ 与 D23 同一条纪律：端口转发 / 防火墙 / 热点属于「实验环境」变量，
  设备侧要能自愈，而不是等人工改配置。
"""
import argparse
import ipaddress
import os
import socket
import ssl
import subprocess
import sys
import threading


def tls_ok(ip, port, ctx, timeout):
    """TCP 连上 + 用 CA 校验证书链。"""
    s = socket.socket()
    s.settimeout(timeout)
    try:
        s.connect((ip, port))
        t = ctx.wrap_socket(s)
        try:
            t.getpeercert()
            t.close()
        except Exception:
            pass
        return True, None
    except Exception as e:
        try:
            s.close()
        except Exception:
            pass
        return False, e


def local_nets():
    """本机 IPv4 网段列表（排除 lo 与链路本地 169.254）。"""
    nets = []
    # 首选：ip 命令（BusyBox 可能没有）
    try:
        out = subprocess.check_output(["ip", "-4", "addr"], text=True)
        cur = None
        for line in out.splitlines():
            if line and not line.startswith(" "):
                cur = line.split(":")[1].strip()
            elif line.strip().startswith("inet "):
                parts = line.split()
                cidr = parts[1]
                if cidr.startswith("169.254") or cur == "lo":
                    continue
                nets.append((cur, ipaddress.ip_network(cidr, strict=False)))
        if nets:
            return nets
    except Exception:
        pass
    # 兜底：ifconfig
    try:
        out = subprocess.check_output(["ifconfig"], text=True)
        cur = None
        for line in out.splitlines():
            if line and not line.startswith(" "):
                cur = line.split()[0]
            elif "inet addr:" in line:
                ip = line.split("inet addr:")[1].split()[0]
                mask = line.split("Mask:")[1].split()[0]
                if ip.startswith("169.254") or ip == "127.0.0.1" or cur == "lo":
                    continue
                nets.append((cur, ipaddress.ip_network(
                    "%s/%s" % (ip, mask), strict=False)))
    except Exception:
        pass
    return nets


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ca", required=True)
    ap.add_argument("--cert", default="")
    ap.add_argument("--key", default="")
    ap.add_argument("--port", type=int, default=8883)
    ap.add_argument("--hint", default="")
    ap.add_argument("--timeout", type=float, default=1.0)
    a = ap.parse_args()

    def log(m):
        sys.stderr.write("[findbroker] %s\n" % m)

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    try:
        ctx.load_verify_locations(cafile=a.ca)
        ctx.verify_mode = ssl.CERT_REQUIRED
    except Exception as e:
        log("CA 加载失败：%r" % (e,))
    if a.cert and a.key:
        try:
            ctx.load_cert_chain(a.cert, a.key)
        except Exception:
            pass

    if a.hint:
        ok, _ = tls_ok(a.hint, a.port, ctx, a.timeout)
        if ok:
            log("hint %s 命中" % a.hint)
            print(a.hint)
            return 0
        log("hint %s 不可用" % a.hint)

    # 扫描同网段
    out = subprocess.run(["ip", "-4", "-o", "addr"], capture_output=True, text=True)
    nets = []
    for line in (out.stdout or "").splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[1] not in ("lo",):
            try:
                nets.append((parts[1], ipaddress.ip_network(parts[3], strict=False)))
            except Exception:
                pass
    if not nets:
        log("无法获取本机网段")
        return 1

    result = []
    lock = threading.Lock()
    sem = threading.Semaphore(64)

    def probe(ip):
        with sem:
            if result:
                return
            ok, _ = tls_ok(ip, a.port, ctx, a.timeout)
            if ok:
                with lock:
                    if not result:
                        result.append(ip)

    ts = []
    for ifname, net in nets:
        for h in net.hosts():
            t = threading.Thread(target=probe, args=(str(h),), daemon=True)
            t.start()
            ts.append(t)
        for t in ts:
            t.join()
        if result:
            break

    if result:
        log("discovered %s" % result[0])
        print(result[0])
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())

