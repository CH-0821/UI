#ifndef __UI_DIRTY_AREAS_H__
#define __UI_DIRTY_AREAS_H__

#include <stdint.h>
#include <stddef.h>     /* NULL */

/* 本模块的可调参数（MAX_W/MAX_H、行缓冲容量、脏矩形表容量、各种上限…）
 * 统一放在 ui_config.h 里，按模块分类 */
#include "./ui_config/ui_config.h"


/* 刚画满、准备交给 DMA 发出去的一段。
 * 行缓冲和脏条表都由 ui_dirty_areas.c 自己管，外面用不到，外面只认这个"结果" */
typedef struct
{
  ui_color_t *buf;          /* 像素起点；行与行在内存里紧挨着排 */
  uint16_t start_x;         /* 这一段最左边一列在屏幕上的列号 */
  uint16_t start_y;         /* 这一段最上面一行在屏幕上的行号 */
  uint16_t row_count;       /* 这一段实际画了几行（最后一段可能不满） */
  uint16_t pixels_per_row;  /* 每行多少像素，就是这一段要发的横向宽度 */
}ui_row_chunk_t;

/* 接下来要推的一片：行范围 + 横向范围。
 * ui_dirty_take 每次给一片，给到返回 0 就说明这一帧的脏区都推完了。
 * 一片对应某个脏矩形的一次切片（整宽 × 尽量多行，不超行缓冲容量） */
typedef struct
{
  uint16_t start_y;   /* 这一片最上面一行的屏幕行号 */
  uint16_t end_y;     /* 最下面一行（含） */
  uint16_t left_x;    /* 这一片横向要发的范围，左端（含） */
  uint16_t right_x;   /* 右端（含） */
}ui_dirty_span_t;

/* ---- 脏区登记与取片 -------------------------------------------------- */

/* 登记一块脏矩形 [x0,y0]-[x1,y1]：夹回屏幕内，再并进脏矩形表
 * （已被某块包住就丢掉；和某块相交且合并划算就并成一块；否则单独记一块）。
 * 一帧里可以调多次，几处脏区各记各、互不干扰。
 * 调用方尽量只标"真正变了的窄带"，别整块整块标 —— 每帧推送量直接由此决定 */
void ui_dirty_mark(int16_t x0, int16_t y0, int16_t x1, int16_t y1);

/* 开一帧：把"上一帧的脏区"补进本帧的脏矩形表（双缓冲用）。
 * 双缓冲下后台缓冲欠的是"自它上次显示以来"的变化 = 上一帧 ∪ 这一帧 —— 上一帧那半靠这里补。
 * 【每帧开画之前必须无条件调一次】，即使这一帧一个 ui_dirty_mark 都不会有：
 * 动画停住、整帧没标脏时更要补，否则那块后台缓冲会停在两帧前、翻过来闪旧画面。
 * 不调（或源码里 UI_DIRTY_KEEP_PREV=0）就是单缓冲行为，一个像素都不多推 */
void ui_dirty_frame_begin(void);

/* 取下一片要推的：先从脏矩形表里挑最靠上的一块，推不完就按行切片、分几次推完。
 * 没有更多脏区就返回 0。
 * 返回 1 时 span 里就是这一片的行、列范围 */
uint8_t ui_dirty_take(ui_dirty_span_t *span);

/* 这一"视觉帧"的脏区还有没有没推完的：还有脏块没被 ui_dirty_take 取走、或手上这块还没推完，
 * 都返回 1，全推完返回 0。
 * 主循环用它决定"动画能不能往下推进"：把整帧的脏块都按同一个动画时刻画完，跨块的形状才不会被拉斜 */
uint8_t ui_dirty_pending(void);

/* 设置一帧最多攒多少个脏矩形（内部夹到 [8, 64]；不调就用满静态容量）。
 * 调小：每帧的扫描 / 合并更快，但更早触发"并进最划算那块 / 整屏重画"的退化。
 * 初始化阶段调一次即可（LCD 起来之后、开始标脏之前） */
void ui_dirty_set_max(uint8_t max_rects);

/* 设置实际屏幕尺寸(像素)：标脏按它夹边界(不设则用 MAX_W/MAX_H)。
 * 开始标脏之前调一次 —— 真屏比 MAX 小时，不设会把屏外坐标当脏区收进来 */
void ui_dirty_set_screen(int16_t w, int16_t h);

