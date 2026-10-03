#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""i.MX6ULL fb0 抓屏转 PNG。

板端参数（实测）：/dev/fb0 实际 1024x640 @32bpp（非 600），LVGL 用前 600 行；
且内容是 SAFE_DISPLAY_ROT=180 软件旋转后的像素 —— 需再转 180° 才是用户视角。

用法: fb2png.py <fb.raw> <out.png> [crop_h] [order]
      order: BGRA(默认) / RGBA
"""
import sys
from PIL import Image

src = sys.argv[1]
dst = sys.argv[2]
crop_h = int(sys.argv[3]) if len(sys.argv) > 3 else 600
order = sys.argv[4] if len(sys.argv) > 4 else "BGRA"

raw = open(src, "rb").read()
W, H = 1024, 640
need = W * H * 4
if len(raw) < need:
    raise SystemExit("raw too small: %d < %d" % (len(raw), need))

im = Image.frombytes("RGBA", (W, H), raw[:need], "raw", order, 0, 1)
if crop_h and crop_h < H:
    im = im.crop((0, 0, W, crop_h))
im = im.rotate(180)
im.convert("RGB").save(dst)
print("saved", dst, im.size)

