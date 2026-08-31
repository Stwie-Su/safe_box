/**
 * @file fonts_ft.c
 * PC 阶段 FreeType 字体初始化。
 *
 * 仅在 CONFIG_LV_USE_FREETYPE=y 时生效；开发板不启用 FreeType，
 * 本文件编译为空函数，业务层通过 app_font() 宏自动切换到位图字体。
 *
 * 注意：LVGL v9 在 lv_init() 中已自动调用 lv_freetype_init()，
 *       本文件只需创建字体，不可再次初始化。
 */
#include "fonts.h"

#if LV_USE_FREETYPE

#include <stdio.h>

/* Ubuntu 18.04 系统 Noto CJK 字体路径（.ttc 集合，FreeType 支持） */
#define FONT_PATH "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"

lv_font_t * g_app_font_14 = NULL;
lv_font_t * g_app_font_16 = NULL;
lv_font_t * g_app_font_20 = NULL;
lv_font_t * g_app_font_28 = NULL;
lv_font_t * g_app_font_36 = NULL;
lv_font_t * g_app_font_44 = NULL;

void app_fonts_init(void)
{
    static bool inited = false;
    if (inited) return;
    inited = true;

    fprintf(stderr, "[fonts_ft] creating fonts from %s\n", FONT_PATH);

    /*
     * 渲染模式用 BITMAP（SW 光栅化）而非 OUTLINE：
     *  - OUTLINE 是给向量渲染器（ThorVG/矢量绘制）用的，本工程纯 SW 渲染，
     *    outline 因 event_cb 未注册无法出图（lv_freetype_outline.c:229 报错）；
     *  - BITMAP 模式 FT_Load_Glyph+FT_Render_Glyph 在 FT 2.8.1 上已验证正常。
     *  - 注意：绘制线程栈需 ≥256KB（CONFIG_LV_DRAW_THREAD_STACK_SIZE），
     *    否则 FT_Load_Glyph 会栈溢出崩溃。
     */
    g_app_font_14 = lv_freetype_font_create(FONT_PATH,
                                            LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 14,
                                            LV_FREETYPE_FONT_STYLE_NORMAL);
    g_app_font_16 = lv_freetype_font_create(FONT_PATH,
                                            LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 16,
                                            LV_FREETYPE_FONT_STYLE_NORMAL);
    g_app_font_20 = lv_freetype_font_create(FONT_PATH,
                                            LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 20,
                                            LV_FREETYPE_FONT_STYLE_NORMAL);
    g_app_font_28 = lv_freetype_font_create(FONT_PATH,
                                            LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 28,
                                            LV_FREETYPE_FONT_STYLE_NORMAL);

    g_app_font_36 = lv_freetype_font_create(FONT_PATH,
                                            LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 36,
                                            LV_FREETYPE_FONT_STYLE_NORMAL);

    g_app_font_44 = lv_freetype_font_create(FONT_PATH,
                                            LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 44,
                                            LV_FREETYPE_FONT_STYLE_NORMAL);

    fprintf(stderr, "[fonts_ft] font ptrs: 14=%p 16=%p 20=%p 28=%p\n",
            (void*)g_app_font_14, (void*)g_app_font_16, (void*)g_app_font_20, (void*)g_app_font_28);

    if (!g_app_font_14 || !g_app_font_16 || !g_app_font_20 || !g_app_font_28) {
        fprintf(stderr, "[fonts_ft] some font size failed to load from %s\n", FONT_PATH);
    }
}

#else /* LV_USE_FREETYPE */

void app_fonts_init(void)
{
    /* 开发板：位图字体由 lv_font_cn_*.c 静态提供，无需初始化 */
}

#endif /* LV_USE_FREETYPE */
