#!/usr/bin/env python3
"""
fm225_sim.py —— FM22x 模组模拟器（Sprint3 步骤 3c，c4/c5）。

用法：
    # 1. 建虚拟串口对（backend 开 /tmp/fm225_host，本脚本开 /tmp/fm225_dev 扮模组）
    socat -d -d pty,raw,echo=0,link=/tmp/fm225_host pty,raw,echo=0,link=/tmp/fm225_dev

    # 2. 应用侧（另开终端）
    SAFE_FACE_BACKEND=fm225 ./build_pc/bin/lvglsim

    # 3. 注入（另开终端，可反复执行；每次执行注入一条指定序列）
    python3 tools/fm225_sim.py ready          # 上电 READY 通知
    python3 tools/fm225_sim.py no_match        # 一次「未匹配」
    python3 tools/fm225_sim.py no_match 3      # 连续 3 次未匹配 → 应触发 WAIT_OTP
    python3 tools/fm225_sim.py liveness        # 活体检测失败 → 拒绝 + ALARM
    python3 tools/fm225_sim.py match 3         # 匹配成功 uid=3 → 开锁
    python3 tools/fm225_sim.py enroll 3        # 录入应答（uid=3 成功）
    python3 tools/fm225_sim.py delete 3        # 删除应答（uid=3 成功）
    python3 tools/fm225_sim.py heartbeat       # 心跳（周期 NOTE READY，可 Ctrl-C）
    python3 tools/fm225_sim.py listen          # 监听模式：打印主控下发的所有命令帧

协议（《FM22x 系列人脸锁算法模组用户开发手册 V1.7》§六）：
    帧 = EF AA + MsgID(1B) + Size(2B 大端) + Data(N) + XOR(1B)
    XOR = 除 SyncWord 外全部字节按位 XOR
    M>>H：REPLY(0x00) Data = mid(被应答命令) + result(MR_*) + [业务数据]
          NOTE (0x01) Data = nid + [附加数据]
    REPLY(VERIFY) 成功时 data 追加 user_id_heb + user_id_leb（手册 s_msg_reply_verify_data
    开头两字段；本链路只解析 uid，user_name/admin/unlockStatus 略去——模拟器与
    backend_fm225 的契约一致即可，真机到货后按完整结构补齐）。

MR_* 结果码（见手册）：0=SUCCESS 1=REJECTED 2=ABORTED 4=CAMERA 5=UNKNOWN
6=INVALIDPARAM 7=NOMEMORY 8=UNKNOWNUSER(未匹配) 9=MAXUSER 10=FACEENROLLED
12=LIVENESSCHECK(活体失败) 13=TIMEOUT
"""

import os
import sys
import time

DEV = os.environ.get("FM225_SIM_DEV", "/tmp/fm225_dev")

MSGID_REPLY = 0x00
MSGID_NOTE = 0x01

# H>>M 命令字（监听模式打印用）
CMD_NAMES = {
    0x10: "RESET", 0x11: "GET_STATUS", 0x12: "VERIFY", 0x13: "ENROLL",
    0x1D: "ENROLL_SINGLE", 0x20: "DELETE_USER", 0x21: "DELETE_ALL",
    0x22: "GET_USER_INFO", 0x23: "FACE_RESET", 0x24: "GET_ALL_USERID",
    0x26: "ENROLL_ITG", 0x30: "GET_VERSION", 0x50: "INIT_ENCRYPTION",
    0x93: "GET_SN", 0xB0: "READ_USB_UVC", 0xB1: "SET_USB_UVC",
}

MR_SUCCESS = 0x00
MR_UNKNOWNUSER = 0x08
MR_LIVENESSCHECK = 0x0C


def build_frame(msgid: int, data: bytes) -> bytes:
    """拼一帧：EF AA + msgid + size(大端) + data + xor。"""
    xor = msgid ^ ((len(data) >> 8) & 0xFF) ^ (len(data) & 0xFF)
    for b in data:
        xor ^= b
    return bytes([0xEF, 0xAA, msgid, (len(data) >> 8) & 0xFF, len(data) & 0xFF]) + data + bytes([xor])


def reply(mid: int, result: int, extra: bytes = b"") -> bytes:
    """REPLY 帧：Data = mid + result + extra。"""
    return build_frame(MSGID_REPLY, bytes([mid, result]) + extra)


def note(nid: int, extra: bytes = b"") -> bytes:
    """NOTE 帧：Data = nid + extra。"""
    return build_frame(MSGID_NOTE, bytes([nid]) + extra)


def uid_bytes(uid: int) -> bytes:
    """user_id heb + leb（手册结构前两字段，先存高八位）。"""
    return bytes([(uid >> 8) & 0xFF, uid & 0xFF])


