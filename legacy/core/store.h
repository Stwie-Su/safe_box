/**
 * @file store.h
 * 数据存储抽象层（DESIGN.md §4）：业务代码只调本层接口，
 * 不关心数据落在 JSON 文件还是将来的 SQLite / 独立分区。
 *
 * 数据目录：PC 阶段 = 可执行文件上级/../src/safe/（免权限）；
 *           开发板 = /var/lib/safe/（CMake 传 SAFE_DIR_DEVICE）。
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 安全策略（users.json 顶层 policy） ---------------- */
typedef struct {
    int pin_min_len;    /* PIN 最短长度 */
    int pin_max_len;    /* PIN 最长长度 */
    int max_failed;     /* 连续失败锁定阈值 */
    int lock_seconds;   /* 锁定时长（秒） */
    int score_high;     /* 置信度 ≥ 此值直接开锁（FR-2） */
    int score_mid;      /* 置信度 ≥ 此值走动态码；< 此值直接拒绝（FR-2） */
} safe_policy_t;

/* ---------------- 用户数据模型（DESIGN.md §2.2，阶段 1 扩展） ---------------- */
typedef struct {
    int     id;
    char    name[32];       /* 用户名（字母数字） */
    char    role[16];       /* "admin" / "user" / "temp" */
    char    pin_hash[65];   /* PBKDF2-SHA256 hex（64 字符） */
    char    pin_salt[33];   /* 16 字节随机盐 hex（32 字符） */
    char    auth_method[16];/* "pin"，预留 "otp" */
    bool    enabled;
    char    created_at[24]; /* ISO8601 */
    int     failed_attempts;
    int64_t lock_until;     /* Unix 秒时间戳，0=未锁定 */

    /* —— 阶段 1 新增（FR-1 三通道 / FR-4 / FR-9 临时授权）—— */
    int     face_id;            /* FM225 注册 ID；-1 = 未录入人脸 */
    bool    face_enable;        /* 管理员开关：人脸通道是否可用 */
    char    totp_secret[36];    /* Base32 密钥（16 字节 → 26 字符 + '\0'）；空=未绑定 */
    bool    totp_enable;        /* 管理员开关：动态密码通道是否可用 */
    int64_t last_otp_counter;   /* TOTP 防重放：上次成功使用过的时间片序号 */
    int64_t valid_until;        /* 临时用户有效期截止（Unix 秒，0 = 不限） */
    int     use_limit;          /* 临时用户开锁次数上限（0 = 不限） */
    int     used_count;         /* 临时用户已开锁次数 */
} safe_user_t;

/* ---------------- 日志条目（safe.log，JSON Lines） ---------------- */
typedef struct {
    char ts[24];        /* ISO8601 时间 */
    char evt[24];       /* unlock / unlock_fail / user_add / ... */
    char user[32];
    int  res;           /* 1=ok 0=fail */
    char detail[96];
} log_entry_t;

/* ---------------- 存储目录 ---------------- */
void store_set_dir(const char *dir);      /* 覆盖数据目录（默认编译期规则） */
const char * store_dir(void);
bool store_init(void);                    /* 确保目录存在；无 users.json 时建默认 admin */

/* ---------------- 用户接口（DESIGN.md §4.3） ---------------- */
int user_load_all(safe_user_t **list, int *count);       /* 0=成功；*list 用 user_list_free 释放 */
int user_add(const safe_user_t *u);                      /* 0=成功；失败计数/锁定状态忽略 */
int user_del(int id);                                    /* 0=成功 */
int user_update(const safe_user_t *u);                   /* 按 id 整体覆盖；0=成功 */
int user_find_by_name(const char *name, safe_user_t *out);/* 0=找到 */
int user_find_by_id(int id, safe_user_t *out);            /* 0=找到 */
int user_find_by_face(int face_id, safe_user_t *out);     /* 0=找到（face_id>=0 且已录入） */
int user_verify_pin(const char *name, const char *pin);  /* 0=通过 1=错误 2=已锁定 -1=无此用户 */
int pin_hash(const char *pin, uint8_t *salt_out, char *hash_hex_out);  /* 生成新哈希，0=成功 */
int pin_check(const char *pin, const char *salt_hex, const char *hash_hex); /* 0=匹配 */
const safe_policy_t * user_policy(void);                 /* 返回当前策略指针（内部静态） */
void user_policy_set(int max_failed, int lock_seconds);   /* 修改安全策略并落盘 */
void user_policy_set_score(int high, int mid);            /* 修改置信度阈值（FR-2）并落盘 */
int  user_next_id(void);                                  /* 分配下一个用户 id */
void user_list_free(safe_user_t *list);

/* ---------------- 网络（psk 可逆加密，DESIGN.md §4.3） ---------------- */
int net_add_wifi(const char *ssid, const char *sec, const char *psk);  /* 0=成功 */
int net_get_psk(const char *ssid, char *psk_out, size_t cap);          /* 0=成功 */

/* ---------------- 日志（DESIGN.md §3） ---------------- */
int log_append(const char *evt, const char *user, int res, const char *detail); /* 0=成功 */
/* evt_filter 为 NULL 不过滤；res_filter < 0 不过滤。返回 0=成功，*out 需 free。 */
int log_query(const char *evt_filter, int res_filter, log_entry_t **out, int *count);

/* ---------------- 设备密钥（PC 阶段固定串，DESIGN.md §4.3） ---------------- */
int devkey_get(uint8_t *key, size_t *len);   /* 0=成功 */

#ifdef __cplusplus
} /*extern "C"*/
#endif
