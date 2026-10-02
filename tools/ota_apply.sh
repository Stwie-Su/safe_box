#!/bin/sh
# OTA 应用升级（v3）：状态文件重写式、落 /var/ota/status；HTTP 下载实时进度；
# 解包用纯 tar（板端 BusyBox 无 -z）；file/sha 可由参数传入，缺省自动读 manifest。
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
wget -q --tries=1 -T 10 -O /tmp/ota_pkg "$URL/$FILE" 2>/dev/null &
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
fi
[ -s /tmp/ota_pkg ] || { upd "STATE=ERR_DOWNLOAD"; exit 1; }

upd "STATE=VERIFY"
ACT=$(sha256sum /tmp/ota_pkg | cut -d' ' -f1)
[ "$ACT" = "$SHA" ] || { upd "STATE=ERR_SHA"; rm -f /tmp/ota_pkg; exit 1; }

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
