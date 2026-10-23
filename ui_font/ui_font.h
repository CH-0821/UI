#ifndef __UI_FONT_H__
#define __UI_FONT_H__

/* ==================================================================
 *  字库库：一个"读点阵"函数指针 + 一层字模缓存
 *
 *  库不关心字库是什么、放在哪、有几种——它只知道"有一个函数能把 code 这个字
 *  的点阵读进 buf"。那个函数由外部实现，用 ui_font_set_reader() 挂上来。
 *
 *  于是两件事都退化成"换个东西指一下"：
 *    · 换字库来源(NAND / 数组 / SPI Flash) = 换一个读函数；
 *    · 选择字体(宋体 / 黑体 …)             = 换 font_id 这个关键码，
 *                                             由读函数自己去解释。
 *  库这一侧一行都不用改。
 *
 *  读函数签名直接复用 ui_create.h 的 ui_hz_glyph_fn_t，不另立类型：
 *    · 把 code 这个字的点阵写进 buf(行优先、字节内低位在左，和内部 Flash
 *      那几套 ascii_* / tfont* 一致)；
 *    · 填好 glyph 的 advance_x / bytes_per_row / height；
 *    · glyph->bitmap 指到 buf；取不到就返回 0。
 *
 *  用法(两步)：
 *    1. 开机时把读函数挂上去(必须抢在首帧整屏绘制【之前】——
 *       首帧铺完就不算脏区了，那之后才就绪的字库没人回头补画)：
 *         ui_font_set_reader(my_read);
 *    2. 建文字对象时：
 *         p->hz_font_id  = ...;             // 关键码，读函数自己解释
 *         p->hz_glyph_fn = ui_font_glyph;   // 带缓存的取模回调
 *         p->font_size   = ...;
 *
 *  本目录四个文件：
 *    ui_font.h / ui_font.c        —— 这个：库的接口和实现，对外就这两个
 *    ui_font_data.h / ui_font_data.c —— 内置点阵数据，库内部的东西
 *                                        (ui_create.c 没挂回调时拿它兜底)
 * ================================================================== */

#include "./ui_create/ui_create.h"   /* ui_glyph_t / ui_hz_glyph_fn_t / UI_GLYPH_MAX_BYTES */

/**
 * @brief       挂上/更换读点阵函数
 * @param       fn : 外部实现的读函数(签名同 ui_hz_glyph_fn_t)；
 *                   传 NULL = 卸载，之后取模一律返回 0
 * @note        换来源就得重挂一次 —— 顺手把字模缓存清空，
 *              免得旧来源的点阵被当成新来源的用
 */
void ui_font_set_reader(ui_hz_glyph_fn_t fn);

/**
 * @brief       取模回调：签名与 ui_hz_glyph_fn_t 一致，直接赋给 hz_glyph_fn
 * @param       font_id   : 原样透传给读函数(ASCII 不看它)
 * @param       code      : ASCII 1 字节 / GBK 2 字节(高字节在前)
 * @param       font_size : 字号，原样透传给读函数(它该不该认由读函数决定)
 * @param       buf       : 字模落地缓冲，容量至少 UI_GLYPH_MAX_BYTES
 * @param       glyph     : 输出字形；取到时 bitmap 指向 buf
 * @retval      1=取到; 0=没取到(没挂读函数 / 读函数说没有)
 * @note        在外部那个读函数外面套了一层逐字缓存：文字是反复重画的，
 *              稳态下重复的字不再去读介质
 */
uint8_t ui_font_glyph(uint8_t font_id, const uint8_t *code,
                      uint16_t font_size, uint8_t *buf,
                      ui_glyph_t *glyph);

#endif
