#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
mqtt_lab.py —— 零依赖 MQTT 3.1.1 实验台（纯 socket，只用 Python 3 标准库）

改造自 safe_remote_test.py（沿用它的 enc_len / read_packet 思路），
去掉了硬编码的 Windows 输出路径，补上「假 broker」与「故障注入」能力。

---------------- 为什么必须有一个可控对端 ----------------
真 broker（mosquitto）是"正确"的：它永远按规范回 CONNACK(0)、回 SUBACK、
回 PINGRESP。而本项目要验的恰好全是**异常路径** ——
CONNACK 返回 4/5（凭据错）、SUBACK 返回 0x80（ACL 拒）、不回 SUBACK、
不回 PINGRESP、主动断链、注入超长/畸形报文……这些在真 broker 上**造不出来**。
没有可控对端，这些路径就只能靠读代码"觉得对"，可复现性为零。

---------------- 为什么坚持零依赖 ----------------
* 只 import 标准库（argparse/json/os/socket/struct/sys/time）：
  VM 上 `python3 tools/mqtt_lab.py` 直接跑，不用 pip install、不用虚拟环境，
  换一台机器/换一块板子照样能跑 —— 验证工具自己不能成为新的环境依赖。
* 直接用 socket 拼 MQTT 报文而不是用 paho：
  这样才能在**字节层面**看到设备到底发了什么（DUP 位、packet id、remaining
  length 编码、有没有 DISCONNECT）。封装库会把这些细节藏起来。

---------------- 三种模式 ----------------
  # 1) 假 broker：设备连过来，逐字节打印它发出的每一个报文
  python3 tools/mqtt_lab.py broker --port 1890 [--connack 0] [--no-puback]
                            [--no-pingresp] [--no-suback] [--suback-rc 128]
                            [--drop-after N] [--send-json '...'] [--send-big N]
  # 2) 观察者：连真 broker 订阅并打印（看上行事件/回执）
  python3 tools/mqtt_lab.py sub --host 127.0.0.1 --port 1883 [--secs 20] [--topic 'safe/#']
  # 3) 指令方：发一条 safe/cmd 并等回执
  python3 tools/mqtt_lab.py pub --host 127.0.0.1 --port 1883 \
          --json '{"cmd":"query_status","req_id":"r1"}'

---------------- 真实用例（本仓库修 MQTT 缺陷时实际用过的命令行）----------------

  ★ 用例 A：CONNACK 永久错误 → 量 net 线程是不是在空转（缺陷 #10）
      1) python3 tools/mqtt_lab.py broker --port 1893 --connack 5 --idle 120
      2) SAFE_DATA_DIR=/tmp/labdata DISPLAY=:99 \
         SAFE_MQTT_HOST=127.0.0.1 SAFE_MQTT_PORT=1893 \
         timeout 60 stdbuf -oL -eL ./build_pc/bin/lvglsim
      3) 另开窗口：ps -L -p <pid> -o tid,comm,pcpu   → 看 safe-mqtt-net 的 %CPU
      判据：修复前 100%（poll 忙轮询），修复后 0%。完整采样脚本见 tools/lab_cpu.sh。

  ★ 用例 B：SUBACK 被拒 → 数 30 秒内重连了几次（缺陷 #11）
      1) python3 tools/mqtt_lab.py broker --port 1894 --suback-rc 128 --idle 120
      2) 同上把设备指到 1894
      3) 数 broker 日志里 ">>> 设备已连接" 的次数
      判据：修复前 30s 内 ~70 次（退避被 CONNACK 归零），修复后 7 次（爬到 30s 封顶）。
      完整统计脚本见 tools/lab_reconnect.sh。

  ★ 用例 C：broker 不回 SUBACK → 设备必须自愈，不能"在线但永远失聪"（缺陷 #12）
      python3 tools/mqtt_lab.py broker --port 1895 --no-suback --idle 120
      判据：设备日志出现「等 SUBACK 超时…已断链重连并重发 SUBSCRIBE」，
            而不是停在 CONNECTED 一动不动。

  ★ 用例 D：注入超长下行（>1KB）→ 触发设备侧断链重连，验证重连后仍收得到指令
      python3 tools/mqtt_lab.py broker --port 1896 --send-big 1200
      再配合 `pub` 模式下发 query_status，看 safe/log 有没有回执。

