#!/bin/sh
# ============================================================
# TLS 证书体系一键生成（FR-25 双向 TLS 闭环）
# ------------------------------------------------------------
# 产出（--out 目录，默认 ./tls_certs）：
#   ca.crt / ca.key              自签 CA（ECDSA P-256，10 年）
#   broker.crt / broker.key      broker 服务端证书（SAN 含板端实际
#                                连接地址：PC portproxy IP + VM IP + 回环）
#   device.crt / device.key      设备（板端）客户端证书
#   phone.crt / phone.key        手机/PC 测试客户端证书
#
# 设计要点：
#   1. SAN 必须覆盖**板端连接字符串**：板子连 10.218.148.51:8883
#      （PC netsh portproxy → VM 192.168.150.139:8883），mbedTLS 校验
#      的是连接目标地址，缺这个 SAN 会 verify 失败。
#   2. ECDSA P-256 + SHA-256：mbedTLS 2.28 与 mosquitto(OpenSSL 3.0)
#      的最大公约数，无互操作坑。
#   3. 私钥文件权限 600；CA key 保留在生成机上（吊销即换 CA 重签）。
# 复现：sh gen_certs.sh [输出目录]
# ============================================================
set -e
OUT=${1:-tls_certs}
mkdir -p "$OUT"
cd "$OUT"

DAYS_CA=3650
DAYS_LEAF=825

gen_ec_key() { openssl ecparam -name prime256v1 -genkey -noout -out "$1"; }

# ---------- CA ----------
if [ ! -f ca.key ]; then
    gen_ec_key ca.key
    openssl req -x509 -new -key ca.key -days "$DAYS_CA" -sha256 \
        -subj "/C=CN/O=SafeCabinet/CN=SafeCabinet Root CA" \
        -out ca.crt
    echo "[OK] ca"
fi

# ---------- broker（SAN 覆盖三种连接路径）----------
if [ ! -f broker.crt ]; then
    gen_ec_key broker.key
    openssl req -new -key broker.key \
        -subj "/C=CN/O=SafeCabinet/CN=safe-broker" -out broker.csr
    printf "subjectAltName=IP:10.218.148.51,IP:10.163.32.202,IP:192.168.150.139,IP:127.0.0.1,DNS:localhost\n" \
        > broker.ext
    openssl x509 -req -in broker.csr -CA ca.crt -CAkey ca.key \
        -CAcreateserial -days "$DAYS_LEAF" -sha256 \
        -extfile broker.ext -out broker.crt
    rm -f broker.csr broker.ext
    echo "[OK] broker（SAN: 10.218.148.51 / 192.168.150.139 / 127.0.0.1 / localhost）"
fi

# ---------- 设备客户端证书 ----------
if [ ! -f device.crt ]; then
    gen_ec_key device.key
    openssl req -new -key device.key \
        -subj "/C=CN/O=SafeCabinet/CN=safe-device" -out device.csr
    openssl x509 -req -in device.csr -CA ca.crt -CAkey ca.key \
        -CAcreateserial -days "$DAYS_LEAF" -sha256 -out device.crt
    rm -f device.csr
    echo "[OK] device"
fi

# ---------- 手机/测试客户端证书 ----------
if [ ! -f phone.crt ]; then
    gen_ec_key phone.key
    openssl req -new -key phone.key \
        -subj "/C=CN/O=SafeCabinet/CN=test-phone" -out phone.csr
    openssl x509 -req -in phone.csr -CA ca.crt -CAkey ca.key \
        -CAcreateserial -days "$DAYS_LEAF" -sha256 -out phone.crt
    rm -f phone.csr
    echo "[OK] phone"
fi

chmod 600 ./*.key
echo "==== 证书清单 ===="
openssl x509 -in ca.crt -noout -subject -dates
openssl x509 -in broker.crt -noout -subject -ext subjectAltName
openssl x509 -in device.crt -noout -subject
openssl x509 -in phone.crt -noout -subject
echo "DONE -> $PWD"
