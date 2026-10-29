/**
 * @file async_store.h
 * 把「读写用户表 / 日志」包装成后台作业，界面调用时不会卡。
 *
 * 为什么要有这一层：磁盘读写和 PIN 哈希都慢，必须在 worker 线程做；
 * 而界面只能主线程碰。这一层就是中间人——界面调函数提交作业，
 * 结果通过回调送回主线程。
 *
 * 三条约定：
 *   1) 所有回调都在【主线程】worker_poll() 里执行，回调里可以放心改界面；
 *   2) 回调收到的 list（用户表 / 日志）由框架在回调返回后释放，
 *      不要把它存起来下次再用；
 *   3) 涉及「读出来 → 改 → 写回去」的操作一律整段交给 worker 做，
 *      不要在主线程读完再让 worker 写（两次操作之间可能被别的写插进来）。
 */
#pragma once
#include "core/store/store.h"
#include "core/auth/auth_verify.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*astore_users_cb_t)(safe_user_t * list, int count);   /* list 回调后由框架释放 */
typedef void (*astore_logs_cb_t)(log_entry_t * list, int count);    /* list 回调后由框架释放 */
typedef void (*astore_int_cb_t)(int result);                        /* 0=成功，其它见各函数说明 */
typedef void (*astore_str_cb_t)(int result, const char * str);      /* 带一个字符串结果 */

/* 读全部用户（列表页、详情页、主页统计用）。 */
void astore_load_users(astore_users_cb_t cb);

/* 上一次加载是不是**失败**了（而不是「确实一个用户都没有」）。
 * 两种情况回调都收到 (NULL, 0)，界面必须靠这两个函数区分，
 * 否则会把「读失败」显示成空列表，还可能引导用户去覆盖已有数据。 */
bool astore_users_load_failed(void);
bool astore_logs_load_failed(void);

/* 查日志。evt_filter 传 NULL 表示不按事件类型过滤，res_filter < 0 表示不按结果过滤。 */
void astore_query_log(const char * evt_filter, int res_filter, astore_logs_cb_t cb);

/* 追加一条日志：只管写，不要结果。 */
void astore_append_log(const char * evt, const char * user, int res, const char * detail);

/* 用户 / 凭据变更后调这个：它会广播「用户数据变了」，让用户页刷新、远程通道上报。
 * 所有增删改的地方都应该调一次；它和 astore_append_log 是两件事（一个通知界面、
 * 一个写本地日志），通常成对出现，但互不影响——上报失败不会阻塞本地操作。 */
void astore_report_user_event(const char * evt, const char * user, int res, const char * detail);

/* 校验管理员 PIN（改设置前的二次确认）。结果：0=通过 1=错误 2=锁定 -1=无此用户。 */
void astore_verify_admin(const char * pin, astore_int_cb_t cb);

/* 校验管理员动态码（敏感操作的二次确认）。
 * 结果：0=通过 1=错误 2=该码已用过（防重放）。 */
void astore_admin_verify_totp(const char * code, astore_int_cb_t cb);

/* ---------------- 修改用户 ----------------
 *
 * 改名 / 改 PIN / 改角色 / 改临时限制，全部合成一次作业、一次写盘：
 * 任何一项校验不通过就整单作废，不会出现「名字改了但 PIN 没改成」的半成品。
 * 结果码见下面的 modify_result_t，界面直接按它显示对应文案。 */
typedef enum {
    MODIFY_OK            =  0,   /* 成功 */
    MODIFY_ERR_OLDPIN    =  1,   /* 旧 PIN 不对 */
    MODIFY_ERR_DUPNAME   =  2,   /* 新用户名已存在 */
    MODIFY_ERR_LAST_ADMIN=  3,   /* 至少要留一个启用中的管理员 */
    MODIFY_ERR_ROLE      =  4,   /* 角色不合法 */
    MODIFY_ERR_LIMIT     =  5,   /* 次数上限必须大于已用次数 */
    MODIFY_ERR_STORE     = -1,   /* 没这个用户，或读写文件失败 */
} modify_result_t;

