#!/bin/sh
# OTA 检查更新（v3：路径可配置 + 重写式状态文件——状态文件=当前状态快照，不再追加，
# UI 端解析取最后匹配，双保险）。HTTP 直连优先，失败回退 ADB 预推送清单。
# 用法: ota_check.sh <url_base> <current_version>
#
# ★ 路径 env 覆盖（2026-10-09 加；**默认值与板端完全一致，板端行为零变化**）：
#     SAFE_OTA_TMP   临时目录（默认 /tmp）
#     SAFE_OTA_DIR   持久目录与「ADB 预推送」兜底位置（默认 /var/ota）
#   为什么加：原脚本把 /tmp、/var/ota 写死，PC / VM 上无法无头验收（会往真实系统
#   路径写文件）。而需求 §10 把 OTA 的验收列为硬要求 —— **不可验收即不可复现**，
#   这与本项目「ctest 全绿证明不了可复现」是同一条纪律。
URL=${1:-http://10.218.148.51:8080}
TMP=${SAFE_OTA_TMP:-/tmp}
OTADIR=${SAFE_OTA_DIR:-/var/ota}
S=$TMP/ota_status
upd() { : > $S; for L in "$@"; do echo "$L" >> $S; done; }
upd "STATE=CHECKING"
SRC=HTTP
wget -q --tries=1 -T 3 -O "$TMP/ota_manifest.json" "$URL/manifest.json" 2>/dev/null
if [ ! -s "$TMP/ota_manifest.json" ]; then
    if [ -s "$OTADIR/ota_manifest_local.json" ]; then
        SRC=LOCAL; cp "$OTADIR/ota_manifest_local.json" "$TMP/ota_manifest.json"
    elif [ -s "$TMP/ota_manifest_local.json" ]; then
        SRC=LOCAL; cp "$TMP/ota_manifest_local.json" "$TMP/ota_manifest.json"
    else
        upd "STATE=ERR_NET"; exit 1
    fi
fi
VER=$(grep -o '"version"[^,}]*' "$TMP/ota_manifest.json" | head -1 | cut -d'"' -f4)
SHA=$(grep -o '"sha256"[^,}]*' "$TMP/ota_manifest.json" | head -1 | cut -d'"' -f4)
FILE=$(grep -o '"file"[^,}]*' "$TMP/ota_manifest.json" | head -1 | cut -d'"' -f4)
if [ -z "$VER" ] || [ -z "$SHA" ] || [ -z "$FILE" ]; then upd "STATE=ERR_MANIFEST"; exit 1; fi
upd "STATE=CHECKED" "SRC=$SRC" "SERVER_VERSION=$VER" "FILE=$FILE" "SHA256=$SHA"