# ---------------- 各注入序列 ----------------

def seq_ready(w):
    w(note(0x00))                     # NID_READY：模组就绪


def seq_no_match(w, n=1):
    for _ in range(n):
        w(reply(0x12, MR_UNKNOWNUSER))   # VERIFY 应答：未匹配用户
        time.sleep(0.15)                 # 模组逐次出结果的时间感


def seq_liveness(w):
    w(reply(0x12, MR_LIVENESSCHECK))    # VERIFY 应答：活体失败


def seq_match(w, uid=3):
    w(reply(0x12, MR_SUCCESS, uid_bytes(uid)))   # VERIFY 应答：成功 + uid


def seq_enroll(w, uid=3):
    w(reply(0x13, MR_SUCCESS, uid_bytes(uid) + bytes([0x1F])))   # ENROLL 成功 uid=3


def seq_delete(w, uid=3):
    w(reply(0x20, MR_SUCCESS, uid_bytes(uid)))   # DELETE_USER 应答


def seq_bad(w):
    """坏帧注入（调试解析器容错）：坏 XOR + 噪声 + 后跟好帧。"""
    good = reply(0x12, MR_UNKNOWNUSER)
    bad = bytearray(good)
    bad[-1] ^= 0xFF                      # 破坏 XOR
    w(bytes([0x12, 0x34, 0xEF, 0x55]))   # 噪声前导（含孤立 0xEF）
    w(bytes(bad))
    w(reply(0x12, MR_UNKNOWNUSER))       # 好帧：验证重同步


def seq_heartbeat(w):
    """心跳：周期 NOTE READY（模组存活信号，主控侧刷新在线状态）。"""
    print("心跳模式：每 2s 一条 NOTE READY，Ctrl-C 退出")
    try:
        while True:
            w(note(0x00))
            time.sleep(2)
    except KeyboardInterrupt:
        print("\n心跳停止")


def seq_listen(r):
    """监听模式：持续读 /tmp/fm225_dev，打印主控下发的命令帧（hex + 命令名）。"""
    print(f"监听 {DEV}：等待主控下发命令（VERIFY 应由应用自动触发）…")
    buf = b""
    try:
        while True:
            chunk = os.read(r, 256)
            if not chunk:
                break
            buf += chunk
            # 简单帧提取：找 EF AA 头，按 Size 切
            while len(buf) >= 7 and buf[0:2] == b"\xEF\xAA":
                size = (buf[3] << 8) | buf[4]
                total = 6 + size + 1
                if len(buf) < total:
                    break
                frame, buf = buf[:total], buf[total:]
                cmd = frame[2]
                name = CMD_NAMES.get(cmd, f"0x{cmd:02X}")
                print(f"  <- 命令 {name}(0x{cmd:02X}) size={size} data={frame[5:5+size].hex()}")
            # 丢掉帧头前的噪声
            while buf and not buf.startswith(b"\xEF\xAA"):
                buf = buf[1:]
    except KeyboardInterrupt:
        print("\n监听结束")


# ---------------- main ----------------

USAGE = __doc__

def main():
    if len(sys.argv) < 2:
        print(USAGE)
        sys.exit(1)
    mode = sys.argv[1]
    arg = int(sys.argv[2]) if len(sys.argv) > 2 else None

    if not os.path.exists(DEV):
        print(f"[!] {DEV} 不存在——先起虚拟串口对：")
        print("    socat -d -d pty,raw,echo=0,link=/tmp/fm225_host "
              "pty,raw,echo=0,link=/tmp/fm225_dev")
        sys.exit(1)

    fd = os.open(DEV, os.O_RDWR)

    if mode == "listen":
        seq_listen(fd)
        return

    seqs = {
        "ready":     lambda: seq_ready(lambda d: os.write(fd, d)),
        "no_match":  lambda: seq_no_match(lambda d: os.write(fd, d), arg or 1),
        "liveness":  lambda: seq_liveness(lambda d: os.write(fd, d)),
        "match":     lambda: seq_match(lambda d: os.write(fd, d), arg or 3),
        "enroll":    lambda: seq_enroll(lambda d: os.write(fd, d), arg or 3),
        "delete":    lambda: seq_delete(lambda d: os.write(fd, d), arg or 3),
        "bad":       lambda: seq_bad(lambda d: os.write(fd, d)),
        "heartbeat": lambda: seq_heartbeat(lambda d: os.write(fd, d)),
    }
    if mode not in seqs:
        print(USAGE)
        sys.exit(1)

    seqs[mode]()
    print(f"已注入序列：{mode}" + (f" ×{arg}" if arg is not None and mode == "no_match" else ""))
    os.close(fd)


if __name__ == "__main__":
    main()
