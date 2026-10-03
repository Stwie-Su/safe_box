#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""板端 TCP/TLS 连通性测试。用法: nettest.py <host> <port> [tls]"""
import socket
import ssl
import sys

host, port = sys.argv[1], int(sys.argv[2])
use_tls = len(sys.argv) > 3 and sys.argv[3] == "tls"
s = socket.socket()
s.settimeout(5)
try:
    s.connect((host, port))
    print("TCP OK", host, port)
    if use_tls:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        try:
            tls = ctx.wrap_socket(s)
            print("TLS OK", tls.version())
            tls.close()
        except Exception as e:
            print("TLS FAIL", e)
except Exception as e:
    print("TCP FAIL", host, port, e)

