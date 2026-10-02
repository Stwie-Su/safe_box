#!/bin/sh
# VM→板子 ADB 分发：manifest + 最新包 → /tmp（即时）+ /var/ota（持久）
set -e
SRV="/home/book/桌面/ota-server"
PKG=$(grep -o '"file"[^,}]*' "$SRV/manifest.json" | head -1 | cut -d'"' -f4)
adb push "$SRV/manifest.json" /tmp/ota_manifest_local.json
adb push "$SRV/$PKG" /tmp/ota_pkg_local
CMD='mkdir -p /var/ota; cp -f /tmp/ota_manifest_local.json /tmp/ota_pkg_local /var/ota/ 2>/dev/null; true'
B64=$(printf '%s' "$CMD" | base64 | tr -d '\n')
adb shell "echo $B64 | base64 -d | su" >/dev/null 2>&1 || true
echo "pushed: $PKG -> /tmp + /var/ota"
