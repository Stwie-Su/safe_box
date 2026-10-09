#!/bin/sh
# OTA 应用升级（v5）：断点续传（wget -c + 服务器 Range/206）+ HMAC 来源验签
# + 版本一致性校验（FR-29 验收：版本不符的包**拒绝且不留残余**）。
# - 断点续传：wget -c 从 /tmp/ota_pkg 已有字节继续；换包残片由「size 越界清除」
#   与 sha256 终验双保险兜底。
# - HMAC：manifest 的 hmac 字段【强制】要求（缺失/不符均拒）——防「剥字段降级」；
#   验签用板载 python3.8.5（tools/ota_hmac.py，无第三方依赖）。
# - 版本：解包后比对「包内 ota_version」与「manifest 声明的 version」，不一致即
#   ERR_VERSION —— 防「清单说 1.0.7、包里塞的是别的版本」的错包 / 伪造包。
# 状态重写式落 $SAFE_OTA_DIR/status；纯 tar；file/sha 缺省读 manifest。
# 用法: ota_apply.sh [url_base] [file] [sha256]
#
# ★ 路径 env 覆盖（2026-10-09 加；**默认值与板端完全一致，板端行为零变化**）：
#     SAFE_OTA_TMP        临时目录（默认 /tmp）
#     SAFE_OTA_DIR        持久目录与状态文件（默认 /var/ota）
#     SAFE_OTA_HMAC_PY    HMAC 验签脚本（默认 /usr/share/lvgl/ota_hmac.py）
#     SAFE_OTA_KEY        HMAC 密钥（默认 /etc/safe/ota.key）
#     SAFE_OTA_INSTALL_DIR 安装目标目录（默认 /usr/share/lvgl）
#     SAFE_OTA_NO_RESTART  非空 = **不做** killall / 起服务（PC 无头验收用：
#                          验收的是「接收 / 校验 / 落盘」，不该真的去杀进程）
#   为什么加：同 ota_check.sh —— 路径写死则无法无头验收，而验收是需求 §10 的硬要求。
URL=${1:-http://10.218.148.51:8080}
TMP=${SAFE_OTA_TMP:-/tmp}
OTADIR=${SAFE_OTA_DIR:-/var/ota}
HMACPY=${SAFE_OTA_HMAC_PY:-/usr/share/lvgl/ota_hmac.py}
OTAKEY=${SAFE_OTA_KEY:-/etc/safe/ota.key}
INSTALLDIR=${SAFE_OTA_INSTALL_DIR:-/usr/share/lvgl}
FILE=${2:-$(grep -o '"file":"[^"]*"' "$TMP/ota_manifest.json" 2>/dev/null | cut -d'"' -f4)}
SHA=${3:-$(grep -o '"sha256":"[^"]*"' "$TMP/ota_manifest.json" 2>/dev/null | cut -d'"' -f4)}
mkdir -p "$OTADIR"
S=$OTADIR/status
upd() { : > $S; for L in "$@"; do echo "$L" >> $S; done; }
[ -n "$FILE" ] && [ -n "$SHA" ] || { upd "STATE=ERR_ARGS"; exit 1; }

TOTAL=$(grep -o '"size":[0-9]*' "$TMP/ota_manifest.json" 2>/dev/null | head -1 | cut -d: -f2)
[ -n "$TOTAL" ] && [ "$TOTAL" -gt 0 ] 2>/dev/null || TOTAL=0

upd "STATE=DOWNLOADING"
SRC=HTTP
# 残片防御：已有文件若超过包大小（服务器换过包），续传会拼出混合体 → 清掉重来
if [ -f "$TMP/ota_pkg" ]; then
    SZ0=$(stat -c %s "$TMP/ota_pkg" 2>/dev/null || echo 0)
    if [ "$TOTAL" -gt 0 ] && [ "$SZ0" -gt "$TOTAL" ]; then rm -f "$TMP/ota_pkg"; fi
fi
# -c 断点续传（服务器需支持 Range/206）；-t 3 -T 30 弱网宽容
wget -q -c -t 3 -T 30 -O "$TMP/ota_pkg" "$URL/$FILE" 2>/dev/null &
WPID=$!
while kill -0 $WPID 2>/dev/null; do
    SZ=$(stat -c %s "$TMP/ota_pkg" 2>/dev/null || echo 0)
    if [ "$TOTAL" -gt 0 ]; then
        P=$((SZ * 100 / TOTAL)); [ $P -gt 100 ] && P=100
        upd "STATE=DOWNLOADING" "PROGRESS=$P%"
    fi
    sleep 1
done
wait $WPID 2>/dev/null
WRC=$?
# ★ 2026-10-09 修（无头验收发现）：**下载未完成 ≠ 校验失败**，两者必须分开。
#   原实现只要文件非空就往下走 sha 终验 —— 于是「断电 / 断网 / 进程被杀」留下的
#   半截残片会被当成完整包校 sha，必然 ERR_SHA，而 ERR_SHA 分支会 `rm -f ota_pkg`
#   把残片删掉。结果就是：断点续传的能力写在这里，却被自己的失败处理抵消掉，
#   下次永远从头下（需求 §10「断点续传从已落盘进度继续而非重头」实际不成立）。
#   现在：没下满 → ERR_DOWNLOAD 并**保留残片**给下次 `-c` 续传；
#         下满了但 sha 不符 → 那才是坏包，ERR_SHA 并删除（安全优先）。
GOT=$(stat -c %s "$TMP/ota_pkg" 2>/dev/null || echo 0)
if [ "$WRC" != 0 ] || { [ "$TOTAL" -gt 0 ] && [ "$GOT" -lt "$TOTAL" ]; }; then
    if [ "$TOTAL" -gt 0 ]; then P=$((GOT * 100 / TOTAL)); else P=0; fi
    upd "STATE=ERR_DOWNLOAD" "PROGRESS=$P%" "GOT=$GOT" "TOTAL=$TOTAL"
    exit 1
fi
if [ ! -s "$TMP/ota_pkg" ]; then
    if [ -s "$OTADIR/ota_pkg_local" ]; then SRC=LOCAL; cp "$OTADIR/ota_pkg_local" "$TMP/ota_pkg"
    elif [ -s "$TMP/ota_pkg_local" ]; then SRC=LOCAL; cp "$TMP/ota_pkg_local" "$TMP/ota_pkg"
    fi
    # ★ D27b：回退包必须用它**自己**的 manifest 校验。
    # 原实现回退 LOCAL 旧包后仍拿 HTTP 新包的 sha256/hmac 去校 → 必然
    # ERR_SHA / ERR_HMAC：安全方向没错（不会装错包），但报错误导，而且
    # 「ADB 预推送」这条兜底通道实际上永远不可用。改为切换到 LOCAL manifest，
    # sha 与 hmac 都取自同一份，校验的是「这个包本身是否可信」。
    if [ "$SRC" = LOCAL ]; then
        LM=$OTADIR/ota_manifest_local.json
        [ -s "$LM" ] || LM=$TMP/ota_manifest_local.json
        if [ -s "$LM" ]; then
            LSHA=$(grep -o '"sha256":"[^"]*"' "$LM" 2>/dev/null | head -1 | cut -d'"' -f4)
            [ -n "$LSHA" ] && SHA=$LSHA
            cp -f "$LM" "$TMP/ota_manifest.json" 2>/dev/null
        fi
    fi
fi
[ -s "$TMP/ota_pkg" ] || { upd "STATE=ERR_DOWNLOAD"; exit 1; }

upd "STATE=VERIFY"
ACT=$(sha256sum "$TMP/ota_pkg" | cut -d' ' -f1)
[ "$ACT" = "$SHA" ] || { upd "STATE=ERR_SHA"; rm -f "$TMP/ota_pkg"; exit 1; }

# HMAC 来源验签（FR-29：sha256 防损坏，hmac 防伪造）。
# 强制字段：缺失即拒（杜绝攻击者剥掉 hmac 字段降级成无验签包）。
HMAC=$(grep -o '"hmac":"[^"]*"' "$TMP/ota_manifest.json" 2>/dev/null | cut -d'"' -f4)
[ -n "$HMAC" ] || { upd "STATE=ERR_HMAC"; rm -f "$TMP/ota_pkg"; exit 1; }
CALC=$(python3 "$HMACPY" "$TMP/ota_pkg" "$OTAKEY" 2>/dev/null)
[ -n "$CALC" ] && [ "$CALC" = "$HMAC" ] || { upd "STATE=ERR_HMAC"; rm -f "$TMP/ota_pkg"; exit 1; }

upd "STATE=EXTRACT" "SRC=$SRC"
rm -rf "$TMP/ota_x"; mkdir -p "$TMP/ota_x"
# ★ ERR_TAR / ERR_PKG 分支也要清残余：需求 §10 要求「被拒的固件包全部拒绝且
#   **不留残余数据**」——解包失败却把包留在磁盘上，等于给下次续传留了个坏起点。
tar -xf "$TMP/ota_pkg" -C "$TMP/ota_x" 2>/dev/null || { upd "STATE=ERR_TAR"; rm -f "$TMP/ota_pkg"; rm -rf "$TMP/ota_x"; exit 1; }
[ -f "$TMP/ota_x/lvglsim" ] || { upd "STATE=ERR_PKG"; rm -f "$TMP/ota_pkg"; rm -rf "$TMP/ota_x"; exit 1; }

# 版本一致性（FR-29 验收「版本不符」）：包内声明的版本必须与 manifest 一致。
# ★ 校验对象是「包里真正带的 ota_version」，不是 URL / 文件名 —— 文件名可以随便改。
MVER=$(grep -o '"version":"[^"]*"' "$TMP/ota_manifest.json" 2>/dev/null | head -1 | cut -d'"' -f4)
if [ -n "$MVER" ]; then
    PVER=$(head -1 "$TMP/ota_x/ota_version" 2>/dev/null | tr -d ' \t\r')
    if [ -z "$PVER" ] || [ "$PVER" != "$MVER" ]; then
        upd "STATE=ERR_VERSION" "PKG_VERSION=$PVER" "MANIFEST_VERSION=$MVER"
        rm -f "$TMP/ota_pkg"; rm -rf "$TMP/ota_x"; exit 1
    fi
fi

upd "STATE=INSTALL"
cp -f "$INSTALLDIR/lvglsim" "$INSTALLDIR/lvglsim.old"
[ -f "$INSTALLDIR/ota_version" ] && cp -f "$INSTALLDIR/ota_version" "$INSTALLDIR/ota_version.old"
cp -f "$TMP/ota_x/lvglsim" "$INSTALLDIR/lvglsim.new" && sync
mv -f "$INSTALLDIR/lvglsim.new" "$INSTALLDIR/lvglsim"
[ -f "$TMP/ota_x/ota_version" ] && { cat "$TMP/ota_x/ota_version" > "$INSTALLDIR/ota_version.new"; mv -f "$INSTALLDIR/ota_version.new" "$INSTALLDIR/ota_version"; }
sync
# 解包目录一并清：安装完它就只是垃圾，留着既占空间、又让「有没有残余」说不清
rm -f "$TMP/ota_pkg"
rm -rf "$TMP/ota_x"
upd "STATE=RESTARTING" "SRC=$SRC"
# PC / VM 无头验收：到此为止（已验证完接收 → 校验 → 落盘），不去动真实服务进程
if [ -n "$SAFE_OTA_NO_RESTART" ]; then
    upd "STATE=INSTALLED" "SRC=$SRC"
    exit 0
fi
sleep 1
killall lvglsim 2>/dev/null
rm -f /var/run/lvgl.pid
/etc/init.d/S05lvgl start
