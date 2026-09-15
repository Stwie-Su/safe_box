/**
 * @file backend_none.c
 * 空人脸后端：人脸通道整体停用时使用（SAFE_FACE_BACKEND=none）。
 *
 * 所有操作返回 SAFE_ERR_UNSUP，能力位为 0。
 * 业务层在调用前应查 face_service_caps()，发现无 FACE_CAP_DETECT 时把人脸入口置灰。
 */

#include "face_backend.h"

#include <stddef.h>

static safe_err_t none_ok(void)
{
    return SAFE_OK;
}

static safe_err_t none_unsup(void)
{
    return SAFE_ERR_UNSUP;
}

static safe_err_t none_delete(int32_t face_id)
{
    (void)face_id;
    return SAFE_ERR_UNSUP;
}

static safe_err_t none_enroll(const char * user_name)
{
    (void)user_name;
    return SAFE_ERR_UNSUP;
}

static int32_t none_module_users(int32_t * ids, int32_t cap)
{
    (void)ids; (void)cap;
    return -1;                       /* 无模组清单概念 */
}

static safe_err_t none_inject(int32_t face_id, face_reason_t reason)
{
    (void)face_id; (void)reason;
    return SAFE_ERR_UNSUP;
}

static const face_backend_t backend = {
    .name          = "none",
    .caps          = 0,
    .max_templates = 0,
    .init          = none_ok,
    .deinit        = none_ok,
    .start         = none_unsup,
    .stop          = none_ok,
    .tick          = NULL,
    .enroll        = none_enroll,
    .delete_tpl    = none_delete,
    .module_users  = none_module_users,
    .inject        = none_inject,
};

const face_backend_t * face_backend_none(void)
{
    return &backend;
}

