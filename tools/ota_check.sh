#!/bin/sh
# OTA 检查更新（v2：重写式状态文件——状态文件=当前状态快照，不再追加，
# UI 端解析取最后匹配，双保险）。HTTP 直连优先，失败回退 ADB 预推送清单。
# 用法: ota_check.sh <url_base> <current_version>
URL=${1:-http://10.218.148.51:8080}
S=/tmp/ota_status
upd() { : > $S; for L in "$@"; do echo "$L" >> $S; done; }
upd "STATE=CHECKING"
SRC=HTTP
wget -q --tries=1 -T 3 -O /tmp/ota_manifest.json "$URL/manifest.json" 2>/dev/null
if [ ! -s /tmp/ota_manifest.json ]; then
    if [ -s /var/ota/ota_manifest_local.json ]; then
        SRC=LOCAL; cp /var/ota/ota_manifest_local.json /tmp/ota_manifest.json
    elif [ -s /tmp/ota_manifest_local.json ]; then
        SRC=LOCAL; cp /tmp/ota_manifest_local.json /tmp/ota_manifest.json
    else
        upd "STATE=ERR_NET"; exit 1
    fi
fi
VER=$(grep -o '"version"[^,}]*' /tmp/ota_manifest.json | head -1 | cut -d'"' -f4)
SHA=$(grep -o '"sha256"[^,}]*' /tmp/ota_manifest.json | head -1 | cut -d'"' -f4)
FILE=$(grep -o '"file"[^,}]*' /tmp/ota_manifest.json | head -1 | cut -d'"' -f4)
if [ -z "$VER" ] || [ -z "$SHA" ] || [ -z "$FILE" ]; then upd "STATE=ERR_MANIFEST"; exit 1; fi
upd "STATE=CHECKED" "SRC=$SRC" "SERVER_VERSION=$VER" "FILE=$FILE" "SHA256=$SHA"