---------------- 使用注意 ----------------
* 把设备指向本工具：SAFE_MQTT_HOST=<本机IP> SAFE_MQTT_PORT=<--port>
  （PC 端跑 UI 需要 DISPLAY=:99，先 `pgrep -x Xvfb || Xvfb :99 -screen 0 800x480x24 &`）
* **不要**把它放在 VM 的 /tmp 下：/tmp 会被清理，工具中途消失会让验证结果
  看起来像"缺陷没修好"（踩过一次）。本文件已入库，统一用 tools/mqtt_lab.py。
* broker 模式是「我冒充 broker」，能看到设备发出的**每一个字节** ——
  退避曲线、PINGREQ 间隔、DUP 重传、有没有发 DISCONNECT，只有这个模式能测。
* 所有收到的报文都打 hex，便于逐字节核对协议字段。
* 注入类参数（--send-json/--send-big/--send-hex）**每次连接只注入一次**：
  每次重连都注入会把「重连→再被拒」做成死循环，那是工具自己的行为，会污染判据。
"""
import argparse
import json
import os
import socket
import struct
import sys
import time

# ---------------- 报文类型 ----------------
T_CONNECT, T_CONNACK, T_PUBLISH, T_PUBACK = 1, 2, 3, 4
T_PUBREC, T_PUBREL, T_PUBCOMP = 5, 6, 7
T_SUBSCRIBE, T_SUBACK = 8, 9
T_UNSUBSCRIBE, T_UNSUBACK = 10, 11
T_PINGREQ, T_PINGRESP, T_DISCONNECT = 12, 13, 14

NAME = {1: "CONNECT", 2: "CONNACK", 3: "PUBLISH", 4: "PUBACK", 5: "PUBREC",
        6: "PUBREL", 7: "PUBCOMP", 8: "SUBSCRIBE", 9: "SUBACK",
        10: "UNSUBSCRIBE", 11: "UNSUBACK", 12: "PINGREQ", 13: "PINGRESP",
        14: "DISCONNECT"}


def log(*a):
    print(*a, flush=True)


def hexdump(b, limit=64):
    s = " ".join("%02X" % x for x in b[:limit])
    return s + (" ..." if len(b) > limit else "")


# ---------------- 编码 ----------------
def enc_len(n):
    out = bytearray()
    while True:
        b = n % 128
        n //= 128
        out.append(b | (0x80 if n > 0 else 0))
        if n <= 0:
            break
    return bytes(out)


def enc_str(s):
    b = s.encode("utf-8")
    return struct.pack(">H", len(b)) + b


def pkt(ptype, flags, body):
    return bytes([(ptype << 4) | (flags & 0x0F)]) + enc_len(len(body)) + body


def pack_connack(rc, session_present=False):
    return pkt(T_CONNACK, 0, bytes([0x01 if session_present else 0x00, rc]))


def pack_suback(pid, rc):
    return pkt(T_SUBACK, 0, struct.pack(">H", pid) + bytes([rc]))


def pack_pingresp():
    return pkt(T_PINGRESP, 0, b"")


def pack_puback(pid):
    return pkt(T_PUBACK, 0, struct.pack(">H", pid))


def pack_connect(cid, keepalive=60, clean=True):
    vh = enc_str("MQTT") + bytes([0x04, 0x02 if clean else 0x00]) + struct.pack(">H", keepalive)
    return pkt(T_CONNECT, 0, vh + enc_str(cid))


def pack_subscribe(topic, qos, pid):
    return pkt(T_SUBSCRIBE, 0x02, struct.pack(">H", pid) + enc_str(topic) + bytes([qos]))


def pack_publish(topic, payload, qos=0, pid=0, dup=False, retain=False):
    if isinstance(payload, str):
        payload = payload.encode("utf-8")
    flags = ((qos & 0x03) << 1) | (0x08 if dup else 0x00) | (0x01 if retain else 0x00)
    body = enc_str(topic) + (struct.pack(">H", pid) if qos > 0 else b"") + payload
    return pkt(T_PUBLISH, flags, body)


# ---------------- 解码 ----------------
def read_exact(s, n):
    data = b""
    while len(data) < n:
        c = s.recv(n - len(data))
        if not c:
            return None
        data += c
    return data


def read_packet(s):
    """返回 (ptype, flags, body)；None = 连接关闭"""
    hdr = read_exact(s, 1)
    if hdr is None:
        return None
    ptype, flags = hdr[0] >> 4, hdr[0] & 0x0F
    mult, shift = 0, 0
    while True:
        b = read_exact(s, 1)
        if b is None:
            return None
        mult |= (b[0] & 0x7F) << shift
        shift += 7
        if not (b[0] & 0x80):
            break
    body = read_exact(s, mult) if mult else b""
    if body is None:
        return None
    return ptype, flags, body


def u16(b, off):
    return struct.unpack(">H", b[off:off + 2])[0]


def parse_connect(body):
    """返回可读字典（含 LWT / keepalive / clean_session —— 验证这几个概念的关键）"""
    d = {}
    try:
        n = u16(body, 0)
        d["proto"] = body[2:2 + n].decode("utf-8", "replace")
        off = 2 + n
        d["level"] = body[off]
        flags = body[off + 1]
        d["keepalive_s"] = u16(body, off + 2)
        off += 4
        n = u16(body, off)
        d["client_id"] = body[off + 2:off + 2 + n].decode("utf-8", "replace")
        off += 2 + n
        d["clean_session"] = bool(flags & 0x02)
        d["has_will"] = bool(flags & 0x04)
        d["will_qos"] = (flags >> 3) & 0x03
        d["will_retain"] = bool(flags & 0x20)
        if d["has_will"]:
            n = u16(body, off)
            d["will_topic"] = body[off + 2:off + 2 + n].decode("utf-8", "replace")
            off += 2 + n
            n = u16(body, off)
            d["will_msg"] = body[off + 2:off + 2 + n].decode("utf-8", "replace")
            off += 2 + n
        if flags & 0x80:
            n = u16(body, off)
            d["username"] = body[off + 2:off + 2 + n].decode("utf-8", "replace")
            off += 2 + n
    except Exception as e:
        d["parse_error"] = repr(e)
    return d


def parse_publish(flags, body):
    qos = (flags >> 1) & 0x03
    dup = bool(flags & 0x08)
    retain = bool(flags & 0x01)
    n = u16(body, 0)
    topic = body[2:2 + n].decode("utf-8", "replace")
    off = 2 + n
    pid = 0
    if qos > 0:
        pid = u16(body, off)
        off += 2
    return topic, body[off:], pid, qos, dup, retain


# ---------------- 模式 1：假 broker ----------------
def mode_broker(a):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", a.port))
    srv.listen(1)
    log("=== 假 broker 监听 0.0.0.0:%d ===" % a.port)
    log("把设备指过来：SAFE_MQTT_HOST=<本机IP> SAFE_MQTT_PORT=%d" % a.port)
    total_pkts = 0
    injected = [False]      # 注入只做一次：否则「重连→再注入→再被拒」是自己造的循环
    while True:
        log("等待设备连接 ...")
        conn, addr = srv.accept()
        log(">>> 设备已连接：%s:%d" % (addr[0], addr[1]))
        conn.settimeout(a.idle)
        t0 = time.time()
        n = 0
        try:
            while True:
                r = read_packet(conn)
                if r is None:
                    log("<<< 连接关闭（设备断开）")
                    break
                ptype, flags, body = r
                n += 1
                total_pkts += 1
                dt = time.time() - t0
                log("[%7.3fs] #%-3d %-11s flags=0x%X len=%-4d | %s"
                    % (dt, n, NAME.get(ptype, "?"), flags, len(body), hexdump(body)))

                if ptype == T_CONNECT:
                    d = parse_connect(body)
                    log("         CONNECT: client_id=%s keepalive=%ss clean_session=%s"
                        % (d.get("client_id"), d.get("keepalive_s"), d.get("clean_session")))
                    if d.get("has_will"):
                        log("         ★ LWT: topic=%s qos=%d retain=%s msg=%s"
                            % (d.get("will_topic"), d.get("will_qos"),
                               d.get("will_retain"), d.get("will_msg")))
                    conn.sendall(pack_connack(a.connack))
                    log("         -> 回 CONNACK rc=%d%s"
                        % (a.connack, "（非 0：设备应停止重连）" if a.connack else ""))
                    if a.connack != 0:
                        break

                elif ptype == T_SUBSCRIBE:
                    pid = u16(body, 0)
                    tl = u16(body, 2)
                    topic = body[4:4 + tl].decode("utf-8", "replace")
                    qos = body[4 + tl]
                    log("         SUBSCRIBE: topic=%s qos=%d pid=%d" % (topic, qos, pid))
                    if a.no_suback:
                        log("         -> 故意不回 SUBACK（--no-suback）")
                    else:
                        conn.sendall(pack_suback(pid, a.suback_rc))
                        log("         -> 回 SUBACK pid=%d rc=%d" % (pid, a.suback_rc))

                    # 订阅之后注入一条下行报文（畸形/超长/正常指令都走这里）
                    # ★ 只注入一次：每次重连都注入会把「重连→再被拒」做成死循环，
                    #   那是工具自己的行为，不是设备缺陷 —— 会污染判据。
                    if injected[0]:
                        pass
                    elif a.send_hex:
                        injected[0] = True
                        raw = bytes.fromhex(a.send_hex.replace(" ", ""))
                        log("         >>> 注入原始字节 %d 个：%s" % (len(raw), hexdump(raw, 40)))
                        conn.sendall(raw)
                    elif a.send_json:
                        injected[0] = True
                        log("         >>> 注入 safe/cmd：%s" % a.send_json)
                        conn.sendall(pack_publish("safe/cmd", a.send_json, qos=1, pid=1))
                    elif a.send_big:
                        injected[0] = True
                        big = '{"cmd":"query_status","req_id":"big","pad":"' + "A" * a.send_big + '"}'
                        log("         >>> 注入超长 safe/cmd（%d 字节）" % len(big))
                        conn.sendall(pack_publish("safe/cmd", big, qos=1, pid=1))

                elif ptype == T_PUBLISH:
                    topic, payload, pid, qos, dup, retain = parse_publish(flags, body)
                    log("         PUBLISH: topic=%s qos=%d pid=%d DUP=%d retain=%d"
                        % (topic, qos, pid, dup, retain))
                    log("         payload=%s" % payload.decode("utf-8", "replace"))
                    if qos == 1:
                        if a.no_puback:
                            log("         -> 故意不回 PUBACK（--no-puback，等它重传 DUP=1）")
                        else:
                            conn.sendall(pack_puback(pid))
                            log("         -> 回 PUBACK pid=%d" % pid)

                elif ptype == T_PINGREQ:
                    log("         PINGREQ（keepalive 心跳）")
                    if a.no_pingresp:
                        log("         -> 故意不回 PINGRESP")
                    else:
                        conn.sendall(pack_pingresp())
                        log("         -> 回 PINGRESP")

                elif ptype == T_DISCONNECT:
                    log("         ★★★ DISCONNECT：设备优雅退出（B1 修复后必然出现）")
                    break

                if a.drop_after and n >= a.drop_after:
                    log("!!! 达到 --drop-after %d，主动断链（触发设备退避重连）" % a.drop_after)
                    break
        except socket.timeout:
            log("<<< %ds 无报文，断开" % a.idle)
        except Exception as e:
            log("<<< 异常：%r" % (e,))
        try:
            conn.close()
        except Exception:
            pass
        log("=== 本轮共 %d 个报文（累计 %d），继续等待下一次连接 ===" % (n, total_pkts))


# ---------------- 模式 2：观察者 ----------------
def mode_sub(a):
    s = socket.create_connection((a.host, a.port), timeout=8)
    s.sendall(pack_connect("lab-sub-%d" % int(time.time()), keepalive=60))
    p, f, b = read_packet(s)
    if p != T_CONNACK or b[1] != 0:
        log("CONNACK 异常：%s rc=%s" % (NAME.get(p), b[1] if len(b) > 1 else "?"))
        sys.exit(1)
    log("CONNACK rc=0，订阅 %s" % a.topic)
    s.sendall(pack_subscribe(a.topic, 0, 1))
    p, f, b = read_packet(s)
    log("SUBACK rc=%d" % (b[2] if len(b) > 2 else -1))
    log("--- 收听 %ss ---" % a.secs)
    s.settimeout(2)
    end = time.time() + a.secs
    cnt = 0
    while time.time() < end:
        try:
            r = read_packet(s)
        except socket.timeout:
            continue
        if r is None:
            log("连接关闭")
            break
        p, f, b = r
        if p == T_PUBLISH:
            topic, payload, pid, qos, dup, retain = parse_publish(f, b)
            cnt += 1
            log("[%s] %s | qos=%d dup=%d retain=%d | %s"
                % (time.strftime("%H:%M:%S"), topic, qos, dup, retain,
                   payload.decode("utf-8", "replace")))
    log("--- 共 %d 条 ---" % cnt)
    s.close()


# ---------------- 模式 3：指令方 ----------------
def mode_pub(a):
    if a.json:
        payload = a.json
    elif a.file:
        payload = open(a.file, "r", encoding="utf-8").read().strip()
    else:
        payload = json.dumps({"cmd": "query_status",
                              "req_id": "lab-%d" % int(time.time())}, ensure_ascii=False)
    s = socket.create_connection((a.host, a.port), timeout=8)
    s.sendall(pack_connect("lab-pub-%d" % int(time.time()), keepalive=60))
    p, f, b = read_packet(s)
    if p != T_CONNACK or b[1] != 0:
        log("CONNACK 异常 rc=%s" % (b[1] if len(b) > 1 else "?"))
        sys.exit(1)
    s.sendall(pack_subscribe("safe/#", 0, 1))
    read_packet(s)
    log(">>> 下发 safe/cmd: %s" % payload)
    s.sendall(pack_publish("safe/cmd", payload, qos=a.qos, pid=0))
    log("--- 等 %ds 回执 ---" % a.secs)
    s.settimeout(2)
    end = time.time() + a.secs
    while time.time() < end:
        try:
            r = read_packet(s)
        except socket.timeout:
            continue
        if r is None:
            break
        p, f, b = r
        if p == T_PUBLISH:
            topic, pl, pid, qos, dup, retain = parse_publish(f, b)
            log("    [%s] %s" % (topic, pl.decode("utf-8", "replace")))
    s.close()


def main():
    ap = argparse.ArgumentParser(description="零依赖 MQTT 3.1.1 实验台")
    sub = ap.add_subparsers(dest="mode", required=True)

    b = sub.add_parser("broker", help="假 broker：看设备发出的每一个字节")
    b.add_argument("--port", type=int, default=1890)
    b.add_argument("--connack", type=int, default=0, help="回给设备的 CONNACK 码（试 3/4/5）")
    b.add_argument("--no-puback", action="store_true", help="不回 PUBACK → 看它重传 DUP")
    b.add_argument("--no-pingresp", action="store_true", help="不回 PINGRESP → 看它判超时")
    b.add_argument("--no-suback", action="store_true", help="不回 SUBACK")
    b.add_argument("--suback-rc", type=int, default=0, help="SUBACK 的返回码（试 0x80=128）")
    b.add_argument("--drop-after", type=int, default=0, help="收到 N 个报文后主动断链")
    b.add_argument("--send-json", default=None, help="订阅后注入一条 safe/cmd（JSON 字符串）")
    b.add_argument("--send-big", type=int, default=0, help="订阅后注入一条 N 字节的超长 safe/cmd")
    b.add_argument("--send-hex", default=None, help="订阅后注入原始字节（hex），用于畸形报文")
    b.add_argument("--idle", type=int, default=120, help="空闲超时秒数")
    b.set_defaults(func=mode_broker)

    s = sub.add_parser("sub", help="观察者：订阅并打印")
    s.add_argument("--host", default="127.0.0.1")
    s.add_argument("--port", type=int, default=1883)
    s.add_argument("--topic", default="safe/#")
    s.add_argument("--secs", type=int, default=20)
    s.set_defaults(func=mode_sub)

    p = sub.add_parser("pub", help="指令方：发一条 safe/cmd")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=1883)
    p.add_argument("--json", default=None)
    p.add_argument("--file", default=None)
    p.add_argument("--qos", type=int, default=0)
    p.add_argument("--secs", type=int, default=10)
    p.set_defaults(func=mode_pub)

    a = ap.parse_args()
    a.func(a)


if __name__ == "__main__":
    main()
