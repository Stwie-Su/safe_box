/**
 * @file cred_reconcile.c
 * FR-21 防线 3「启动对账」实现：孤儿凭据标失效（需求 §5）。
 *
 * 规格：系统启动或模组就绪后，核对模组用户清单与本地凭据。
 *   · **正孤儿**（本地有、模组无）→ 标记失效且不用于放行（FR-21 防线 3）；
 *   · **反向孤儿**（模组有、本地无认领）→ 2026-09-20 补：只**识别 + 记审计**，
     * 不自动删（「模组上有脸、本地暂未绑定」可能是用户的临时状态）。
 *
 * 落点说明（为什么放在 core/auth，触发点放在 app.c）：
 *   - 对账的输入（模组清单）来自 hal/hal_face.h 的 face_service_module_users()，
 *     输出（本地 users.json 的 face_enable）属凭据域，与 auth_fsm 的「凭据失效」判定
 *     （cred_name_ok 也把 face_enable 置 false）同属一处职责；core→hal 是架构允许方向。
 *   - 周期性触发点放在编排层 app.c 的 app_tick_fast（主线程 20ms 泵）：那里既是
 *     face_service_module_users() 的合法调用点，也是 worker_post() 的合法调用点，
 *     且不引入新线程（全机常驻线程 <= 4 硬约束）。
 *
 * 纪律：
 *   - 文件 IO（读 users.json + 写回）一律经 worker_post 下沉后台线程，主线程不做同步落盘；
 *   - 一次性（幂等）：模组清单首次取得后只跑一次，置内部标志防重复；
 *   - -1（无此概念 / fake / none）与 -2（尚未取得）不执行对账——否则会把全部凭据一夜判死。
 */
#include "core/event_bus.h"   /* 对账完成后广播 EV_USER_CHANGED */
#include "core/auth/cred_reconcile.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/store/store.h"
#include "core/support/worker.h"
#include "hal/hal_face.h"

/* 模组清单本地缓存容量（与 hal/face/backend_fm225.c 的 FM225_RECON_MAX_IDS 对齐；
 * 实测模组容量 100，留余量）。 */
#define CRED_RECON_MAX_IDS 128

/* 审计事件名：与 FR-21 防线 1（auth_fsm.c 的 cred_name_ok）保持同一事件名 "ALARM"，
 * 使日志页 / 监控页（page_logs.c / page_monitor.c 按精确 strcmp 过滤）能识别。
 * ⚠️ 大小写必须与写入侧完全一致（本项目踩过 "UNLOCK" vs "unlock" 的坑）。 */
#define CRED_RECON_EVT "ALARM"

/* 一次性对账标志：置位后 cred_reconcile_tick() 不再触发（幂等）。 */
static bool s_done;

/* 后台对账作业载荷（堆分配，done 回调释放）。 */
typedef struct {
    int32_t count;                     /* 模组清单数量（-1/-2 已在上层过滤，这里 >= 0） */
    int32_t ids[CRED_RECON_MAX_IDS];   /* 模组侧 ID 快照 */
    int     changed;                   /* worker 回填：被标失效的用户数 */
} cred_recon_job_t;

static bool id_in_list(const int32_t * ids, int32_t n, int32_t id)
{
    for(int32_t i = 0; i < n; i++) {
        if(ids[i] == id) return true;
    }
    return false;
}

