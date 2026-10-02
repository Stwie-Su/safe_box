#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""OTA 包 HMAC-SHA256 验签（板端，FR-29 来源验证）。

用法: ota_hmac.py <pkg_file> <key_file>
输出: 小写 hex 摘要（stdout 唯一一行）；错误输出空串并退 1。

设计说明：
- 密钥为 64 字符 hex 字符串文件（/etc/safe/ota.key），两端（打包机与本脚本）
  以【同样的字节串】作为 HMAC key——打包侧用
  `openssl dgst -sha256 -hmac "$(cat ota_hmac.key)"`，本侧用
  `hmac.new(key_bytes, ...)`，两者对同一 key 字节串结果一致。
- 板端 BusyBox 无 hmacsum，板载 python3.8.5 标准库 hmac/hashlib 即可，无第三方依赖。
"""
import hashlib
import hmac
import sys


def main() -> int:
    if len(sys.argv) != 3:
        return 1
    try:
        with open(sys.argv[2], "rb") as f:
            key = f.read().strip()
        with open(sys.argv[1], "rb") as f:
            data = f.read()
    except OSError:
        return 1
    if not key:
        return 1
    print(hmac.new(key, data, hashlib.sha256).hexdigest())
    return 0


if __name__ == "__main__":
    sys.exit(main())
