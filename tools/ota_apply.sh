#!/bin/sh
# OTA 应用升级（v4）：断点续传（wget -c + 服务器 Range/206）+ HMAC 来源验签。
# - 断点续传：wget -c 从 /tmp/ota_pkg 已有字节继续；换包残片由「size 越界清除」
#   与 sha256 终验双保险兜底。
# - HMAC：manifest 的 hmac 字段【强制】要求（缺失/不符均拒）——防「剥字段降级」；
#   验签用板载 python3.8.5（tools/ota_hmac.py，无第三方依赖）。
# 其余同 v3：状态重写式落 /var/ota/status；纯 tar；file/sha 缺省读 manifest。
# 用法: ota_apply.sh [url_base] [file] [sha256]
URL=${1:-http://10.218.148.51:8080}
FILE=${2:-$(grep -o '"file":"[^"]*"' /tmp/ota_manifest.json 2>/dev/null | cut -d'"' -f4)}
SHA=${3:-$(grep -o '"sha256":"[^"]*"' /tmp/ota_manifest.json 2>/dev/null | cut -d'"' -f4)}
mkdir -p /var/ota
S=/var/ota/status
upd() { : > $S; for L in "$@"; do echo "$L" >> $S; done; }
[ -n "$FILE" ] && [ -n "$SHA" ] || { upd "STATE=ERR_ARGS"; exit 1; }

TOTAL=$(grep -o '"size":[0-9]*' /tmp/ota_manifest.json 2>/dev/null | head -1 | cut -d: -f2)
[ -n "$TOTAL" ] && [ "$TOTAL" -gt 0 ] 2>/dev/null || TOTAL=0

upd "STATE=DOWNLOADING"
SRC=HTTP
# 残片防御：已有文件若超过包大小（服务器换过包），续传会拼出混合体 → 清掉重来
if [ -f /tmp/ota_pkg ]; then
    SZ0=$(stat -c %s /tmp/ota_pkg 2>/dev/null || echo 0)
    if [ "$TOTAL" -gt 0 ] && [ "$SZ0" -gt "$TOTAL" ]; then rm -f /tmp/ota_pkg; fi
fi
# -c 断点续传（服务器需支持 Range/206）；-t 3 -T 30 弱网宽容
wget -q -c -t 3 -T 30 -O /tmp/ota_pkg "$URL/$FILE" 2>/dev/null &
WPID=$!
while kill -0 $WPID 2>/dev/null; do
    SZ=$(stat -c %s /tmp/ota_pkg 2>/dev/null || echo 0)
    if [ "$TOTAL" -gt 0 ]; then
        P=$((SZ * 100 / TOTAL)); [ $P -gt 100 ] && P=100
        upd "STATE=DOWNLOADING" "PROGRESS=$P%"
    fi
    sleep 1
done
wait $WPID 2>/dev/null
if [ ! -s /tmp/ota_pkg ]; then
    if [ -s /var/ota/ota_pkg_local ]; then SRC=LOCAL; cp /var/ota/ota_pkg_local /tmp/ota_pkg
    elif [ -s /tmp/ota_pkg_local ]; then SRC=LOCAL; cp /tmp/ota_pkg_local /tmp/ota_pkg
    fi
    # ★ D27b：回退包必须用它**自己**的 manifest 校验。
    # 原实现回退 LOCAL 旧包后仍拿 HTTP 新包的 sha256/hmac 去校 → 必然
    # ERR_SHA / ERR_HMAC：安全方向没错（不会装错包），但报错误导，而且
    # 「ADB 预推送」这条兜底通道实际上永远不可用。改为切换到 LOCAL manifest，
    # sha 与 hmac 都取自同一份，校验的是「这个包本身是否可信」。
    if [ "$SRC" = LOCAL ]; then
        LM=/var/ota/ota_manifest_local.json
        [ -s "$LM" ] || LM=/tmp/ota_manifest_local.json
        if [ -s "$LM" ]; then
            LSHA=$(grep -o '"sha256":"[^"]*"' "$LM" 2>/dev/null | head -1 | cut -d'"' -f4)
            [ -n "$LSHA" ] && SHA=$LSHA
            cp -f "$LM" /tmp/ota_manifest.json 2>/dev/null
        fi
    fi
fi
[ -s /tmp/ota_pkg ] || { upd "STATE=ERR_DOWNLOAD"; exit 1; }

upd "STATE=VERIFY"
ACT=$(sha256sum /tmp/ota_pkg | cut -d' ' -f1)
[ "$ACT" = "$SHA" ] || { upd "STATE=ERR_SHA"; rm -f /tmp/ota_pkg; exit 1; }

# HMAC 来源验签（FR-29：sha256 防损坏，hmac 防伪造）。
# 强制字段：缺失即拒（杜绝攻击者剥掉 hmac 字段降级成无验签包）。
HMAC=$(grep -o '"hmac":"[^"]*"' /tmp/ota_manifest.json 2>/dev/null | cut -d'"' -f4)
[ -n "$HMAC" ] || { upd "STATE=ERR_HMAC"; rm -f /tmp/ota_pkg; exit 1; }
CALC=$(python3 /usr/share/lvgl/ota_hmac.py /tmp/ota_pkg /etc/safe/ota.key 2>/dev/null)
[ -n "$CALC" ] && [ "$CALC" = "$HMAC" ] || { upd "STATE=ERR_HMAC"; rm -f /tmp/ota_pkg; exit 1; }

upd "STATE=EXTRACT" "SRC=$SRC"
rm -rf /tmp/ota_x; mkdir -p /tmp/ota_x
tar -xf /tmp/ota_pkg -C /tmp/ota_x 2>/dev/null || { upd "STATE=ERR_TAR"; exit 1; }
[ -f /tmp/ota_x/lvglsim ] || { upd "STATE=ERR_PKG"; exit 1; }

upd "STATE=INSTALL"
cp -f /usr/share/lvgl/lvglsim /usr/share/lvgl/lvglsim.old
[ -f /usr/share/lvgl/ota_version ] && cp -f /usr/share/lvgl/ota_version /usr/share/lvgl/ota_version.old
cp -f /tmp/ota_x/lvglsim /usr/share/lvgl/lvglsim.new && sync
mv -f /usr/share/lvgl/lvglsim.new /usr/share/lvgl/lvglsim
[ -f /tmp/ota_x/ota_version ] && { cat /tmp/ota_x/ota_version > /usr/share/lvgl/ota_version.new; mv -f /usr/share/lvgl/ota_version.new /usr/share/lvgl/ota_version; }
sync
rm -f /tmp/ota_pkg
upd "STATE=RESTARTING" "SRC=$SRC"
sleep 1
killall lvglsim 2>/dev/null
rm -f /var/run/lvgl.pid
/etc/init.d/S05lvgl start
