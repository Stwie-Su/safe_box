/**
 * @file test_cred_reconcile.c
 * FR-21 防线 3「启动对账：孤儿凭据标失效」用例（需求 §5）。
 *
 * 场景：本地 2 个用户绑定 face_id=1、2，模组清单只有 [1]
 *   → 只有 face_id=2 的用户被置 face_enable=false；
 *   → face_id=1 不受影响；face_id=-1（未绑定）不动；
 *   → 审计日志有记录；再跑一次无重复动作（幂等）。
 * 另测：清单返回 -1（无此概念）/ -2（尚未取得）时不对账；清单为空（0）时全部绑定凭据皆为孤儿。
 *
 * 说明：store_init() 的 bootstrap 会创建管理员 admin，但**出厂不带人脸绑定**
 * （QA-26：face_id=-1 / face_enable=false；曾经的 face_id=1 是假绑定 —— 模组里
 * 根本没有那个模板，会被对账判成孤儿并让刷脸必失败）。
 * 本用例需要「本地绑定 face_id=1 的用户」，因此**显式**给 admin 建立绑定，
 * 不再依赖 bootstrap 预置（那正是 QA-26 修掉的行为）。
 */
#include "test_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/auth/cred_reconcile.h"
#include "core/store/store.h"

#define TEST_DATA_DIR "/tmp/safe_test_recon"

static void add_user(const char * name, int face_id, bool face_enable)
{
    safe_user_t u;
    memset(&u, 0, sizeof(u));
    u.id = user_next_id();
    strncpy(u.name, name, sizeof(u.name) - 1);
    strcpy(u.role, "user");
    uint8_t salt[16];
    pin_hash("1234", salt, u.pin_hash);
    test_salt_to_hex(salt, u.pin_salt);
    u.enabled     = true;
    u.face_id     = face_id;
    u.face_enable = face_enable;
    CHECK(user_add(&u) == 0);
}

int main(void)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", TEST_DATA_DIR, TEST_DATA_DIR);
    if(system(cmd) != 0) {}
    store_set_dir(TEST_DATA_DIR);
    store_init();     /* bootstrap 建 admin；QA-26 起出厂**不带**人脸绑定 */

    /* 前提自检①：出厂管理员无绑定（把 QA-26 的行为固定下来，防回退） */
    safe_user_t chk;
    CHECK(user_find_by_name("admin", &chk) == 0);
    CHECK(chk.face_id == -1);
    CHECK(chk.face_enable == false);

    /* 前提自检②：显式建立「本地绑定 face_id=1 的用户」 —— 本用例需要它 */
    CHECK(user_face_set(chk.id, 1) == 0);
    CHECK(user_find_by_face(1, &chk) == 0);
    CHECK(chk.face_id == 1);
    CHECK(chk.face_enable == true);   /* 绑定即自动启用通道（不变式） */

    add_user("bob",   2, true);    /* 本地绑定 face_id=2（模组清单里没有 → 孤儿） */
    add_user("carol", -1, true);   /* 故意传不一致态：face_id<0 且 enable=true，
                                      存储层应归一化（user_add 入口不变式） */

    const int32_t mod[1] = { 1 };  /* 模组侧只有 face_id=1 */

    /* ---- 第一次对账：只有 bob（face_id=2）被标失效 ---- */
    int changed = cred_reconcile_apply(mod, 1);
    CHECK(changed == 1);

    CHECK(user_find_by_face(1, &chk) == 0);
    CHECK(chk.face_enable == true);         /* face_id=1（admin）不受影响 */
    CHECK(user_find_by_face(2, &chk) == 0);
    CHECK(chk.face_enable == false);        /* face_id=2 被标失效 */
    CHECK(chk.enabled == true);             /* 只标失效，不删 / 不停用用户 */
    CHECK(user_find_by_name("carol", &chk) == 0);
    CHECK(chk.face_id == -1);
    CHECK(chk.face_enable == false);        /* 构造传 true，但 face_id<0 ⟹ 存储层归一化为
                                               false（不变式）；对账不许翻成 true */

    /* ---- 审计日志有记录（事件名 "ALARM"，与写入侧同大小写） ---- */
    log_entry_t * logs = NULL;
    int ln = 0;
    CHECK(log_query("ALARM", 0, &logs, &ln) == 0);
    CHECK(ln >= 1);
    if(logs) free(logs);

    /* ---- 幂等：再跑一次无重复动作（第二次不再改动任何用户、不新增日志） ---- */
    int changed2 = cred_reconcile_apply(mod, 1);
    CHECK(changed2 == 0);
    {
        log_entry_t * logs2 = NULL;
        int ln2 = 0;
        CHECK(log_query("ALARM", 0, &logs2, &ln2) == 0);
        CHECK(ln2 == ln);                  /* 日志条数不变：无重复记录 */
        if(logs2) free(logs2);
    }

    /* ---- 未取得 / 无此概念（-1 / -2）不执行对账，不误杀 ---- */
    add_user("dave", 5, true);                     /* 新增一个未在清单里的绑定用户 */
    CHECK(cred_reconcile_apply(NULL, -2) == 0);    /* 尚未取得：不执行 */
    CHECK(user_find_by_face(5, &chk) == 0);
    CHECK(chk.face_enable == true);                /* 未被误判失效 */
    CHECK(cred_reconcile_apply(NULL, -1) == 0);    /* 无此概念：不执行 */
    CHECK(user_find_by_face(5, &chk) == 0);
    CHECK(chk.face_enable == true);

    /* ---- 模组清单为空（count=0）是合法值：所有绑定凭据都是孤儿 ---- */
    CHECK(cred_reconcile_apply(NULL, 0) == 2);     /* admin(1)、dave(5) 被标失效 */
    CHECK(user_find_by_face(1, &chk) == 0);
    CHECK(chk.face_enable == false);
    CHECK(user_find_by_face(5, &chk) == 0);
    CHECK(chk.face_enable == false);

    /* ---- 反向孤儿（模组有、本地无认领，2026-09-20 补）：只识别 + 记审计，
     *      **不自动删** —— 「模组上有脸、本地暂未绑定」可能是用户的临时状态。 ---- */
    {
        log_entry_t * lg = NULL;
        int lc = 0;
        CHECK(log_query("ALARM", 0, &lg, &lc) == 0);
        const int before = lc;
        if (lg) free(lg);

        /* 模组里放 7、8 两个模板：本地无人认领（admin/dave 绑的是 1/5，且均已失效）
         * → 没有正孤儿可标（返回 0），但应识别出 2 个反向孤儿并记一条审计。 */
        const int32_t mod2[2] = { 7, 8 };
        CHECK(cred_reconcile_apply(mod2, 2) == 0);

        CHECK(log_query("ALARM", 0, &lg, &lc) == 0);
        CHECK(lc > before);                        /* 新增了审计 */
        bool found = false;
        for (int i = 0; i < lc; i++) {
            if (strstr(lg[i].detail, "未被任何本地用户认领") != NULL) {
                found = true;
                break;
            }
        }
        if (lg) free(lg);
        CHECK(found);                              /* 反向孤儿确实被记录 */

        /* 且不得因识别反向孤儿而改动任何本地用户（只读语义） */
        CHECK(user_find_by_name("dave", &chk) == 0);
        CHECK(chk.enabled == true);
    }

    TEST_RESULT();
}
