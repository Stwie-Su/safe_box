/**
 * @file actuator_service.c
 * 执行器分发层：NFR-7 的 500ms 上限在这里强制截断，后端不必重复防御。
 *
 * 后端选择（SAFE_ACTUATOR_BACKEND）：
 *   gpio  = 真实继电器（sysfs /sys/class/gpio）
 *   mock  = 只打日志，PC 与未接线阶段用
 *   不指定 = **优先 gpio，不可用则回退 mock**（PC 上 /sys/class/gpio 不存在；
 *            板上引脚若被设备树里其它驱动占用，export 也会失败）。
 * 与相机 / 时间 / 看门狗同构：运行期选择 + 失败降级，业务层一行都不用改。
 */

#include "actuator_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const hal_actuator_backend_t hal_actuator_backend_mock;
extern const hal_actuator_backend_t hal_actuator_backend_gpio;

static const hal_actuator_backend_t * s_backend = &hal_actuator_backend_mock;
static bool s_high;

safe_err_t hal_actuator_init(void)
{
    const char * want = getenv("SAFE_ACTUATOR_BACKEND");

    s_high = false;

    if(want != NULL && strcmp(want, "mock") == 0) {
        s_backend = &hal_actuator_backend_mock;
        return s_backend->init();
    }
    if(want != NULL && strcmp(want, "gpio") == 0) {
        /* 显式指定就认账：失败如实返回，不悄悄换成 mock 让人以为锁能动 */
        s_backend = &hal_actuator_backend_gpio;
        return s_backend->init();
    }

    s_backend = &hal_actuator_backend_gpio;
    if(s_backend->init() == SAFE_OK) return SAFE_OK;

    printf("[ACTUATOR] gpio 不可用，回退 mock（PC 或引脚被占用）\n");
    s_backend = &hal_actuator_backend_mock;
    return s_backend->init();
}

safe_err_t hal_actuator_pulse(uint32_t ms)
{
    if(ms == 0) return SAFE_ERR_PARAM;
    if(ms > HAL_ACTUATOR_PULSE_MAX_MS) ms = HAL_ACTUATOR_PULSE_MAX_MS;   /* NFR-7 */

    /* ★ 先执行、成功才置位。
     * 旧写法是「先 s_high = true 再 pulse」：pulse 返回 SAFE_ERR_BUSY / SAFE_ERR_FAIL
     * 时状态位不回滚，而 hal_actuator_state() 被 ui.c:144 与 page_monitor.c:500
     * 直接当作「是否已开锁」展示 —— 于是一次**失败**的开锁会让界面永久停在
     * 开锁态（真 bug，P0）。失败必须失败得干净：状态位只在后端确实拉高后置位。 */
    safe_err_t e = s_backend->pulse(ms);
    if(e != SAFE_OK) return e;

    s_high = true;
    return SAFE_OK;
}

safe_err_t hal_actuator_release(void)
{
    s_high = false;
    return s_backend->release();
}

bool hal_actuator_state(void)
{
    return s_high;
}

void hal_actuator_notify_low(void)
{
    s_high = false;
}