/* ---- 行缓冲推送 ------------------------------------------------------ */

/* 把刚画满的这一片交出去（给 SPI DMA 发），绘制转到另一块上继续画 */
ui_row_chunk_t ui_row_buf_swap(void);

/* 开新的一片：记下这一片的起始屏幕行号 y。
 * 这一片实际画几行由 ui_dirty_take 切好（整宽 × 尽量多行，不超行缓冲容量）。
 * 每片开画前调一次 */
void ui_row_buf_set_y(uint16_t y);

/* 把整段缓冲（这一段的行数 × 这一条的宽度）统统刷成背景色 */
void ui_row_buf_clear(ui_color_t color);

/* ---- 推屏流水线 ------------------------------------------------------
 * 把"取脏条 -> 画一片 -> 交给硬件"这条流水线收在本模块里，主循环每轮只调一次 ui_render_slice()。
 * 两头和硬件 / 场景绑死的部分用钩子接进来：
 *   dev->busy()  : 上一片搬完没有(1=还在搬) —— 流水线靠它做背压，也靠它判断"整帧推完没有"
 *   dev->start() : 把一片交给硬件(比如 DMA2D 搬运)
 *   scene()      : 把【当前这一片】整屏画一遍(自己铺底 + 遍历对象) —— 由调用方提供
 * 两块行缓冲始终是"一块给硬件读、一块给 CPU 写"，"画下一片"正好挤在"搬上一片"期间做。
 * 一帧必须等 ui_render_idle() 返回 1 才翻页，否则会把还在搬的最后一片翻上屏(画面缺一块) */
typedef struct
{
  uint8_t (*busy)(void);                      /* 1 = 上一片还在搬，这一轮先别给 */
  void    (*start)(const ui_row_chunk_t *c);  /* 把这一片交给硬件 */
}ui_flush_dev_t;

typedef void (*ui_scene_draw_fn)(void);

/* 调试浮层：每画完一片场景、交给硬件之前额外调一次。
 * 钩子里直接用行级接口（ui_row_draw_*）往当前行缓冲画东西即可 —— 它天然压在
 * 这一片的所有页面内容之上；因为它不是 UI 对象，命中测试永远碰不到（点击穿透）。
 * 传 NULL = 关掉。库里的默认实现是 ui_module 的调试 HUD（ui_config.h 的 UI_DEBUG_HUD） */
typedef void (*ui_overlay_draw_fn)(void);
void ui_render_set_overlay(ui_overlay_draw_fn fn);

/* 实际屏幕尺寸（ui_dirty_set_screen 设进来的值）：给需要"贴屏幕右上角"之类的库内模块用 */
int16_t ui_dirty_screen_w(void);
int16_t ui_dirty_screen_h(void);

/* 初始化：接上推屏后端和场景绘制。LCD 和对象都就绪之后调一次 */
void ui_render_init(const ui_flush_dev_t *dev, ui_scene_draw_fn scene);

/* 推进流水线一步：要么把手上画好的片发出去，要么取下一片画。不阻塞，主循环每轮调一次 */
void ui_render_slice(void);

/* 这一帧是不是都推完了(脏区取完 + 手上没片 + 硬件也搬完了) */
uint8_t ui_render_idle(void);

/* ---- 供图元绘制使用的行级接口（只给 ui_create.c 的绘制函数用）--------
 * 一次画满当前这一片：先 select 把绘制游标指到某一行、拿到行首地址，
 * 再用 span 往这一行涂色（自动裁到当前脏条范围内）。 */
uint16_t  ui_row_draw_rows(void);             /* 当前这一片共几行（最大 MAX_H=600，>255 也合法） */
int16_t   ui_row_draw_screen_y(uint16_t row); /* 片内第 row 行对应的屏幕行号 */
ui_color_t *ui_row_draw_select(uint16_t row);   /* 游标指到片内第 row 行，返回该行行首地址 */
int16_t   ui_row_draw_left(void);            /* 当前脏条左端（屏幕 x，含） */
int16_t   ui_row_draw_right(void);           /* 当前脏条右端（屏幕 x，含） */
void      ui_row_draw_span(int16_t from_x, int16_t to_x, ui_color_t color); /* 给 [from_x,to_x] 涂色 */

