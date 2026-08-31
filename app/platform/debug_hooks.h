/**
 * @file debug_hooks.h
 * PC 调试钩子：起始页 / 起始主题 / 自动开弹窗 / 截图，全部由环境变量触发。
 *
 * 这些是开发期的手动验证手段，不适合混进主流程，所以单独成文。
 * 板子构建（非 SDL）下为空实现，环境变了也不会有副作用。
 */
#pragma once

/* 在 UI 与应用初始化完成后调用一次。 */
void debug_hooks_apply(void);
