/**
 * @file test_crypto.c
 * 密码学原语单测（QA 补缺，2026-10-06）：
 *   1) SHA-1 / HMAC-SHA1 —— RFC 3174 与 RFC 2202 **标准测试向量**；
 *   2) crypto_pbkdf2_sha256 —— PBKDF2-HMAC-SHA256 标准向量（iter=1/2/4096）；
 *   3) crypto 认证加密层 —— 加解密往返、语义安全（密文不重复）、
 *      篡改检测（IV / SALT / MAC / 密文 / MAGIC）、格式错误与容量边界。
 *
 * ★ 期望值**不是**从本实现抄回来的自证：全部由 Python hashlib / hmac
 *   （与 OpenSSL 同源的参考实现）在验收时独立算出后固化进本文件。
 *   因此若 AES / SHA1 / PBKDF2 的实现被改坏，本用例会真失败。
 *
 * 只链 safe_core + safe_thirdparty，不碰 LVGL / 网络 / 文件。
 */
#include "test_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/support/sha1.h"
#include "core/support/crypto.h"

/* ----------------------------- 工具 ----------------------------- */
static void hex_of(const uint8_t * p, size_t n, char * out)
{
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i]     = H[(p[i] >> 4) & 0x0f];
        out[2 * i + 1] = H[p[i] & 0x0f];
    }
    out[2 * n] = '\0';
}

static bool digest20_eq(const uint8_t d[SHA1_DIGEST_LEN], const char * hex)
{
    char buf[SHA1_DIGEST_LEN * 2 + 1];
    hex_of(d, SHA1_DIGEST_LEN, buf);
    return strcmp(buf, hex) == 0;
}

/* --------------------------- SHA-1（RFC 3174） --------------------------- */
static void test_sha1(void)
{
    uint8_t d[SHA1_DIGEST_LEN];

    sha1_bytes("", 0, d);
    CHECK(digest20_eq(d, "da39a3ee5e6b4b0d3255bfef95601890afd80709"));

    sha1_bytes("abc", 3, d);
    CHECK(digest20_eq(d, "a9993e364706816aba3e25717850c26c9cd0d89d"));

    sha1_bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, d);
    CHECK(digest20_eq(d, "84983e441c3bd26ebaae4aa1f95129e5e54670f1"));

    /* 一百万个 'a'：顺带压 uint64 位计数与跨块边界 */
    {
        uint8_t chunk[1024];
        memset(chunk, 'a', sizeof(chunk));
        sha1_ctx_t c;
        sha1_init(&c);
        for (int i = 0; i < 976; i++) sha1_update(&c, chunk, sizeof(chunk));  /* 999424 */
        sha1_update(&c, chunk, 576);                                          /* +576 = 1000000 */
        sha1_final(&c, d);
        CHECK(digest20_eq(d, "34aa973cd4c4daa4f61eeb2bdbad27316534016f"));
    }

    /* 分块一致性：一次性计算 == 分三段 update（长度横跨 55/56/64/65 等块边界） */
    {
        const int lens[] = {1, 55, 56, 63, 64, 65, 119, 120, 127, 128, 1000};
        for (size_t k = 0; k < sizeof(lens) / sizeof(lens[0]); k++) {
            int len = lens[k];
            uint8_t * buf = (uint8_t *)malloc((size_t)len);
            if (!buf) continue;
            unsigned x = 12345u;
            for (int i = 0; i < len; i++) {
                x = x * 1103515245u + 12345u;
                buf[i] = (uint8_t)(x >> 16);
            }
            uint8_t d1[SHA1_DIGEST_LEN], d2[SHA1_DIGEST_LEN];
            sha1_bytes(buf, (size_t)len, d1);

            sha1_ctx_t c;
            sha1_init(&c);
            int cut1 = len / 3, cut2 = (2 * len) / 3;
            sha1_update(&c, buf, (size_t)cut1);
            sha1_update(&c, buf + cut1, (size_t)(cut2 - cut1));
            sha1_update(&c, buf + cut2, (size_t)(len - cut2));
            sha1_final(&c, d2);

            CHECK(memcmp(d1, d2, SHA1_DIGEST_LEN) == 0);
            free(buf);
        }
    }

    /* 空 update 与零长度输入不得破坏状态 */
    {
        sha1_ctx_t c;
        sha1_init(&c);
        sha1_update(&c, "abc", 0);
        sha1_update(&c, "abc", 3);
        sha1_update(&c, "xyz", 0);
        sha1_final(&c, d);
        CHECK(digest20_eq(d, "a9993e364706816aba3e25717850c26c9cd0d89d"));
    }
}

