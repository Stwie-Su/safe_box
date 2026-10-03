#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""严格版尺寸扫描：只认「同一变量」在 create 后 8 行内被显式设尺寸。"""
import os
import re
import sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else "/home/book/桌面/lv_port_linux/app"
SIZE_RE = re.compile(r"lv_obj_set_(size|width|height)\s*\(\s*(\w+)\s*,")


def scan(path):
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    out = []
    for i, ln in enumerate(lines):
        m = re.search(r"\b(\w+)\s*=\s*lv_obj_create\s*\(", ln)
        if not m:
            continue
        var = m.group(1)
        window = lines[i + 1:i + 9]
        sized = False
        for w in window:
            for mm in SIZE_RE.finditer(w):
                if mm.group(2) == var:
                    sized = True
        if not sized:
            out.append((i + 1, var, ln.strip()[:60]))
    return out


n = 0
for dp, _, fs in os.walk(ROOT):
    if "/build" in dp or "/generated" in dp:
        continue
    for f in sorted(fs):
        if not f.endswith(".c"):
            continue
        p = os.path.join(dp, f)
        rel = os.path.relpath(p, ROOT)
        for ln, var, txt in scan(p):
            print("[SIZE-STRICT] %s:%d  %s" % (rel, ln, var))
            n += 1
print("TOTAL=%d" % n)

