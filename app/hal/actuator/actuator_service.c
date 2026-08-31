/**
 * @file actuator_service.c
 * 执行器门面：NFR-7 的 500ms 上限在这里强制截断，后端不必重复防御。
 *
 * 阶段 3 接 GPIO 时：实现 actuator_gpio.c 并把默认后端换掉，
 * auth_fsm 与 UI 调用的接口不变。
 */

#include "actuator_backend.h"

extern const hal_actuator_backend_t hal_actuator_backend_mock;

static const hal_actuator_backend_t * s_backend = &hal_actuator_backend_mock;
static bool s_high;

safe_err_t hal_actuator_init(void)
{
    s_high = false;
    return s_backend->init();
}

safe_err_t hal_actuator_pulse(uint32_t ms)
{
    if(ms == 0) return SAFE_ERR_PARAM;
    if(ms > HAL_ACTUATOR_PULSE_MAX_MS) ms = HAL_ACTUATOR_PULSE_MAX_MS;   /* NFR-7 */

    s_high = true;
    return s_backend->pulse(ms);
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
