/**
 * @file debug_hooks.h
 * PC 调试用的小开关：起始页、主题、自动弹窗、截图，全由环境变量触发。
 *
 * 这些只是开发期手动验证的手段，不影响正常流程；板子构建下是空实现，
 * 就算设了环境变量也不会有任何反应。
 */
#pragma once

/* UI 和各模块都初始化完之后调用一次（main.c 里调的）。 */
void debug_hooks_apply(void);