/* 半透明涂色：把 color 按 alpha 和"行缓冲里已经画好的下层像素"逐像素混合。
 *   alpha = 255 等同 ui_row_draw_span（不透明）；alpha = 0 什么都不画；1~254 为混合比例。
 * 裁剪口径和 ui_row_draw_span 完全一样（脏条 ∩ 视口裁剪，纵向也会裁）。
 * 因为混的是行缓冲当前内容，框架"每片先铺背景、再按 z 序画对象"的流程天然成立：
 * 先画下层、后画这个半透明对象，就能透出下面的卡片 / 渐变。
 * 代价是逐像素运算，大面积遮罩比整段填色慢，按需使用 */
void      ui_row_draw_span_blend(int16_t from_x, int16_t to_x, ui_color_t color, uint8_t alpha);

/* 按 alpha 混合 / 线性插值：格式相关的位运算都在实现里，外部只认 ui_color_t */
ui_color_t ui_color_blend(ui_color_t bg, ui_color_t fg, uint8_t alpha);
ui_color_t ui_color_lerp(ui_color_t c0, ui_color_t c1, uint16_t t, uint16_t tmax);

/* ---- 视口裁剪 --------------------------------------------------------
 * 给"滚动列表 / 视口"这类场景用：设一个裁剪矩形后，本片绘制出的像素只会落在矩形内
 * （横向夹到矩形；纵向不在矩形内的屏幕行整行不画）。
 * 用法是"设裁剪 -> 画一批对象 -> 清裁剪"，裁剪是绘制期状态、不进脏区表。
 * 不设置（或 ui_row_clip_clear）就是整屏，行为跟以前完全一样 */
void ui_row_clip_set(int16_t x0, int16_t y0, int16_t x1, int16_t y1);
void ui_row_clip_clear(void);

/* 圆角裁剪：四角同一半径（0 = 矩形），每行按圆角内缩后再当矩形裁剪用。
 * 半径内部夹到 min(w,h)/2；内缩公式和 ui_tool_round_rect_row_ex 完全一致，
 * 所以拿它裁"按同一半径画的圆角窗口"时，内容不会从四个圆角缺口里漏出来。
 * 半径上限 255（受 uint8_t 限制），常规卡片 8~24 足够 */
void ui_row_clip_set_round(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t radius);

/* 可嵌套的裁剪：入栈当前裁剪并压上新的一个（和上一层求交），出栈时还原。
 * 容器对象（视窗）绘制子对象时用它 —— 多个容器嵌套时，内层裁剪自动夹在外层里面；
 * 求交是【逐行】做的，所以外层是圆角、内层是矩形也能夹对。
 * 栈深上限 UI_ROW_CLIP_MAX，超出后退化成"矩形求交、不保存"（正常嵌套一两层用不到） */
void ui_row_clip_push(int16_t x0, int16_t y0, int16_t x1, int16_t y1);

/* 带圆角的可嵌套裁剪（参数含义同 ui_row_clip_set_round） */
void ui_row_clip_push_round(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t radius);
void ui_row_clip_pop(void);

/* ---- 标脏裁剪 --------------------------------------------------------
 * 和上面"绘制裁剪"分属两条路：这条作用在 ui_dirty_mark 上 —— 进来的脏矩形先夹到
 * 当前标脏裁剪矩形内，夹空了直接丢掉，于是窗口外的区域一个像素都不推。
 * 给"容器（视窗）里的子对象做动画"用：子对象本来就只画在窗口内，窗口外没有变化，
 * 也就不该为它标脏。用法是 push -> 驱动这一段动画 -> pop，可嵌套。
 * 【注意】push 之后，这一段里所有标脏都会被夹掉窗口外的部分 ——
 * 只对"确实只影响这个窗口"的对象用；窗口外还有对象在动就别夹，否则会漏脏（画面残留） */
void ui_dirty_clip_push(int16_t x0, int16_t y0, int16_t x1, int16_t y1);
void ui_dirty_clip_pop(void);

/* 当前正在画的这一行（片内游标）的屏幕 y 是否落在裁剪矩形内。
 * 不走 ui_row_draw_span 的图元（比如圆环直写缓冲）自己调它做纵向裁剪；没设裁剪恒为 1 */
uint8_t ui_row_draw_row_visible(void);

/* 裁剪后本行的有效横向范围（= 脏条 ∩ 裁剪矩形）。同样给直写缓冲的图元用；
 * 没设裁剪时等于 ui_row_draw_left / ui_row_draw_right */
int16_t ui_row_clip_left(void);
int16_t ui_row_clip_right(void);

#endif