/* ------------------------ HMAC-SHA1（RFC 2202） ------------------------ */
static void test_hmac_sha1(void)
{
    uint8_t d[SHA1_DIGEST_LEN];

    /* case 1：key = 0x0b × 20，data = "Hi There" */
    {
        uint8_t key[20]; memset(key, 0x0b, sizeof(key));
        hmac_sha1(key, sizeof(key), "Hi There", 8, d);
        CHECK(digest20_eq(d, "b617318655057264e28bc0b6fb378c8ef146be00"));
    }

    /* case 2：key = "Jefe" */
    {
        const char * msg = "what do ya want for nothing?";
        hmac_sha1("Jefe", 4, msg, strlen(msg), d);
        CHECK(digest20_eq(d, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"));
    }

    /* case 3：key = 0xaa × 20，data = 0xdd × 50 */
    {
        uint8_t key[20]; memset(key, 0xaa, sizeof(key));
        uint8_t msg[50]; memset(msg, 0xdd, sizeof(msg));
        hmac_sha1(key, sizeof(key), msg, sizeof(msg), d);
        CHECK(digest20_eq(d, "125d7342b9ac11cd91a39af48aa17b4f63f175d3"));
    }

    /* case 6：key 长于 64 字节（须先哈希再填充） */
    {
        uint8_t key[80]; memset(key, 0xaa, sizeof(key));
        const char * msg = "Test Using Larger Than Block-Size Key - Hash Key First";
        hmac_sha1(key, sizeof(key), msg, strlen(msg), d);
        CHECK(digest20_eq(d, "aa4ae5e15272d00e95705637ce8a3b55ed402112"));
    }

    /* case 7：key 长于 64 且数据跨多个块 */
    {
        uint8_t key[80]; memset(key, 0xaa, sizeof(key));
        const char * msg = "Test Using Larger Than Block-Size Key and Larger Than One Data Block";
        hmac_sha1(key, sizeof(key), msg, strlen(msg), d);
        CHECK(digest20_eq(d, "f47fbddec305ad37fccbc5dced519fc5db07424c"));
    }

    /* 空 key + 空消息：零填充路径不得越界读，结果须与参考实现一致 */
    {
        hmac_sha1("", 0, "", 0, d);
        CHECK(digest20_eq(d, "fbdb1d1b18aa6c08324b7d64b71fb76370690e1d"));
    }

    /* 正好 64 字节 key（不触发「先哈希」分支的边界） */
    {
        uint8_t key[64]; memset(key, 0x5a, sizeof(key));
        uint8_t ref[SHA1_DIGEST_LEN];
        /* 参考值：与 64 字节 key 未哈希直填等价，用 HMAC 定义自校验：
         * HMAC(k,m) = H((k^opad) || H((k^ipad) || m))，此处用 sha1 手工复算。 */
        uint8_t k0[64], ipad[64 + 8], opad[64 + SHA1_DIGEST_LEN], inner[SHA1_DIGEST_LEN];
        memcpy(k0, key, 64);
        memcpy(ipad, k0, 64);
        for (int i = 0; i < 64; i++) ipad[i] = (uint8_t)(k0[i] ^ 0x36);
        memcpy(ipad + 64, "secret!", 7);
        sha1_bytes(ipad, 64 + 7, inner);
        memcpy(opad, k0, 64);
        for (int i = 0; i < 64; i++) opad[i] = (uint8_t)(k0[i] ^ 0x5c);
        memcpy(opad + 64, inner, SHA1_DIGEST_LEN);
        sha1_bytes(opad, 64 + SHA1_DIGEST_LEN, ref);

        hmac_sha1(key, 64, "secret!", 7, d);
        CHECK(memcmp(d, ref, SHA1_DIGEST_LEN) == 0);
    }
}

/* --------------------- PBKDF2-HMAC-SHA256 标准向量 --------------------- */
static void test_pbkdf2(void)
{
    uint8_t dk[32];
    char hex[65];

    crypto_pbkdf2_sha256((const uint8_t *)"password", 8, (const uint8_t *)"salt", 4, 1, dk, 32);
    hex_of(dk, 32, hex);
    CHECK(strcmp(hex, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b") == 0);

    crypto_pbkdf2_sha256((const uint8_t *)"password", 8, (const uint8_t *)"salt", 4, 2, dk, 32);
    hex_of(dk, 32, hex);
    CHECK(strcmp(hex, "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43") == 0);

    crypto_pbkdf2_sha256((const uint8_t *)"password", 8, (const uint8_t *)"salt", 4, 4096, dk, 32);
    hex_of(dk, 32, hex);
    CHECK(strcmp(hex, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a") == 0);

    /* 短派生必须是完整派生的**前缀**（否则 pin_hash 与 pin_check 会不一致） */
    {
        uint8_t full[32], part[16];
        crypto_pbkdf2_sha256((const uint8_t *)"password", 8, (const uint8_t *)"salt", 4, 4096, full, 32);
        crypto_pbkdf2_sha256((const uint8_t *)"password", 8, (const uint8_t *)"salt", 4, 4096, part, 16);
        CHECK(memcmp(full, part, 16) == 0);
    }

    /* 迭代次数不同 → 结果必须不同（防止 iter 参数被悄悄忽略） */
    {
        uint8_t a[32], b[32];
        crypto_pbkdf2_sha256((const uint8_t *)"pw", 2, (const uint8_t *)"salt", 4, 1, a, 32);
        crypto_pbkdf2_sha256((const uint8_t *)"pw", 2, (const uint8_t *)"salt", 4, 2, b, 32);
        CHECK(memcmp(a, b, 32) != 0);
    }

    /* salt 不同 → 结果必须不同（防止 salt 参数被悄悄忽略） */
    {
        uint8_t a[32], b[32];
        crypto_pbkdf2_sha256((const uint8_t *)"pw", 2, (const uint8_t *)"salt1", 5, 100, a, 32);
        crypto_pbkdf2_sha256((const uint8_t *)"pw", 2, (const uint8_t *)"salt2", 5, 100, b, 32);
        CHECK(memcmp(a, b, 32) != 0);
    }
}

/* ----------------------- crypto 认证加密层 ----------------------- */
static void test_crypto_blob(void)
{
    uint8_t blob[CRYPTO_BLOB_MAX];
    uint8_t work[CRYPTO_BLOB_MAX];
    char    out[128];
    const size_t header = 4 + CRYPTO_IVLEN + CRYPTO_SALTLEN + CRYPTO_MACLEN;   /* 68 */

    /* 最长明文（64 字节）也要装得进 CRYPTO_BLOB_MAX */
    CHECK(header + (CRYPTO_PLAIN_MAX + CRYPTO_BLOCKLEN) <= CRYPTO_BLOB_MAX);

    /* 1) 往返：长度覆盖块边界与上限 */
    {
        const int lens[] = {1, 8, 15, 16, 17, 31, 32, 47, 48, 63, 64};
        for (size_t k = 0; k < sizeof(lens) / sizeof(lens[0]); k++) {
            int L = lens[k];
            char plain[128];
            for (int i = 0; i < L; i++) plain[i] = (char)('0' + (i % 10));
            plain[L] = '\0';

            size_t n = crypto_encrypt(plain, blob, sizeof(blob));
            size_t want = header + (size_t)((L / CRYPTO_BLOCKLEN + 1) * CRYPTO_BLOCKLEN);
            CHECK(n == want);

            memset(out, 0x5a, sizeof(out));
            CHECK(crypto_decrypt(blob, n, out, sizeof(out)) == true);
            CHECK(strcmp(out, plain) == 0);
            CHECK(strlen(out) == (size_t)L);
        }
    }

    /* 2) 特殊字符（非字母数字、含 0x7f）往返一致 */
    {
        const char * pw = "p@ss w0rd!\x01\x7f~";
        size_t n = crypto_encrypt(pw, blob, sizeof(blob));
        CHECK(n > header);
        CHECK(crypto_decrypt(blob, n, out, sizeof(out)) == true);
        CHECK(strcmp(out, pw) == 0);
    }

    /* 3) 明文长度边界：空串与超上限（> 64）均拒绝 */
    {
        CHECK(crypto_encrypt("", blob, sizeof(blob)) == 0);

        char p64[65], p65[66];
        memset(p64, 'x', 64); p64[64] = '\0';
        memset(p65, 'y', 65); p65[65] = '\0';
        CHECK(crypto_encrypt(p64, blob, sizeof(blob)) == header + 80);
        CHECK(crypto_encrypt(p65, blob, sizeof(blob)) == 0);
    }

    /* 4) 输出缓冲容量边界：恰好放下通过，少 1 字节拒绝 */
    {
        size_t n = crypto_encrypt("0123456789abcdef", blob, sizeof(blob));
        CHECK(n == header + 32);
        CHECK(crypto_encrypt("0123456789abcdef", blob, n)     == n);
        CHECK(crypto_encrypt("0123456789abcdef", blob, n - 1) == 0);
    }

    /* 5) 语义安全：同一明文两次加密密文不同（随机 IV + 随机 SALT） */
    {
        uint8_t b1[CRYPTO_BLOB_MAX], b2[CRYPTO_BLOB_MAX];
        size_t n1 = crypto_encrypt("same-password", b1, sizeof(b1));
        size_t n2 = crypto_encrypt("same-password", b2, sizeof(b2));
        CHECK(n1 == n2);
        CHECK(n1 > header);
        CHECK(memcmp(b1, b2, n1) != 0);
        /* 但两者都能解回同一个明文 */
        char o1[64], o2[64];
        CHECK(crypto_decrypt(b1, n1, o1, sizeof(o1)) == true);
        CHECK(crypto_decrypt(b2, n2, o2, sizeof(o2)) == true);
        CHECK(strcmp(o1, "same-password") == 0);
        CHECK(strcmp(o2, "same-password") == 0);
    }

    /* 6) 篡改检测：改任何一个字段都必须拒绝（不得解密出「看起来像」的明文） */
    {
        size_t n = crypto_encrypt("wifi-psk-1234", blob, sizeof(blob));
        CHECK(crypto_decrypt(blob, n, out, sizeof(out)) == true);

        /* MAGIC */
        memcpy(work, blob, n); work[0] = 'X';
        CHECK(crypto_decrypt(work, n, out, sizeof(out)) == false);
        /* IV（偏移 4） */
        memcpy(work, blob, n); work[4] ^= 0x01;
        CHECK(crypto_decrypt(work, n, out, sizeof(out)) == false);
        /* SALT（偏移 20） */
        memcpy(work, blob, n); work[20] ^= 0x01;
        CHECK(crypto_decrypt(work, n, out, sizeof(out)) == false);
        /* HMAC（偏移 36） */
        memcpy(work, blob, n); work[36] ^= 0x01;
        CHECK(crypto_decrypt(work, n, out, sizeof(out)) == false);
        /* 密文首字节（偏移 68） */
        memcpy(work, blob, n); work[header] ^= 0x01;
        CHECK(crypto_decrypt(work, n, out, sizeof(out)) == false);
        /* 密文末字节（PKCS7 填充区） */
        memcpy(work, blob, n); work[n - 1] ^= 0x01;
        CHECK(crypto_decrypt(work, n, out, sizeof(out)) == false);
        /* 原封不动仍然通过（确认上面不是因为 work 拷贝出错才失败） */
        memcpy(work, blob, n);
        CHECK(crypto_decrypt(work, n, out, sizeof(out)) == true);
    }

    /* 7) 格式错误：长度不足 / 空密文 / 非块对齐 / 空指针 */
    {
        CHECK(crypto_decrypt(blob, 0, out, sizeof(out)) == false);              /* 空输入 */
        CHECK(crypto_decrypt(blob, header - 1, out, sizeof(out)) == false);     /* 短于头 */
        CHECK(crypto_decrypt(blob, header, out, sizeof(out)) == false);         /* 密文长度 0 */
        CHECK(crypto_decrypt(blob, header + 8, out, sizeof(out)) == false);     /* 非 16 倍数 */
        CHECK(crypto_decrypt(NULL, 0, out, sizeof(out)) == false);              /* 空指针 + 0 长 */
    }

    /* 8) 输出缓冲容量：必须留得下明文 + 结尾 '\0' */
    {
        size_t n = crypto_encrypt("12345678", blob, sizeof(blob));
        char small[16];
        memset(small, 0, sizeof(small));
        CHECK(crypto_decrypt(blob, n, small, 8) == false);   /* 需要 8+1 = 9 */
        CHECK(crypto_decrypt(blob, n, small, 9) == true);
        CHECK(strcmp(small, "12345678") == 0);
    }
}

/* ------------------- 边界回归（efc7d68 修掉的三处越界） -------------------
 * 这三处**在修复前无法写进用例** —— 断言它们就等于主动触发栈溢出 / NULL 解引用。
 * 修复后才锁得住，属「先修后锁」：不锁的话同一处将来会被二次改坏。 */
static void test_crypto_boundaries(void)
{
    uint8_t blob[CRYPTO_BLOB_MAX];
    char    out[128];
    const size_t header = 4 + CRYPTO_IVLEN + CRYPTO_SALTLEN + CRYPTO_MACLEN;  /* 68 */
    const size_t ct_max = (size_t)(CRYPTO_PLAIN_MAX + CRYPTO_BLOCKLEN);       /* 80 */

    /* ① ct_len 上界：> 80 必须拒绝。
     *    下游 macdata / pt / plain 都是 80 字节**栈**数组；修复前只查 16 对齐，
     *    不查上界 —— 换任何一个 in_len 更大的调用点就是栈溢出。 */
    {
        uint8_t big[256];
        memset(big, 0, sizeof(big));
        size_t n = crypto_encrypt("0123456789abcdef", blob, sizeof(blob));
        CHECK(n == header + 32);
        memcpy(big, blob, n);            /* 后续字节任意：上界检查须先于 HMAC 生效 */
        CHECK(header + 96 <= sizeof(big));
        CHECK(crypto_decrypt(big, header + 96, out, sizeof(out)) == false);

        /* 另一侧：恰好等于上界（80）的**合法**密文不得被上界判断误拒 */
        char p64[65];
        memset(p64, 'x', 64);
        p64[64] = '\0';
        size_t n64 = crypto_encrypt(p64, blob, sizeof(blob));
        CHECK(n64 == header + ct_max);
        CHECK(crypto_decrypt(blob, n64, out, sizeof(out)) == true);
        CHECK(strlen(out) == 64);
    }

    /* ② NULL 入参：必须先于任何解引用返回 false。
     *    修复前 in_len >= header 时会 memcmp(in,"SFP1",4) / memcpy 到 NULL。 */
    {
        size_t n = crypto_encrypt("0123456789abcdef", blob, sizeof(blob));
        CHECK(crypto_decrypt(NULL, n,   out,  sizeof(out)) == false);
        CHECK(crypto_decrypt(blob, n,   NULL, sizeof(out)) == false);
        CHECK(crypto_decrypt(NULL, 0,   out,  sizeof(out)) == false);
        CHECK(crypto_decrypt(blob, n,   out,  0)           == false);
    }

    /* ③ pbkdf2 长盐钳制：saltlen > 16 时只取前 16 字节，
     *    不得越界写 saltblk[saltlen+3]，也不得把盐整个丢弃。 */
    {
        uint8_t salt16[16], salt17[17], salt20[20];
        for (int i = 0; i < 20; i++) {
            uint8_t v = (uint8_t)(0x10 + i);
            if (i < 16) salt16[i] = v;
            if (i < 17) salt17[i] = v;
            salt20[i] = v;
        }
        uint8_t d16[32], d17[32], d20[32];
        crypto_pbkdf2_sha256((const uint8_t *)"pw", 2, salt16, 16, 100, d16, 32);
        crypto_pbkdf2_sha256((const uint8_t *)"pw", 2, salt17, 17, 100, d17, 32);
        crypto_pbkdf2_sha256((const uint8_t *)"pw", 2, salt20, 20, 100, d20, 32);
        /* 钳制语义：17 / 20 字节盐的结果 == 前 16 字节盐的结果 */
        CHECK(memcmp(d16, d17, 32) == 0);
        CHECK(memcmp(d16, d20, 32) == 0);

        /* 反向：前 16 字节里改一字节，结果必须不同
         * —— 证明钳的是**长度**（取前缀）而不是把盐整个忽略掉。 */
        uint8_t salt16b[16];
        memcpy(salt16b, salt16, 16);
        salt16b[0] ^= 0xff;
        uint8_t d16b[32];
        crypto_pbkdf2_sha256((const uint8_t *)"pw", 2, salt16b, 16, 100, d16b, 32);
        CHECK(memcmp(d16, d16b, 32) != 0);
    }
}

int main(void)
{
    test_sha1();
    test_hmac_sha1();
    test_pbkdf2();
    test_crypto_blob();
    test_crypto_boundaries();
    TEST_RESULT();
}
