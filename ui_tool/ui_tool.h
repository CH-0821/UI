#ifndef __UI_TOOL_H__
#define __UI_TOOL_H__

#include <stdint.h>
#include "./ui_create/ui_create.h"   /* ui_rect_para_t / ui_row_draw_* / UI_TYPE_RECT */

/* ==================================================================
 * 圆角矩形工具
 *   这套框架是"按行往行缓冲里画"的：一行一行算、每行涂一段。
 *   圆角的关键就是——求出"这一行该涂哪一段"[x0,x1]，再照常 ui_row_draw_span。
 *   下面的 ui_tool_round_rect_row 就是干这个的，实心 / 描边 / 渐变都拿它取范围。
 * ================================================================== */

/* 圆角掩码：逐个角可组合，指定要圆哪些角 —— 可以四角全圆、只圆上下两角，
 * 也可以只圆某一个角（比如只圆左上角就传 UI_ROUND_TOP_LEFT）。
 * 每个角一个 bit，TOP / BOTTOM / ALL 是把对应位拼起来的分组别名。
 * 柱底压在基线上的柱子只圆上两角（下边留直角），卡片 / 色块一般全圆 */
#define UI_ROUND_TOP_LEFT      0x1u
#define UI_ROUND_TOP_RIGHT     0x2u
#define UI_ROUND_BOTTOM_LEFT   0x4u
#define UI_ROUND_BOTTOM_RIGHT  0x8u
#define UI_ROUND_TOP     (UI_ROUND_TOP_LEFT | UI_ROUND_TOP_RIGHT)
#define UI_ROUND_BOTTOM  (UI_ROUND_BOTTOM_LEFT | UI_ROUND_BOTTOM_RIGHT)
#define UI_ROUND_ALL     (UI_ROUND_TOP | UI_ROUND_BOTTOM)

/* 圆角矩形：算某一屏幕行 y 的横向填充范围 [x0, x1]（含端点）。
 *   半径夹到 min(w,h)/2，四角是四分之一圆（角点用整数开方求内缩量，不引浮点）。
 *   corners 用 UI_ROUND_* 指定要圆哪些角（只圆上角就传 UI_ROUND_TOP）。
 *   返回 1 = 该行落在矩形内、x0/x1 有效；0 = 该行在矩形外（不用画）。
 *   纯几何、不带颜色 —— 想上什么色由调用方决定 */
uint8_t ui_tool_round_rect_row_ex(int16_t cx, int16_t cy, int16_t w, int16_t h,
                                  int16_t radius, int16_t y, uint8_t corners,
                                  int16_t *x0, int16_t *x1);

/* 全圆角版（corners = UI_ROUND_ALL）的便捷封装 */
uint8_t ui_tool_round_rect_row(int16_t cx, int16_t cy, int16_t w, int16_t h,
                               int16_t radius, int16_t y,
                               int16_t *x0, int16_t *x1);

/* 圆角矩形绘制：读 para 的 中心/宽高/线宽/颜色/fill
 *   fill != 0 -> 实心圆角矩形（颜色 = para->color）
 *   fill == 0 -> 只描边，border_width 宽的圆角外框
 * radius 由外部给（para 里没有这个字段）—— 想让某对象圆角，就在它的绘制回调里包一层：
 *     static void my_draw(void *p) { ui_tool_draw_round_rect(p, 16); }
 *   再把 obj->para->head.draw 指到 my_draw
 * corners 用 UI_ROUND_* 指定圆哪些角（柱身只圆上角就传 UI_ROUND_TOP） */
void ui_tool_draw_round_rect_ex(void *para, int16_t radius, uint8_t corners);

/* 全圆角版（corners = UI_ROUND_ALL）的便捷封装 */
void ui_tool_draw_round_rect(void *para, int16_t radius);

/* 圆角矩形 + 竖直渐变填充：底行取 c_bottom、顶行取 c_top，中间按行过渡。
 * 背景 / 卡片那种"上浅下深"的色块用它上色就带圆角了 */
void ui_tool_draw_round_grad(void *para, int16_t radius, ui_color_t c_bottom, ui_color_t c_top);

/* ==================================================================
 * 图片 / 图标按行搬运（裁剪在这里做）
 *   图片类图元不逐像素判几何，要做的只有"把点阵的对应行搬进行缓冲"，
 *   全部裁剪（出脏条 / 出视窗）都在求交这一步完成。
 * ================================================================== */

/* 把点阵的一行铺进当前脏条：这一行在屏幕上占 [img_x_min, img_x_max]（含端点，
 * 已含平移），row_px 指向该行第 0 列的像素（RGB565）。
 * 先和"当前脏条 ∩ 视窗裁剪"求交（含视窗纵向裁剪），交上的段直接搬进行缓冲。
 * key / key_on = 透明键色：key_on != 0 时，值等于 key 的像素【跳过不写】——
 * 行缓冲里原来那块（下层已画好的卡片底）就露出来了。片外图标数据是 RGB565、
 * 没有 alpha 通道，透明就是这么约定的（转 bin 时把透明像素写成这个键色）。
 * alpha：0(未设) / 255 = 不透明直写；1~254 = 再和下层像素半透明混合（键色像素仍跳过）。
 * 返回 1 = 画了；0 = 整段被裁掉 */
uint8_t ui_tool_image_row(uint16_t strip_row, const ui_color_t *row_px,
                          int16_t img_x_min, int16_t img_x_max,
                          ui_color_t key, uint8_t key_on, uint8_t alpha);

/* 片外图标版：像素先经 row_fn 按段读进 line_buf，再走和上面同一套裁剪搬运。
 * img_y 是图片内行号（透传给回调）；line_buf / line_max 是调用方给的缓冲及容量
 * （像素数），可见段比缓冲宽时自动分段读，不截断。
 * key / key_on / alpha 同 ui_tool_image_row。
 * 返回 1 = 画了；0 = 整段被裁掉或回调取不到（取到一半失败时本行可能画了一部分） */
uint8_t ui_tool_image_row_read(uint16_t strip_row,
                               ui_img_row_fn_t row_fn, uint8_t img_id, int16_t img_y,
                               int16_t img_x_min, int16_t img_x_max,
                               ui_color_t *line_buf, uint16_t line_max,
                               ui_color_t key, uint8_t key_on, uint8_t alpha);

#endif