int cred_reconcile_apply(const int32_t * mod_ids, int32_t mod_count)
{
    /* -1（无此概念）/ -2（尚未取得）：不执行对账，避免把全部凭据判死。 */
    if(mod_count < 0) return 0;
    if(mod_count > 0 && mod_ids == NULL) return 0;

    safe_user_t * users = NULL;
    int n = 0;
    if(user_load_all(&users, &n) != 0) return 0;   /* 读失败：本次跳过，不误伤 */

    int changed = 0;
    for(int i = 0; i < n; i++) {
        safe_user_t * u = &users[i];
        if(u->face_id < 0)  continue;   /* 未绑定人脸：不是孤儿，不许动 */
        if(!u->face_enable) continue;   /* 已失效：无重复动作（幂等） */
        if(id_in_list(mod_ids, mod_count, u->face_id)) continue;   /* 模组有该模板：非孤儿 */

        /* 孤儿凭据：只标失效、不删用户；PIN / 动态码通道不受影响。 */
        u->face_enable = false;
        if(user_update(u) == 0) {
            changed++;
            char detail[128];
            snprintf(detail, sizeof(detail),
                     "孤儿凭据：face_id=%d 不在模组清单，已标失效", u->face_id);
            log_append(CRED_RECON_EVT, u->name, 0, detail);
            printf("[recon] 孤儿凭据：user=%s face_id=%d -> face_enable=false\n",
                   u->name, u->face_id);
        }
    }
    /* ★ 反向孤儿（模组有、本地无）识别 —— 2026-09-20 补上的能力缺口。
     *
     * 含义：模组侧存在某个模板，但**本地没有任何用户认领它**。
     * 危害：它占着模组的用户名额，而且在界面上**完全不可见** ——
     *       累积多了就撞容量上限（表现就是「模组容量计数异常（删除被拒）」）。
     *       正孤儿会被标失效（可见），反向孤儿原本是隐形的，用户永远不知道
     *       模组为什么会满。
     *
     * ★ 为什么**只识别、不自动删**：「模组上有张脸、本地暂时没绑」可能是用户的
     *   临时状态（比如刚在模组上单独录了一张、还没在 app 里绑人）。自动删会毁掉它，
     *   而这类破坏不可逆。是否清理交给用户 —— SYSTEM 页「清空模组人脸」，
     *   或回用户页重新录入以认领它。
     *
     * ★ 判据用「face_id 是否匹配」，不看 face_enable：某人脸通道被标失效
     *   （face_enable=false）只代表本地暂时不用它，模板仍在模组里、仍被本地认领，
     *   不该误报成无人认领。 */
    int     orphan_in_mod = 0;
    int32_t orphan_ids[CRED_RECON_MAX_IDS];
    int     orphan_n = 0;
    for(int32_t i = 0; i < mod_count; i++) {
        bool claimed = false;
        for(int j = 0; j < n; j++) {
            if(users[j].face_id == mod_ids[i]) { claimed = true; break; }
        }
        if(claimed) continue;
        orphan_in_mod++;
        if(orphan_n < CRED_RECON_MAX_IDS) orphan_ids[orphan_n++] = mod_ids[i];
    }
    if(orphan_in_mod > 0) {
        char detail[192];
        int off = snprintf(detail, sizeof(detail),
                           "模组侧有 %d 个模板未被任何本地用户认领（占用名额）：",
                           orphan_in_mod);
        for(int k = 0; k < orphan_n && off < (int)sizeof(detail) - 8; k++) {
            off += snprintf(detail + off, sizeof(detail) - (size_t)off, "%s%d",
                            k ? "," : "", (int)orphan_ids[k]);
        }
        log_append(CRED_RECON_EVT, "-", 0, detail);
        printf("[recon] 反向孤儿：%s\n", detail);
        printf("[recon]   → 可在「系统」页用「清空模组人脸」清理，或重新录入以认领\n");
    }

    user_list_free(users);
    return changed;
}

/* 后台作业：在 worker 线程执行（文件 IO，不阻塞主线程）。 */
static void cred_reconcile_worker(void * p)
{
    cred_recon_job_t * job = (cred_recon_job_t *)p;
    job->changed = cred_reconcile_apply(job->ids, job->count);
}

/* 完成回调：在主线程 worker_poll() 执行（可安全广播事件 / 操作 UI）。 */
static void cred_reconcile_done(void * p)
{
    cred_recon_job_t * job = (cred_recon_job_t *)p;
    if(job->changed > 0) {
        printf("[recon] 启动对账完成：%d 条孤儿凭据已标记失效\n", job->changed);
        /* ★ 2026-09-20 起**恢复广播**：EV_USER_CHANGED 现在有订阅者了
         * （page_users 订阅它重新拉列表）—— 原注释所说的「全工程无订阅者、
         * 属死代码」已不成立。对账会把孤儿凭据的 face_enable 标为 false，
         * 用户页上「删人脸 / 录人脸」按钮状态必须跟着变，否则界面与实际不符。 */
        event_bus_publish(EV_USER_CHANGED, NULL);
    }
    free(job);
}

void cred_reconcile_tick(void)
{
    if(s_done) return;                                   /* 只跑一次（幂等） */

    /* 一次性取「数量 + 清单」——**必须单次调用**：此前分两次调用（先以
     * face_service_module_users(NULL,0) 取 count，再取 ids）之间存在竞态
     * （QA 复核 低危#4）——首次取完 count 后，face 线程若收到 NOTE READY 会把
     * s_mod_id_count 复位成 -1，第二次取 ids 即返回 -2 且**不写** job->ids（全 0）；
     * 于是 job->count >= 0 而 ids 全 0，对账把所有已绑定用户误判成孤儿、整片标失效。
     * 合并成一次调用后 count 与 ids 取自同一快照，窗口消失。 */
    cred_recon_job_t * job = (cred_recon_job_t *)calloc(1, sizeof(*job));
    if(job == NULL) return;                              /* 分配失败：下个 tick 再试，不算已跑 */

    int32_t count = face_service_module_users(job->ids, CRED_RECON_MAX_IDS);
    if(count < 0) {                                      /* -1 无此概念 / -2 尚未取得：不触发 */
        free(job);
        return;
    }
    if(count > CRED_RECON_MAX_IDS) count = CRED_RECON_MAX_IDS;   /* 与已写入的 ids 数对齐 */

    job->count = count;
    s_done = true;                                       /* 投递即置位：杜绝重复对账 */
    worker_post(cred_reconcile_worker, job, cred_reconcile_done);
}

void cred_reconcile_reset(void)
{
    s_done = false;
}