typedef struct {
    int     id;
    char    new_name[32];   /* 空串 = 不改名 */
    char    old_pin[16];    /* 要改 PIN 时必填：先证明你知道旧 PIN */
    char    new_pin[16];    /* 空串 = 不改 PIN */
    char    role[16];       /* 空串 = 不改角色；合法值 admin / user / temp */
    int64_t valid_until;    /* 改成 temp 时生效（0 = 不限时间）；非 temp 会被清零 */
    int     use_limit;      /* 同上，临时用户可用次数 */
    int     result;         /* 输出：上面那个结果码 */
} modify_user_req_t;

/* 提交「修改用户」作业。req 里的字符串会被拷走，调用后可以立即复用或释放。
 * cb 在主线程收到结果，值和 req->result 一样。 */
void astore_modify_user(const modify_user_req_t * req, astore_int_cb_t cb);

/* 读取 / 保存 WiFi 密码（内部加密存放）。 */
void astore_net_get_psk(const char * ssid, astore_str_cb_t cb);
void astore_net_add_wifi(const char * ssid, const char * sec, const char * psk, astore_int_cb_t cb);

/* 保存安全策略（会重写 users.json）。 */
void astore_set_policy(int max_failed, int lock_seconds, astore_int_cb_t cb);

/* ---------------- 认证过程中要改用户表的那几种情况 ----------------
 *
 * 认证不只是「比对一下」，还要回写用户表：
 *   - 失败一次：失败次数 +1，达到上限就锁定；
 *   - 开锁成功：失败次数清零；临时用户已用次数 +1，用完了删号；
 *   - 人脸凭据和本地记录不一致：把本地的人脸通道关掉；
 *   - 临时授权过期 / 用完：删掉这个用户。
 *
 * 这些都必须整段在 worker 里做「读 → 改 → 写」。若在主线程读、worker 写，
 * 连续两次失败会各自基于同一份旧数据计算，最后只 +1，失败次数永远攒不满。
 * worker 是单线程先进先出，所以前一个作业写完，后一个才开始，顺序有保证。 */
typedef enum {
    AUTH_SIDE_FAIL_BUMP = 1,     /* 失败次数 +1，达到 max_failed 就锁定 */
    AUTH_SIDE_UNLOCK_TOUCH,      /* 开锁成功：清失败计数；临时用户计数，用完则删号 */
    AUTH_SIDE_FACE_INVALIDATE,   /* 把人脸通道标记为失效 */
    AUTH_SIDE_TEMP_REVOKE,       /* 临时授权失效，删号 */
} auth_side_op_t;

/**
 * 认证副作用的完成回调（在**主线程**执行）。
 *   result      0 = 成功；-1 = 没这个用户或写盘失败（只作提示，不改变认证结论）
 *   face_id_rm  >= 0 表示这次删号顺带出了一个模组模板号，调用方应在主线程调
 *               hal_face_delete_async(face_id_rm) 把它从模组里删掉；< 0 表示不用删
 *   removed     true = 这个用户已经被删掉了
 */
typedef void (*astore_auth_side_cb_t)(int result, int32_t face_id_rm, bool removed);

/**
 * 提交一个认证副作用作业。cb 可以传 NULL（不需要回执）。
 * user 传 NULL 或空串时直接回调 result=-1（不知道是谁，就没法改）。
 *
 * revoke_detail：只在这次真的删了用户时用（会写一条告警日志），其他情况传 NULL。
 * 这条日志之所以在 worker 里写：主线程只知道「次数用完了」，到底删没删只有 worker 知道，
 * 谁判定谁记录，才不会出现「没删成却记了一笔」。
 */
void astore_auth_side(auth_side_op_t op, const char *user, const char *revoke_detail,
                      astore_auth_side_cb_t cb);

#ifdef __cplusplus
} /*extern "C"*/
#endif
