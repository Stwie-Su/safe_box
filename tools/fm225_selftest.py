#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""FM225（FM22x 双目人脸模组）串口自检工具 —— 上板预备。

用途：在**不启动主程序**的前提下，单独验证「上位机串口 <-> FM225」这一段链路是否通，
把「线接错了 / 波特率不对 / 节点没权限 / 模组没上电」这几类上板最常见的问题
在 30 秒内定位清楚。

设计约束：**只用 Python 标准库**（os / termios / select），不依赖 pyserial——
板子的 Buildroot 根文件系统里没有 pip，也不该为一个自检脚本装依赖。

三种模式
--------
1) 监听模式（默认）：配好 115200 8N1，等模组主动上报的帧。模组上电就绪会发
   NOTE READY（EF AA 01 00 01 00 00），只要收到任意一帧 XOR 校验正确的帧即判
   链路通。适用于：模组已上电、只想知道线序/波特率对不对。

2) --send-reset：先下发 FACE_RESET（cmd=0x10，帧 EF AA 10 00 00 10）再监听。
   模组若在线会回 REPLY / 重发 NOTE READY，比纯监听更容易拿到反馈。

3) --loopback：把排针上的 TX 与 RX 用杜邦线**短接**，工具下发一帧并期待原样
   收回。收到的字节与发出的完全一致 -> 证明「这个设备节点、这两个引脚、这个
   波特率」是对的。这是到货后确认排针定义的**第一步**（此时模组甚至可以不上电）。
   注意：短接前先确认模组 TX 处于高阻/未上电，避免两个输出口对顶。

用法
----
    python3 tools/fm225_selftest.py                      # 监听默认节点（SAFE_FM225_DEV）
    python3 tools/fm225_selftest.py /dev/ttymxc2         # 指定节点
    python3 tools/fm225_selftest.py /dev/ttymxc2 --send-reset
    python3 tools/fm225_selftest.py /dev/ttymxc2 --loopback
    python3 tools/fm225_selftest.py /dev/ttymxc2 --timeout 15 --baud 115200

退出码
------
    0  PASS          收到合法帧 / 回环字节完全一致
    1  NO_DATA       超时未收到任何字节（接线或模组未上电）
    2  OPEN_FAIL     设备节点打不开（不存在 / 无权限 / 被占用）
    3  LOOPBACK_BAD  回环收到字节但与发出的不一致（波特率或线序错）
    4  BAD_FRAME     收到了字节但没有任何一帧 XOR 校验通过（波特率多半不对）

协议依据：《FM22x 系列人脸锁算法模组用户开发手册 V1.7》§六(一)
    帧 = EF AA + MsgID + Size(2B 大端) + Data + XOR
    XOR = MsgID ^ Size高 ^ Size低 ^ 所有 Data 字节
