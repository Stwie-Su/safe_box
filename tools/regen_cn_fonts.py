#!/usr/bin/env python3
"""Regenerate lv_font_cn_*.c from all CJK chars used in src/.
Uses the system's NotoSansCJK-Regular.ttc (VM has it; .otf not available).
"""
import os
import re
import subprocess
import sys

SRC = os.path.expanduser("~/桌面/lv_port_linux/src")
FONT = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"
OUT = os.path.expanduser("~/桌面/lv_port_linux/src/ui/fonts")
SIZES = [14, 16, 20, 28]

def collect_cjk(path):
    chars = set()
    for root, _, files in os.walk(path):
        for fn in files:
            if not fn.endswith((".c", ".h")):
                continue
            fp = os.path.join(root, fn)
            try:
                with open(fp, "r", encoding="utf-8", errors="ignore") as f:
                    text = f.read()
            except Exception:
                continue
            for ch in text:
                cp = ord(ch)
                # CJK Unified Ideographs + Extension A + compatibility + punctuation blocks
                if (0x4E00 <= cp <= 0x9FFF) or (0x3400 <= cp <= 0x4DBF) or (0xF900 <= cp <= 0xFAFF) or \
                   (0x3000 <= cp <= 0x303F) or (0xFF00 <= cp <= 0xFFEF):
                    chars.add(cp)
    return sorted(chars)

def build_range(cps):
    # always include ASCII + common symbols from original fonts
    parts = ["0x20-0x7f"]
    extra_symbols = [0xB0, 0xB7, 0xD7, 0x2014, 0x2026, 0x2103, 0x2190, 0x2192,
                     0x25A0, 0x25CF, 0x2713]
    all_cps = sorted(set(cps) | set(extra_symbols))
    # group consecutive codepoints
    ranges = []
    start = prev = all_cps[0]
    for cp in all_cps[1:]:
        if cp == prev + 1:
            prev = cp
        else:
            if start == prev:
                ranges.append(f"0x{start:04X}")
            else:
                ranges.append(f"0x{start:04X}-0x{prev:04X}")
            start = prev = cp
    if start == prev:
        ranges.append(f"0x{start:04X}")
    else:
        ranges.append(f"0x{start:04X}-0x{prev:04X}")
    return ",".join(parts + ranges)

def main():
    if not os.path.exists(FONT):
        print(f"font not found: {FONT}")
        sys.exit(1)
    cps = collect_cjk(SRC)
    rng = build_range(cps)
    print(f"collected {len(cps)} unique CJK/codepoints from src/")
    print(f"range preview: {rng[:200]}...")

    for sz in SIZES:
        out_file = os.path.join(OUT, f"lv_font_cn_{sz}.c")
        cmd = [
            "lv_font_conv",
            "--font", FONT,
            "--size", str(sz),
            "--bpp", "4",
            "--no-compress",
            "--no-prefilter",
            "-r", rng,
            "--format", "lvgl",
            "-o", out_file,
        ]
        print(f"\n=== generating lv_font_cn_{sz}.c ===")
        print(" ".join(cmd))
        subprocess.run(cmd, check=True)
        print(f"done: {out_file} ({os.path.getsize(out_file)} bytes)")

if __name__ == "__main__":
    main()
