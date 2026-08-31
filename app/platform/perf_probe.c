#include "platform/perf_probe.h"

#include <stdlib.h>
#include <string.h>

#include "lvgl.h"

static bool     s_on      = false;
static uint32_t s_loops   = 0;
static uint32_t s_sum_ms  = 0;
static uint32_t s_max_ms  = 0;

static void perf_log_cb(lv_timer_t * t)
{
    (void)t;
    if(s_loops == 0) return;
    LV_LOG_USER("perf: cycles=%u  handler avg=%u ms  max=%u ms",
                (unsigned)s_loops,
                (unsigned)(s_sum_ms / s_loops),
                (unsigned)s_max_ms);
    s_loops  = 0;
    s_sum_ms = 0;
    s_max_ms = 0;
}

void perf_probe_init_from_env(void)
{
    const char * e = getenv("SAFE_PERF_LOG");
    if(e == NULL || *e == '\0' || strcmp(e, "0") == 0) return;

    s_on = true;
    lv_timer_create(perf_log_cb, 2000, NULL);
}

bool perf_probe_enabled(void)
{
    return s_on;
}

void perf_probe_record(uint32_t cost_ms)
{
    if(!s_on) return;
    s_loops++;
    s_sum_ms += cost_ms;
    if(cost_ms > s_max_ms) s_max_ms = cost_ms;
}