"""

import argparse
import os
import select
import sys
import termios
import time

SYNC0 = 0xEF
SYNC1 = 0xAA

CMD_RESET = 0x10
CMD_GET_STATUS = 0x11

MSGID_REPLY = 0x00
MSGID_NOTE = 0x01
MSGID_IMAGE = 0x02
MSGID_NAME = {MSGID_REPLY: "REPLY", MSGID_NOTE: "NOTE", MSGID_IMAGE: "IMAGE"}

NID_READY = 0x00
NID_FACE_STATE = 0x01

BAUD_MAP = {
    9600: termios.B9600,
    19200: termios.B19200,
    38400: termios.B38400,
    57600: termios.B57600,
    115200: termios.B115200,
    230400: termios.B230400,
    460800: termios.B460800,
    921600: termios.B921600,
}

DEFAULT_DEV = "/tmp/fm225_host"   # 与 app/hal/face/backend_fm225.c 的默认保持一致


def default_dev() -> str:
    """与主程序同一套优先级：SAFE_FM225_DEV > SAFE_FACE_DEV > /tmp/fm225_host。"""
    for name in ("SAFE_FM225_DEV", "SAFE_FACE_DEV"):
        v = os.environ.get(name, "")
        if v:
            return v
    return DEFAULT_DEV


def build_frame(msgid_or_cmd: int, payload: bytes = b"") -> bytes:
    """组帧：EF AA + cmd + size(大端) + payload + XOR。"""
    size = len(payload)
    xor = msgid_or_cmd ^ ((size >> 8) & 0xFF) ^ (size & 0xFF)
    for b in payload:
        xor ^= b
    return bytes([SYNC0, SYNC1, msgid_or_cmd,
                  (size >> 8) & 0xFF, size & 0xFF]) + payload + bytes([xor])


class FrameParser:
    """与固件 fm225_proto.c 同构的最小解析器（只关心能否解出合法帧）。"""

    def __init__(self) -> None:
        self.state = 0          # 0=等 EF, 1=等 AA, 2=msgid, 3=size 高, 4=size 低, 5=data, 6=xor
        self.msgid = 0
        self.size = 0
        self.got = 0
        self.buf = bytearray()
        self.xor = 0
        self.frames = []        # [(msgid, data)]
        self.bad = 0
        self.bytes_in = 0

    def feed(self, chunk: bytes) -> None:
        self.bytes_in += len(chunk)
        for b in chunk:
            if self.state == 0:
                if b == SYNC0:
                    self.state = 1
                continue
            if self.state == 1:
                if b == SYNC1:
                    self.state = 2
                else:
                    self.state = 0 if b != SYNC0 else 1
                continue
            if self.state == 2:
                self.msgid = b
                self.xor = b
                self.state = 3
                continue
            if self.state == 3:
                self.size = b << 8
                self.xor ^= b
                self.state = 4
                continue
            if self.state == 4:
                self.size |= b
                self.xor ^= b
                self.got = 0
                self.buf = bytearray()
                self.state = 5 if self.size else 6
                continue
            if self.state == 5:
                self.buf.append(b)
                self.xor ^= b
                self.got += 1
                if self.got >= self.size:
                    self.state = 6
                continue
            if self.state == 6:
                if b == (self.xor & 0xFF):
                    self.frames.append((self.msgid, bytes(self.buf)))
                else:
                    self.bad += 1
                self.state = 0
                continue


def configure_port(fd: int, baud: int) -> None:
    """115200 8N1 无流控 raw 模式——与 backend_fm225.c 的 termios 配置对齐。"""
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0                                    # iflag
    attrs[1] = 0                                    # oflag
    cflag = attrs[2]
    cflag &= ~(termios.CSIZE | termios.PARENB | termios.CSTOPB)
    cflag |= termios.CS8 | termios.CREAD | termios.CLOCAL
    if hasattr(termios, "CRTSCTS"):
        cflag &= ~termios.CRTSCTS                   # 关硬件流控
    attrs[2] = cflag
    attrs[3] = 0                                    # lflag：raw
    attrs[4] = BAUD_MAP[baud]                       # ispeed
    attrs[5] = BAUD_MAP[baud]                       # ospeed
    attrs[6][termios.VMIN] = 1
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)


def describe_frame(msgid: int, data: bytes) -> str:
    if not data:
        return "%s (空 Data)" % MSGID_NAME.get(msgid, "0x%02X" % msgid)
    if msgid == MSGID_NOTE:
        nid = data[0]
        extra = "READY(模组就绪)" if nid == NID_READY else (
            "FACE_STATE" if nid == NID_FACE_STATE else "nid=0x%02X" % nid)
        return "NOTE %s" % extra
    if msgid == MSGID_REPLY:
        if len(data) >= 2:
            return "REPLY mid=0x%02X result=0x%02X" % (data[0], data[1])
        return "REPLY data=%s" % data.hex()
    return "%s data=%s" % (MSGID_NAME.get(msgid, "0x%02X" % msgid), data.hex())


def main() -> int:
    ap = argparse.ArgumentParser(description="FM225 串口自检（纯标准库）")
    ap.add_argument("dev", nargs="?", default=None,
                    help="串口设备节点（默认取 SAFE_FM225_DEV / SAFE_FACE_DEV，"
                         "再默认 /tmp/fm225_host）")
    ap.add_argument("--baud", type=int, default=115200, choices=sorted(BAUD_MAP),
                    help="波特率（默认 115200，与模组手册一致）")
    ap.add_argument("--timeout", type=float, default=8.0,
                    help="等待字节的秒数（默认 8）")
    ap.add_argument("--send-reset", action="store_true",
                    help="先下发 FACE_RESET 再监听（更容易拿到反馈）")
    ap.add_argument("--loopback", action="store_true",
                    help="回环模式：短接 TX/RX，下发一帧期待原样收回")
    args = ap.parse_args()

    dev = args.dev or default_dev()

    print("FM225 串口自检")
    print("  设备节点 : %s" % dev)
    print("  波特率   : %d 8N1 无流控" % args.baud)
    print("  模式     : %s" % ("回环（TX/RX 需短接）" if args.loopback
                               else ("下发 RESET + 监听" if args.send_reset else "监听")))
    print("  超时     : %.1fs" % args.timeout)

    if not os.path.exists(dev):
        print("  节点检查 : 不存在")
        print("FAIL: 设备节点 %s 不存在（上板先确认接线与内核是否导出该节点）" % dev)
        return 2

    try:
        fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    except OSError as exc:
        print("FAIL: 打开 %s 失败：%s（检查权限 dialout/root，或是否已被别的进程占用）"
              % (dev, exc))
        return 2
    print("  节点检查 : 存在，打开成功（fd=%d）" % fd)

    try:
        configure_port(fd, args.baud)
    except termios.error as exc:
        print("FAIL: termios 配置失败：%s（pty/虚拟串口可能不支持全部参数）" % exc)
        os.close(fd)
        return 2
    print("  termios  : 配置成功")

    if args.loopback:
        probe = build_frame(CMD_GET_STATUS)
        print("  下发     : %s" % probe.hex(" ").upper())
        os.write(fd, probe)
        got = bytearray()
        deadline = time.time() + args.timeout
        while time.time() < deadline and len(got) < len(probe):
            r, _, _ = select.select([fd], [], [], 0.2)
            if r:
                got.extend(os.read(fd, 256))
        if not got:
            print("FAIL: 回环超时未收到任何字节——TX/RX 没短接，或短接的不是这个节点")
            return 1
        print("  收回     : %s" % bytes(got).hex(" ").upper())
        if bytes(got) == probe:
            print("PASS: 回环字节完全一致（%d B）——节点/引脚/波特率三者都对" % len(got))
            return 0
        print("FAIL: 回环字节不一致——多半是波特率不对，或 TX/RX 接到了别的引脚")
        return 3

    if args.send_reset:
        frame = build_frame(CMD_RESET)
        print("  下发     : FACE_RESET %s" % frame.hex(" ").upper())
        os.write(fd, frame)

    parser = FrameParser()
    deadline = time.time() + args.timeout
    print("  监听中   ...（模组上电就绪会主动发 NOTE READY）")
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            try:
                chunk = os.read(fd, 4096)
            except OSError:
                break
            if chunk:
                parser.feed(chunk)
                if parser.frames:
                    break

    print("  收到字节 : %d B（合法帧 %d，校验失败 %d）"
          % (parser.bytes_in, len(parser.frames), parser.bad))
    if parser.frames:
        for msgid, data in parser.frames[:10]:
            print("  -> %s" % describe_frame(msgid, data))
        print("PASS: 收到合法帧——串口链路通（线序与波特率正确，模组已上电）")
        return 0

    if parser.bytes_in == 0:
        print("FAIL: 超时未收到任何字节")
        print("  排查顺序：① 模组是否上电 ② TX/RX 是否交叉（模组 TX 接上位机 RX）"
              " ③ 共地 ④ 波特率 ⑤ 用 --loopback 先验证引脚")
        return 1

    print("FAIL: 收到了 %d 字节但没有一帧 XOR 校验通过——波特率大概率不对" % parser.bytes_in)
    return 4


if __name__ == "__main__":
    sys.exit(main())
