/**
 * @file store.h
 * 最底层的数据读写：用户表、日志、网络密码，都落到磁盘文件上。
 *
 * 数据放在哪：
 *   PC   = 可执行文件旁边的 src/safe/（省得配权限）
 *   板子 = /var/lib/safe/
 *
 * ★ 这些函数都是**同步读写磁盘**的，会阻塞。界面代码不要直接调它们
 *   （会把画面卡住），请走 async_store 那一层的异步包装，让 worker 线程去读写。
 *   只有 user_policy() 这种「读内存快照」的可以随时调。
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 虚位密码开启时，一次输入最多接受多少位。
 * 超过这个长度直接判失败（不参与比对）。没开虚位时用策略里的 pin_max_len。
 * 键盘页和本层共用这一个上限，不要各写一个数。 */
#define SAFE_VIRTUAL_PIN_MAX_INPUT 12

/* ---------------- 安全策略（存在 users.json 的 policy 里） ---------------- */
typedef struct {
    int  pin_min_len;        /* PIN 最短几位 */
    int  pin_max_len;        /* PIN 最长几位 */
    int  max_failed;         /* 连续失败几次锁定 */
    int  lock_seconds;       /* 锁多久（秒） */
    int  face_otp_after;     /* 人脸连续没认出来几次，转输动态码 */
    int  face_verify_timeout_s; /* 人脸验证最多等几秒 */
    bool virtual_pin_enable; /* 虚位密码开关 */
    /* 录入人脸时是采一帧还是采五个方向（正/左/右/上/下）。
     * 默认单帧；五向需要真机确认效果，所以做成可切换、随时能退回去。 */
    bool enroll_five_way;
} safe_policy_t;

/* ---------------- 一个用户 ---------------- */
typedef struct {
    int     id;
    char    name[32];       /* 用户名（字母数字） */
    char    role[16];       /* "admin" / "user" / "temp" */
    char    pin_hash[65];   /* PIN 的哈希（64 个十六进制字符），不存明文 */
    char    pin_salt[33];   /* 盐（32 个十六进制字符） */
    char    auth_method[16];/* "pin"，以后可能加 "otp" */
    bool    enabled;
    char    created_at[24]; /* 创建时间，ISO8601 */
    int     failed_attempts;
    int64_t lock_until;     /* 锁定到什么时候（Unix 秒），0 = 没锁 */

    /* 三条认证通道与临时授权 */
    int     face_id;            /* 模组里对应的人脸编号；-1 = 没录 */
    bool    face_enable;        /* 人脸通道开关 */
    char    totp_secret[36];    /* 动态码密钥（Base32），空 = 没绑定 */
    bool    totp_enable;        /* 动态码通道开关 */
    int64_t last_otp_counter;   /* 上次用掉的时间片，防止同一个码用两次 */
    int64_t valid_until;        /* 临时用户有效期（Unix 秒），0 = 不限 */
    int     use_limit;          /* 临时用户最多开几次锁，0 = 不限 */
    int     used_count;         /* 临时用户已经开了几次 */
} safe_user_t;

/* ---------------- 一条日志（safe.log，一行一个 JSON） ---------------- */
typedef struct {
    char ts[24];        /* 时间，ISO8601 */
    char evt[24];       /* unlock / unlock_fail / user_add / ... */
    char user[32];
    int  res;           /* 1 = 成功，0 = 失败 */
    char detail[96];
} log_entry_t;

/* ---------------- 目录 ---------------- */
void store_set_dir(const char *dir);      /* 换数据目录（默认按编译期规则） */
const char * store_dir(void);
bool store_init(void);                    /* 确保目录存在；没有 users.json 就建一个默认管理员 */

/* ---------------- 用户（同步读写文件，慢） ---------------- */
int user_load_all(safe_user_t **list, int *count);       /* 0=成功；*list 用 user_list_free 释放 */
int user_add(const safe_user_t *u);                      /* 0=成功 */
int user_del(int id);                                    /* 0=成功 */

/* 删用户，同时把人脸模板号带出来，让上层去模组里删。
 * out_face_id 可以传 NULL（只删本地）。
 * 只有「这个模板号确实绑在要删的用户身上」才会带出来；如果本地记录已经变了
 * （这个号已经属于别人），就返回 -1 并记一条日志 —— 宁可在模组里留个孤儿模板，
 * 也不能把别人的模板删掉。 */
int user_del_cascade(int id, int32_t *out_face_id);       /* 0=成功 */

/* 把所有用户的人脸绑定清掉（模组被人整体清空后用），没有改动就不写盘。 */
int user_face_clear_all(void);
int user_update(const safe_user_t *u);                   /* 按 id 整体覆盖；0=成功 */
int user_rename(int id, const char *newname);            /* 改名，重名返回 -2；0=成功 */
int user_face_set(int user_id, int face_id);             /* 绑模板(>=0，同时开人脸通道) / 解绑(-1)；0=成功 */
int user_find_by_name(const char *name, safe_user_t *out);/* 0=找到 */
int user_find_by_id(int id, safe_user_t *out);            /* 0=找到 */
int user_find_by_face(int face_id, safe_user_t *out);     /* 0=找到 */
int user_verify_pin(const char *name, const char *pin);  /* 0=通过 1=错误 2=已锁定 -1=无此用户 */
int pin_hash(const char *pin, uint8_t *salt_out, char *hash_hex_out);  /* 生成新哈希，0=成功 */
int pin_check(const char *pin, const char *salt_hex, const char *hash_hex); /* 0=匹配 */
const safe_policy_t * user_policy(void);                 /* 当前策略（内部静态，读内存，很快） */
void user_policy_set(int max_failed, int lock_seconds);   /* 改策略并写盘 */
void user_policy_set_face(int otp_after, int timeout_s);  /* 改人脸策略并写盘 */
/* 虚位密码开关并写盘。注意实现顺序是「先读文件同步，再改内存，最后写」：
 * 如果先改内存再读文件，刚改的值会被文件里的旧值覆盖。 */
void user_policy_set_enroll_mode(bool five_way);  /* 录入模式（单帧 / 五向）并写盘 */
void user_policy_set_virtual_pin(bool enable);
/* 角色名是否合法：只认 "admin" / "user" / "temp"。 */
bool user_role_valid(const char * role);
int  user_next_id(void);                                  /* 分配下一个用户 id */
void user_list_free(safe_user_t *list);

/* ---------------- 网络（WiFi 密码加密存放） ---------------- */
int net_add_wifi(const char *ssid, const char *sec, const char *psk);  /* 0=成功 */
int net_get_psk(const char *ssid, char *psk_out, size_t cap);          /* 0=成功 */

/* ---------------- 日志 ---------------- */
int log_append(const char *evt, const char *user, int res, const char *detail); /* 0=成功 */
/* evt_filter 传 NULL 不按类型过滤；res_filter < 0 不按结果过滤。*out 需要 free。 */
int log_query(const char *evt_filter, int res_filter, log_entry_t **out, int *count);

/* ---------------- 设备密钥（PC 阶段是固定串） ---------------- */
int devkey_get(uint8_t *key, size_t *len);   /* 0=成功 */

#ifdef __cplusplus
} /*extern "C"*/
#endif
