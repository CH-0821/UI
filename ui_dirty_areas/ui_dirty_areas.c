#include "./ui_dirty_areas/ui_dirty_areas.h"

/* 本模块的可调参数（行缓冲容量、脏矩形表容量、各种上限与阈值）
 * 都在 ui_config.h 里，按模块分类放着 */


/* 一个待推的脏矩形（屏幕坐标、含端点）。
 * 关键点：**宽的可以很宽、窄的可以很高** —— 光标那种"3px 宽、64 行高"的竖边，
 * 就是一个 3×64 的小矩形，一次 Address_Set + 一次 DMA 就整条推完，
 * 不会再像以前那样被 5 行一条的条带切成 13 段、花 4ms 才写完（那正是台阶的来源） */
typedef struct
{
  int16_t x0;
  int16_t y0;
  int16_t x1;
  int16_t y1;
} ui_dirty_rect_t;

/* 绘制裁剪：矩形 + 统一圆角半径（0 = 直角）。
 * 圆角裁剪本质是"逐行区间"：每行按四角内缩后得到 [x0+d, x1-d]，
 * 所以多层裁剪只要逐行求交就是精确的（见 clip_row_range） */
typedef struct
{
  uint8_t on;
  int16_t x0;
  int16_t y0;
  int16_t x1;
  int16_t y1;
  uint8_t radius;
}ui_row_clip_t;

/* ---- 本模块的全部状态 --------------------------------------------------
 * 脏矩形表 + 乒乓行缓冲 + 当前推送进度，就这一份、只在本文件可见 */
typedef struct
{
  /* 脏矩形表：一帧里各个对象各自 ui_dirty_mark，能划算合并的就并成一块（见 dirty_merge_worth）。
   * 帧末 ui_dirty_take 一块一块取出来推，推完就没了 */
  ui_dirty_rect_t rects[UI_DIRTY_MAX_CAP];
  uint8_t         count;      /* 表里有几块 */
  uint8_t         max_rects;  /* 一帧最多攒几块：ui_dirty_set_max 设定，初值 = CAP */

  uint8_t         has_cur;    /* 1 = 有一块正在推、还没推完（要按行切片分几次推） */
  ui_dirty_rect_t cur;        /* 正在推的那一块 */
  int16_t         cur_y;      /* 这一块下一个要推的起始行 */
  uint16_t        chunk_rows; /* 当前这一片切了几行 */

  /* 乒乓行缓冲：两块轮流用，一块正在画、另一块同时交给 DMA 发上一片，画和发就能重叠。
   * 每片存"当前这块的横向范围 × 这一次切出来的行数"，行与行紧挨着排，一片就是连续内存 */
  ui_color_t buf[2][BUF_PIXELS];

  uint8_t  buf_sel;     /* 两块里正在画的是哪一块（0 或 1） */
  uint16_t draw_y0;     /* 当前这一片的起始屏幕行号 */
  uint16_t draw_rows;   /* 当前这一片实际有几行（可能 >255，最多 MAX_H=600） */
  ui_color_t *draw_row;   /* 片内正在画的那一行，在块里的起始地址 */
  int16_t  draw_screen_y; /* 游标那一行对应的屏幕行号（ui_row_draw_select 里更新） */

  /* 视口裁剪（屏幕坐标、含端点 + 统一圆角）：on = 0 时整屏，行为跟以前一样。
   * 栈里存的是外层的裁剪，逐行和它求交（嵌套视窗 / 圆角滑窗都靠这个） */
  ui_row_clip_t clip;

  /* 标脏裁剪矩形（屏幕坐标、含端点）：dclip_on = 1 时 ui_dirty_mark 先夹到它里面。
   * 和上面的绘制裁剪是两条独立的路：这条只管"标多少脏"，不管"画什么" */
  uint8_t  dclip_on;
  int16_t  dclip_x0;
  int16_t  dclip_y0;
  int16_t  dclip_x1;
  int16_t  dclip_y1;

  /* 实际屏幕尺寸(像素)：标脏夹边界用这个，不能用 MAX_W/MAX_H ——
   * 那两个是"驱动能带的最大屏"(1024×600)，真屏可能比它小(比如 800×480)。
   * 用 max 去夹，就会把屏外的坐标当合法脏区收进来，最后 DMA 越界写坏画面。
   * 由 ui_dirty_set_screen 设定；没设之前退化成 MAX_W/MAX_H(老行为) */
  int16_t  screen_w;
  int16_t  screen_h;
}ui_state_t;

/* 模块内唯一的一份状态。容量默认用满静态表（ui_dirty_set_max 可以改小） */
static ui_state_t ui_state = { .max_rects = (uint8_t)UI_DIRTY_MAX_CAP,
                               .screen_w  = (int16_t)MAX_W,
                               .screen_h  = (int16_t)MAX_H };

/* 当前正在推的那一块脏矩形的横向范围：绘制和裁剪都按它来 */
static int16_t cur_left(void)
{
  return ui_state.cur.x0;
}

static int16_t cur_right(void)
{
  return ui_state.cur.x1;
}

/* ---- 绘制裁剪：当前裁剪 + 外层裁剪栈 -------------------------------
 * 栈里存的是 push 之前的那一层；求有效范围时把栈里所有"开着的"和当前层逐行求交，
 * 于是视窗套视窗、圆角窗口里再套视窗都是精确的（矩形是圆角半径为 0 的特例） */
static ui_row_clip_t ui_clip_stack[UI_ROW_CLIP_MAX];
static uint8_t       ui_clip_depth = 0;

/* 逐位开方（和 ui_tool 的 round_isqrt 同一套算法）：
 * 低层模块不反向依赖 ui_tool，这里留一份小的 */
static uint32_t clip_isqrt(uint32_t v)
{
  uint32_t r = 0u;
  uint32_t b = 1u << 30;

  while (b > v) { b >>= 2; }
  while (b != 0u)
  {
    if (v >= (r + b)) { v -= (r + b); r = (r >> 1) + b; }
    else              { r >>= 1; }
    b >>= 2;
  }
  return r;
}

/* 圆角裁剪在屏幕行 y 上的左右内缩量（四角同一半径）；中间行 / 直角返回 0。
 * 公式和 ui_tool_round_rect_row_ex 完全一致，裁剪边界和画出来的圆角逐像素对齐 */
static int16_t clip_row_inset(const ui_row_clip_t *c, int16_t y)
{
  int16_t r  = (int16_t)c->radius;
  int16_t dy;

  if (r <= 0)
  {
    return 0;
  }

  if (y < (int16_t)(c->y0 + r))                 /* 上端圆角那几行 */
  {
    dy = (int16_t)(c->y0 + r - y);
  }
  else if (y > (int16_t)(c->y1 - r))            /* 下端圆角那几行 */
  {
    dy = (int16_t)(y - (c->y1 - r));
  }
  else
  {
    return 0;                                   /* 中间整宽 */
  }

  return (int16_t)(r - (int16_t)clip_isqrt((uint32_t)((int32_t)r * r - (int32_t)dy * dy)));
}

/* 把一层裁剪并进 [*l, *r]；返回 0 = 这一行被它整行挡掉 */
static uint8_t clip_apply(const ui_row_clip_t *c, int16_t y, int16_t *l, int16_t *r)
{
  int16_t d;

  if (c->on == 0u)
  {
    return 1u;
  }
  if (y < c->y0 || y > c->y1)
  {
    return 0u;
  }

  d = clip_row_inset(c, y);
  if ((int16_t)(c->x0 + d) > *l) { *l = (int16_t)(c->x0 + d); }
  if ((int16_t)(c->x1 - d) < *r) { *r = (int16_t)(c->x1 - d); }
  return 1u;
}

/* 屏幕行 y 的有效横向范围 = 当前脏条 ∩ 所有在用的裁剪层。
 * 返回 0 = 这一行完全不可见（纵向被挡 / 交集为空）；1 = *l / *r 有效 */
static uint8_t clip_row_range(int16_t y, int16_t *l, int16_t *r)
{
  int16_t cl = cur_left();
  int16_t cr = cur_right();
  uint8_t i;

  for (i = 0u; i < ui_clip_depth; i++)
  {
    if (clip_apply(&ui_clip_stack[i], y, &cl, &cr) == 0u)
    {
      return 0u;
    }
  }
  if (clip_apply(&ui_state.clip, y, &cl, &cr) == 0u)
  {
    return 0u;
  }
  if (cl > cr)
  {
    return 0u;
  }

  *l = cl;
  *r = cr;
  return 1u;
}

/* 这一片要发的宽度，也就是当前这块脏矩形有多宽。
 * 块内行与行就按这个宽度挨着排，一片才是连续内存 */
static uint16_t cur_width(void)
{
  return (uint16_t)(cur_right() - cur_left() + 1);
}

/* 当前 buf_sel 指向的那块缓冲的首地址。
 * 画的时候（clear / buf_row）它就是正在画的那块；
 * swap 在翻转 buf_sel 之前调它，拿到的则是刚画完、准备发出去的那块 */
static ui_color_t *buf_cur(void)
{
  return ui_state.buf[ui_state.buf_sel];
}

/* 片内第 row 行在块里的首地址 */
static ui_color_t *buf_row(uint16_t row)
{
  return buf_cur() + (uint32_t)row * cur_width();
}

/* 切到另一块：把刚画满的这一段交出去（给 DMA 发），绘制转到另一块上继续画 */
ui_row_chunk_t ui_row_buf_swap(void)
{
  ui_row_chunk_t chunk;

  chunk.buf            = buf_cur();
  chunk.start_x        = (uint16_t)cur_left();
  chunk.start_y        = ui_state.draw_y0;
  chunk.row_count      = ui_state.draw_rows;
  chunk.pixels_per_row = cur_width();

  ui_state.buf_sel ^= 1;
  ui_state.draw_rows = 0;
  return chunk;
}

/* 开新的一片：记下这一片的起始屏幕行号 y。
 * 这一片实际画几行由 ui_dirty_take 切好存在 chunk_rows 里（整宽 × 尽量多行，不超缓冲容量） */
void ui_row_buf_set_y(uint16_t y)
{
  ui_state.draw_y0   = y;
  ui_state.draw_rows = ui_state.chunk_rows;
}

/* 把 [*from_x, *to_x] 裁到"所有裁剪层 ∩ 当前脏条"以内（含端点）。
 * 圆角裁剪按当前行先内缩再求交，见 clip_row_range。
 * 返回 1 = 还有要画的部分；0 = 整段被裁掉（纵向不在裁剪区里也算） */
static uint8_t span_clip(int16_t *from_x, int16_t *to_x)
{
  int16_t left, right;

  if (clip_row_range(ui_state.draw_screen_y, &left, &right) == 0u)
  {
    return 0u;
  }

  if (*from_x < left)
  {
    *from_x = left;
  }
  if (*to_x > right)
  {
    *to_x = right;
  }

  return (uint8_t)(*from_x <= *to_x);
}

/* 颜色混合：a = 255 得到前景色，a = 0 保持背景。按 UI_COLOR_FMT 选实现 */
ui_color_t ui_color_blend(ui_color_t bg, ui_color_t fg, uint8_t a)
{
#if (UI_COLOR_FMT == UI_COLOR_FMT_RGB565)
  uint16_t ia = (uint16_t)(256u - a);
  uint16_t r  = (uint16_t)((((fg >> 11) & 0x1Fu) * a + ((bg >> 11) & 0x1Fu) * ia) >> 8);
  uint16_t g  = (uint16_t)((((fg >>  5) & 0x3Fu) * a + ((bg >>  5) & 0x3Fu) * ia) >> 8);
  uint16_t b  = (uint16_t)((((fg      ) & 0x1Fu) * a + ((bg      ) & 0x1Fu) * ia) >> 8);

  return (ui_color_t)((r << 11) | (g << 5) | b);
#elif (UI_COLOR_FMT == UI_COLOR_FMT_BGR565)
  uint16_t ia = (uint16_t)(256u - a);
  uint16_t r  = (uint16_t)((((fg      ) & 0x1Fu) * a + ((bg      ) & 0x1Fu) * ia) >> 8);
  uint16_t g  = (uint16_t)((((fg >>  5) & 0x3Fu) * a + ((bg >>  5) & 0x3Fu) * ia) >> 8);
  uint16_t b  = (uint16_t)((((fg >> 11) & 0x1Fu) * a + ((bg >> 11) & 0x1Fu) * ia) >> 8);

  return (ui_color_t)(r | (g << 5) | (b << 11));
#else
  uint32_t ia = (uint32_t)(256u - a);
  uint32_t r  = ((((fg >> 16) & 0xFFu) * a + ((bg >> 16) & 0xFFu) * ia) >> 8);
  uint32_t g  = ((((fg >>  8) & 0xFFu) * a + ((bg >>  8) & 0xFFu) * ia) >> 8);
  uint32_t b  = ((((fg      ) & 0xFFu) * a + ((bg      ) & 0xFFu) * ia) >> 8);
#if (UI_COLOR_FMT == UI_COLOR_FMT_ARGB8888)
  return (ui_color_t)(0xFF000000u | (r << 16) | (g << 8) | b);
#else
  return (ui_color_t)((r << 16) | (g << 8) | b);
#endif
#endif
}

/* 颜色线性插值：t/tmax 从 0 到 1，逐通道插值 */
ui_color_t ui_color_lerp(ui_color_t c0, ui_color_t c1, uint16_t t, uint16_t tmax)
{
  if (tmax == 0u)
  {
    tmax = 1u;
  }

#if (UI_COLOR_FMT == UI_COLOR_FMT_RGB565)
  int32_t r0 = (c0 >> 11) & 0x1F, g0 = (c0 >> 5) & 0x3F, b0 = c0 & 0x1F;
  int32_t r1 = (c1 >> 11) & 0x1F, g1 = (c1 >> 5) & 0x3F, b1 = c1 & 0x1F;

  return (ui_color_t)(((r0 + (r1 - r0) * (int32_t)t / (int32_t)tmax) << 11) |
                      ((g0 + (g1 - g0) * (int32_t)t / (int32_t)tmax) << 5)  |
                       (b0 + (b1 - b0) * (int32_t)t / (int32_t)tmax));
#elif (UI_COLOR_FMT == UI_COLOR_FMT_BGR565)
  int32_t r0 = c0 & 0x1F, g0 = (c0 >> 5) & 0x3F, b0 = (c0 >> 11) & 0x1F;
  int32_t r1 = c1 & 0x1F, g1 = (c1 >> 5) & 0x3F, b1 = (c1 >> 11) & 0x1F;

  return (ui_color_t)((r0 + (r1 - r0) * (int32_t)t / (int32_t)tmax) |
                      ((g0 + (g1 - g0) * (int32_t)t / (int32_t)tmax) << 5) |
                      ((b0 + (b1 - b0) * (int32_t)t / (int32_t)tmax) << 11));
#else
  int32_t r0 = (int32_t)((c0 >> 16) & 0xFFu), g0 = (int32_t)((c0 >> 8) & 0xFFu), b0 = (int32_t)(c0 & 0xFFu);
  int32_t r1 = (int32_t)((c1 >> 16) & 0xFFu), g1 = (int32_t)((c1 >> 8) & 0xFFu), b1 = (int32_t)(c1 & 0xFFu);
  int32_t r  = r0 + (r1 - r0) * (int32_t)t / (int32_t)tmax;
  int32_t g  = g0 + (g1 - g0) * (int32_t)t / (int32_t)tmax;
  int32_t b  = b0 + (b1 - b0) * (int32_t)t / (int32_t)tmax;

#if (UI_COLOR_FMT == UI_COLOR_FMT_ARGB8888)
  return (ui_color_t)(0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b);
#else
  return (ui_color_t)(((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b);
#endif
#endif
}

/* 把 [from_x, to_x] 这一段像素刷成 color，超出的部分裁到当前这条脏条以内。
 * 若设了视口裁剪：纵向不在裁剪矩形内的屏幕行整行不画，横向再夹到裁剪矩形 */
static void fill_span(int16_t from_x, int16_t to_x, ui_color_t color)
{
  ui_color_t *dst;

  if (span_clip(&from_x, &to_x) == 0u)
  {
    return;
  }

  /* 块内下标从当前这条脏条的左端开始数 */
  dst = ui_state.draw_row + (from_x - cur_left());
  for (int16_t x = from_x; x <= to_x; x++)
  {
    *dst++ = color;
  }
}

/* 把整段缓冲（draw_rows 行 × 当前这条的宽度）统统刷成背景色 */
void ui_row_buf_clear(ui_color_t color)
{
  ui_color_t *dst = buf_cur();
  uint32_t n = (uint32_t)ui_state.draw_rows * cur_width();

  for (uint32_t i = 0; i < n; i++)
  {
    dst[i] = color;
  }
}

/* ---- 供图元绘制使用的行级接口（见 ui_dirty_areas.h）------------------ */

uint16_t ui_row_draw_rows(void)
{
  return ui_state.draw_rows;
}

int16_t ui_row_draw_screen_y(uint16_t row)
{
  return (int16_t)(ui_state.draw_y0 + row);
}

ui_color_t* ui_row_draw_select(uint16_t row)
{
  ui_state.draw_row = buf_row(row);
  ui_state.draw_screen_y = (int16_t)(ui_state.draw_y0 + row);
  return ui_state.draw_row;
}

int16_t ui_row_draw_left(void)
{
  return cur_left();
}

int16_t ui_row_draw_right(void)
{
  return cur_right();
}

void ui_row_draw_span(int16_t from_x, int16_t to_x, ui_color_t color)
{
  fill_span(from_x, to_x, color);
}

void ui_row_draw_span_blend(int16_t from_x, int16_t to_x, ui_color_t color, uint8_t alpha)
{
  ui_color_t *dst;

  if (alpha == 0u)                       /* 全透明：一个像素都不动 */
  {
    return;
  }
  if (alpha >= 255u)                     /* 不透明：走原来的整段填色（快路径） */
  {
    fill_span(from_x, to_x, color);
    return;
  }
  if (span_clip(&from_x, &to_x) == 0u)
  {
    return;
  }

  /* 逐像素和行缓冲里"已经画好的下层"混合：块内下标同样从左端开始数 */
  dst = ui_state.draw_row + (from_x - cur_left());
  for (int16_t x = from_x; x <= to_x; x++)
  {
    *dst = ui_color_blend(*dst, color, alpha);
    dst++;
  }
}

/* ---- 视口裁剪 -------------------------------------------------------- */

/* 归一：保证 x0 <= x1、y0 <= y1（端点传反了也照样处理） */
static void clip_normalize(int16_t* x0, int16_t* y0, int16_t* x1, int16_t* y1)
{
  if (*x0 > *x1) { int16_t t = *x0; *x0 = *x1; *x1 = t; }
  if (*y0 > *y1) { int16_t t = *y0; *y0 = *y1; *y1 = t; }
}

/* 设一段矩形裁剪（屏幕坐标、含端点）：之后本片绘制只落在矩形内。
 * 端点顺序传反了也没关系，内部会归一 */
void ui_row_clip_set(int16_t x0, int16_t y0, int16_t x1, int16_t y1)
{
  ui_row_clip_set_round(x0, y0, x1, y1, 0u);
}

/* 设一段圆角裁剪：四角同一半径（0 = 矩形）。radius 内部夹到 min(w,h)/2 */
void ui_row_clip_set_round(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t radius)
{
  int16_t w, h, rmax;

  clip_normalize(&x0, &y0, &x1, &y1);

  w = (int16_t)(x1 - x0 + 1);
  h = (int16_t)(y1 - y0 + 1);
  if (w <= 0 || h <= 0)
  {
    /* 空矩形 = 整片都不可见：给一个 left > right 的区间（别把裁剪关掉） */
    ui_state.clip.on     = 1u;
    ui_state.clip.x0     = 1;
    ui_state.clip.y0     = 0;
    ui_state.clip.x1     = 0;
    ui_state.clip.y1     = 0;
    ui_state.clip.radius = 0u;
    return;
  }

  rmax = (int16_t)((w < h ? w : h) >> 1);
  if ((int32_t)radius > (int32_t)rmax)
  {
    radius = (uint8_t)rmax;                    /* rmax > 255 时上面这个判断不成立，不会被 8 位截断 */
  }

  ui_state.clip.on     = 1u;
  ui_state.clip.x0     = x0;
  ui_state.clip.y0     = y0;
  ui_state.clip.x1     = x1;
  ui_state.clip.y1     = y1;
  ui_state.clip.radius = radius;
}

void ui_row_clip_clear(void)
{
  ui_state.clip.on = 0u;
}

/* 绘制裁剪入栈：先保存当前层，再把新的一层设为当前。
 * 有效范围是"栈里所有层 ∩ 当前层"逐行求交（见 clip_row_range）。
 * 栈满时退化成"矩形求交、不保存"——只保证不漏画，圆角信息丢失 */
void ui_row_clip_push_round(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t radius)
{
  clip_normalize(&x0, &y0, &x1, &y1);

  if (ui_clip_depth < (uint8_t)UI_ROW_CLIP_MAX)
  {
    ui_clip_stack[ui_clip_depth] = ui_state.clip;
    ui_clip_depth++;
    ui_row_clip_set_round(x0, y0, x1, y1, radius);
    return;
  }

  /* 栈满：和当前层求交后直接替换，不保存 */
  if (ui_state.clip.on != 0u)
  {
    if (x0 < ui_state.clip.x0) { x0 = ui_state.clip.x0; }
    if (y0 < ui_state.clip.y0) { y0 = ui_state.clip.y0; }
    if (x1 > ui_state.clip.x1) { x1 = ui_state.clip.x1; }
    if (y1 > ui_state.clip.y1) { y1 = ui_state.clip.y1; }
  }
  ui_row_clip_set_round(x0, y0, x1, y1, 0u);
}

void ui_row_clip_push(int16_t x0, int16_t y0, int16_t x1, int16_t y1)
{
  ui_row_clip_push_round(x0, y0, x1, y1, 0u);
}

void ui_row_clip_pop(void)
{
  if (ui_clip_depth == 0u)
  {
    ui_state.clip.on = 0u;                     /* 没有可还原的：退回"不裁剪" */
    return;
  }

  ui_clip_depth--;
  ui_state.clip = ui_clip_stack[ui_clip_depth];
}

/* ---- 标脏裁剪用的保存记录 / 通用入栈出栈（不需要圆角） ---- */

typedef struct
{
  uint8_t on;
  int16_t x0, y0, x1, y1;
}ui_clip_saved_t;

/* 裁剪栈入栈（标脏裁剪用）：
 * 先把当前裁剪 (on, cx0..cy1) 存进 stack[depth]（栈满就只求交、不存），再与新矩形求交写回 */
static void clip_stack_push(uint8_t* on, int16_t* cx0, int16_t* cy0, int16_t* cx1, int16_t* cy1,
                            ui_clip_saved_t* stack, uint8_t* depth, uint8_t max_depth,
                            int16_t x0, int16_t y0, int16_t x1, int16_t y1)
{
  clip_normalize(&x0, &y0, &x1, &y1);

  /* 栈没满就先存一份当前裁剪，pop 时还原 */
  if (*depth < max_depth)
  {
    stack[*depth].on = *on;
    stack[*depth].x0 = *cx0;
    stack[*depth].y0 = *cy0;
    stack[*depth].x1 = *cx1;
    stack[*depth].y1 = *cy1;
    (*depth)++;
  }

  /* 与当前裁剪求交（当前没裁剪时就是本矩形），再压上 */
  if (*on)
  {
    if (x0 < *cx0) { x0 = *cx0; }
    if (y0 < *cy0) { y0 = *cy0; }
    if (x1 > *cx1) { x1 = *cx1; }
    if (y1 > *cy1) { y1 = *cy1; }
  }

  *cx0 = x0;
  *cy0 = y0;
  *cx1 = x1;
  *cy1 = y1;
  *on  = 1u;
}

/* 裁剪栈出栈（同上）：还原上一份；没得还原就退回"不裁剪" */
static void clip_stack_pop(uint8_t* on, int16_t* cx0, int16_t* cy0, int16_t* cx1, int16_t* cy1,
                           ui_clip_saved_t* stack, uint8_t* depth)
{
  if (*depth == 0u)
  {
    *on = 0u;                        /* 没有可还原的：退回"不裁剪" */
    return;
  }

  (*depth)--;
  *on  = stack[*depth].on;
  *cx0 = stack[*depth].x0;
  *cy0 = stack[*depth].y0;
  *cx1 = stack[*depth].x1;
  *cy1 = stack[*depth].y1;
}

/* ---- 标脏裁剪 -------------------------------------------------------- */

/* 栈深上限 UI_DIRTY_CLIP_MAX 在 ui_config.h */

static ui_clip_saved_t ui_dirty_clip_stack[UI_DIRTY_CLIP_MAX];
static uint8_t         ui_dirty_clip_depth = 0;

/* 开一段"标脏裁剪"：这段里 ui_dirty_mark 进来的块先夹到本矩形内，夹空了直接丢。
 * 和绘制裁剪一样存栈，嵌套时内层与外层求交 */
void ui_dirty_clip_push(int16_t x0, int16_t y0, int16_t x1, int16_t y1)
{
  clip_stack_push(&ui_state.dclip_on, &ui_state.dclip_x0, &ui_state.dclip_y0,
                  &ui_state.dclip_x1, &ui_state.dclip_y1,
                  ui_dirty_clip_stack, &ui_dirty_clip_depth, (uint8_t)UI_DIRTY_CLIP_MAX,
                  x0, y0, x1, y1);
}

void ui_dirty_clip_pop(void)
{
  clip_stack_pop(&ui_state.dclip_on, &ui_state.dclip_x0, &ui_state.dclip_y0,
                 &ui_state.dclip_x1, &ui_state.dclip_y1,
                 ui_dirty_clip_stack, &ui_dirty_clip_depth);
}

/* 游标那一行的屏幕 y 是否落在（所有层的）裁剪区里；没设裁剪恒为 1 */
uint8_t ui_row_draw_row_visible(void)
{
  int16_t l, r;

  return clip_row_range(ui_state.draw_screen_y, &l, &r);
}

/* 裁剪后本行的有效横向范围 = 脏条 ∩ 所有裁剪层（圆角已按行内缩）；没设裁剪就是脏条本身。
 * 整行不可见时返回一个空区间（left > right），调用方的循环自然不会画 */
int16_t ui_row_clip_left(void)
{
  int16_t l, r;

  if (clip_row_range(ui_state.draw_screen_y, &l, &r) == 0u)
  {
    return 1;
  }
  return l;
}

int16_t ui_row_clip_right(void)
{
  int16_t l, r;

  if (clip_row_range(ui_state.draw_screen_y, &l, &r) == 0u)
  {
    return 0;
  }
  return r;
}

/* ---- 脏矩形表 -------------------------------------------------------- */

/* 两块脏矩形相交时，合并是否划算：并集面积不超过两块面积和的 2 倍才合。
 * 这样"几乎重合的两条描边带"（old / new）会并成一条，省一次窗口；
 * 而"上带 + 左带"这种只在角上碰一下的不会被并成整个大框 —— 那会把窄带的收益全吃掉 */
static uint8_t dirty_merge_worth(const ui_dirty_rect_t *a, const ui_dirty_rect_t *b)
{
  int16_t lx, hx, ly, hy;
  int32_t union_area, sum_area;

  if (a->x1 < b->x0 || b->x1 < a->x0 || a->y1 < b->y0 || b->y1 < a->y0)
  {
    return 0;                            /* 不相交：没什么可合 */
  }

  lx = (a->x0 < b->x0) ? a->x0 : b->x0;
  hx = (a->x1 > b->x1) ? a->x1 : b->x1;
  ly = (a->y0 < b->y0) ? a->y0 : b->y0;
  hy = (a->y1 > b->y1) ? a->y1 : b->y1;

  union_area = (int32_t)(hx - lx + 1) * (int32_t)(hy - ly + 1);
  sum_area   = (int32_t)(a->x1 - a->x0 + 1) * (int32_t)(a->y1 - a->y0 + 1)
             + (int32_t)(b->x1 - b->x0 + 1) * (int32_t)(b->y1 - b->y0 + 1);

  return (union_area <= sum_area * 2) ? 1u : 0u;
}

/* ==================== 双/三缓冲:记住上一帧的脏区 ============================
 * 单缓冲时每帧只推"这一帧变了的窄带"就够了;双缓冲不行 —— 后台缓冲里躺着的是
 * "上上帧",只补"上一帧->这一帧"的差异是不够的。后台缓冲欠的是
 *     自这块缓冲上次当前台以来的所有变化 = 上一帧的脏区 ∪ 这一帧的脏区
 * 于是这里把上一帧的脏矩形(合并到不超过 UI_DIRTY_PREV_MAX 块)存下来,下一帧一开
 * 标脏就先把它们补进脏矩形表 —— 标脏 / 取片那套流程完全不用改。
 *
 * UI_DIRTY_KEEP_PREV: 总开关。改成 0 就退回单缓冲行为,一个像素都不多推。
 * UI_DIRTY_PREV_MAX : 上一帧最多记几块脏矩形(多的两两合并);以后三缓冲再扩。
 * 这两个都在 ui_config.h。
 * ========================================================================== */

#if UI_DIRTY_KEEP_PREV

static ui_dirty_rect_t ui_prev[UI_DIRTY_PREV_MAX];   /* 上一帧的脏矩形(<=UI_DIRTY_PREV_MAX 块) */
static uint8_t         ui_prev_count;
static ui_dirty_rect_t ui_cur[UI_DIRTY_PREV_MAX];    /* 这一帧自己标出来的脏矩形(<=UI_DIRTY_PREV_MAX 块) */
static uint8_t         ui_cur_count;
static uint8_t         ui_prev_seeded;               /* 本帧是否已经把 ui_prev 补进表了 */

/* 把一块并进一张"不超过 UI_DIRTY_PREV_MAX 块"的小表 t[0..*n-1]：
 *   已被包住 -> 丢；与某块相交且划算 -> 并进去；还有空位 -> 新记一块；
 *   满了 -> 先把"并起来面积最省"的两块并成一块腾出空位，再把 b 记上。
 * 并集只会更大、不会漏 */
static void dirty_small_add(ui_dirty_rect_t *t, uint8_t *n, const ui_dirty_rect_t *b)
{
  uint8_t i;
  uint8_t a = 0;
  uint8_t c = 1;
  int32_t best_cost = 0x7FFFFFFF;

  for (i = 0; i < *n; i++)                              /* 已被包住：不用记 */
  {
    if (b->x0 >= t[i].x0 && b->y0 >= t[i].y0 && b->x1 <= t[i].x1 && b->y1 <= t[i].y1)
    {
      return;
    }
  }

  for (i = 0; i < *n; i++)                              /* 和某块相交且划算：并进去 */
  {
    if (dirty_merge_worth(&t[i], b) != 0u)
    {
      if (b->x0 < t[i].x0) { t[i].x0 = b->x0; }
      if (b->y0 < t[i].y0) { t[i].y0 = b->y0; }
      if (b->x1 > t[i].x1) { t[i].x1 = b->x1; }
      if (b->y1 > t[i].y1) { t[i].y1 = b->y1; }
      return;
    }
  }

  if (*n < (uint8_t)UI_DIRTY_PREV_MAX)                  /* 还有空位：新记一块 */
  {
    t[*n] = *b;
    (*n)++;
    return;
  }

  /* 满了：挑"并起来面积最小"的两块并成一块，腾出一格（故 UI_DIRTY_PREV_MAX 需 >= 2） */
  for (i = 0; i + 1 < *n; i++)
  {
    for (uint8_t j = (uint8_t)(i + 1); j < *n; j++)
    {
      int16_t lx = (t[i].x0 < t[j].x0) ? t[i].x0 : t[j].x0;
      int16_t hx = (t[i].x1 > t[j].x1) ? t[i].x1 : t[j].x1;
      int16_t ly = (t[i].y0 < t[j].y0) ? t[i].y0 : t[j].y0;
      int16_t hy = (t[i].y1 > t[j].y1) ? t[i].y1 : t[j].y1;
      int32_t cost = (int32_t)(hx - lx + 1) * (int32_t)(hy - ly + 1);

      if (cost < best_cost) { best_cost = cost; a = i; c = j; }
    }
  }

  /* 并到 t[a]，再把 t[c] 删掉（用最后一块补上空位） */
  if (t[a].x0 > t[c].x0) { t[a].x0 = t[c].x0; }
  if (t[a].y0 > t[c].y0) { t[a].y0 = t[c].y0; }
  if (t[a].x1 < t[c].x1) { t[a].x1 = t[c].x1; }
  if (t[a].y1 < t[c].y1) { t[a].y1 = t[c].y1; }

  if (c != (uint8_t)(*n - 1))
  {
    t[c] = t[*n - 1];
  }
  (*n)--;

  t[*n] = *b;
  (*n)++;
}

#endif /* UI_DIRTY_KEEP_PREV */

/* 把一块脏矩形并进主表：已被某块包住就丢；和某块相交且合并划算就并成一块；否则单独记一块；
 * 表满时按代价决定"并进最合适那块"还是"整屏重画"。
 * 标脏（ui_dirty_mark）和"把上一帧的脏块补进来"两条路都走这里，合并口径一致 */
static void dirty_table_add(const ui_dirty_rect_t *b)
{
  ui_dirty_rect_t blk = *b;
  uint8_t i;
  uint8_t merged;

  /* 已经被表里某一块包住了：不用再记 */
  for (i = 0; i < ui_state.count; i++)
  {
    const ui_dirty_rect_t *r = &ui_state.rects[i];

    if (blk.x0 >= r->x0 && blk.y0 >= r->y0 && blk.x1 <= r->x1 && blk.y1 <= r->y1)
    {
      return;
    }
  }

  /* 把划算合的合进来。合完 blk 变大，可能又碰到别的，所以循环到不再变化 */
  do
  {
    merged = 0;
    for (i = 0; i < ui_state.count; i++)
    {
      ui_dirty_rect_t *r = &ui_state.rects[i];

      if (dirty_merge_worth(r, &blk) != 0u)
      {
        if (r->x0 < blk.x0) { blk.x0 = r->x0; }
        if (r->y0 < blk.y0) { blk.y0 = r->y0; }
        if (r->x1 > blk.x1) { blk.x1 = r->x1; }
        if (r->y1 > blk.y1) { blk.y1 = r->y1; }

        /* 并进来的这块不再单独留着：后面的往前挪一格 */
        for (uint8_t m = i; m + 1 < ui_state.count; m++)
        {
          ui_state.rects[m] = ui_state.rects[m + 1];
        }
        ui_state.count--;
        merged = 1;
        break;                 /* blk 变了，重新从头扫 */
      }
    }
  } while (merged != 0u);

  if (ui_state.count < ui_state.max_rects)
  {
    ui_state.rects[ui_state.count] = blk;
    ui_state.count++;
  }
  else
  {
    /* 表满了：先算"并进哪一块、多推的像素最少"。
     *   代价不大 → 并进去：覆盖正确，这帧只多推一小块；
     *   代价很大 → 整屏标脏、并把表清成一块：这帧退化成整屏重画，
     *              形状可预测，也顺手把攒出来的碎片清掉。
     * 两条路都不会漏像素（并集 / 整屏都只会比真实脏区更大） */
    uint8_t         best      = 0;
    int32_t         best_cost = 0x7FFFFFFF;

    for (i = 0; i < ui_state.count; i++)
    {
      const ui_dirty_rect_t *e = &ui_state.rects[i];
      int16_t lx = (e->x0 < blk.x0) ? e->x0 : blk.x0;
      int16_t hx = (e->x1 > blk.x1) ? e->x1 : blk.x1;
      int16_t ly = (e->y0 < blk.y0) ? e->y0 : blk.y0;
      int16_t hy = (e->y1 > blk.y1) ? e->y1 : blk.y1;
      int32_t union_area = (int32_t)(hx - lx + 1) * (int32_t)(hy - ly + 1);
      int32_t own_area   = (int32_t)(e->x1 - e->x0 + 1) * (int32_t)(e->y1 - e->y0 + 1);
      int32_t cost       = union_area - own_area;      /* 并进来要多推多少像素 */

      if (cost < best_cost)
      {
        best_cost = cost;
        best      = i;
      }
    }

    if (best_cost > DIRTY_OVERFLOW_COST_MAX)
    {
      /* 并进去会把它撑成一大块：不如整屏一块，可预测地退化成整屏重画 */
      ui_state.rects[0].x0 = 0;
      ui_state.rects[0].y0 = 0;
      ui_state.rects[0].x1 = (int16_t)(MAX_W - 1);
      ui_state.rects[0].y1 = (int16_t)(MAX_H - 1);
      ui_state.count       = 1;
    }
    else
    {
      ui_dirty_rect_t *r = &ui_state.rects[best];

      if (blk.x0 < r->x0) { r->x0 = blk.x0; }
      if (blk.y0 < r->y0) { r->y0 = blk.y0; }
      if (blk.x1 > r->x1) { r->x1 = blk.x1; }
      if (blk.y1 > r->y1) { r->y1 = blk.y1; }
    }
  }
}

/* 登记一块脏矩形 [x0,y0]-[x1,y1]：夹回屏幕内，再并进脏矩形表。
 * 已被表里某块包住就丢掉；和某块相交且合并划算就并成一块；否则单独记一块。
 * 一帧里可以调多次，几处脏区各记各的。
 * 调用者尽量只标"真正变了的窄带"，别整块整块标 —— 每帧推送量直接由此决定 */
void ui_dirty_mark(int16_t x0, int16_t y0, int16_t x1, int16_t y1)
{
  ui_dirty_rect_t blk;

  if (x0 > x1) { int16_t t = x0; x0 = x1; x1 = t; }
  if (y0 > y1) { int16_t t = y0; y0 = y1; y1 = t; }

  if (x0 < 0)         { x0 = 0; }
  if (y0 < 0)         { y0 = 0; }
  if (x1 > ui_state.screen_w - 1) { x1 = (int16_t)(ui_state.screen_w - 1); }
  if (y1 > ui_state.screen_h - 1) { y1 = (int16_t)(ui_state.screen_h - 1); }
  if (x0 > x1 || y0 > y1) { return; }

  /* 标脏裁剪：先夹到当前裁剪矩形内 —— 夹空了说明这块整个在窗口外，
   * 那部分本来就没画过、也没变化，不用推 */
  if (ui_state.dclip_on)
  {
    if (x0 < ui_state.dclip_x0) { x0 = ui_state.dclip_x0; }
    if (y0 < ui_state.dclip_y0) { y0 = ui_state.dclip_y0; }
    if (x1 > ui_state.dclip_x1) { x1 = ui_state.dclip_x1; }
    if (y1 > ui_state.dclip_y1) { y1 = ui_state.dclip_y1; }
    if (x0 > x1 || y0 > y1) { return; }
  }

  blk.x0 = x0; blk.y0 = y0; blk.x1 = x1; blk.y1 = y1;

#if UI_DIRTY_KEEP_PREV
  /* 这一帧自己的脏区另记一份（记的是原始标脏块，不含补进来的上一帧那部分），
   * 下一帧帧首它就是"上一帧的脏区"。记的时候两两合并，压到 <= UI_DIRTY_PREV_MAX 块 */
  dirty_small_add(ui_cur, &ui_cur_count, &blk);
#endif

  dirty_table_add(&blk);
}

/* 开一帧：先把上一帧自己标的脏区存成"上一帧的脏区"，再把它原样补进本帧的脏矩形表。
 *
 * 为什么必须每帧无条件调一次（哪怕这一帧一个 ui_dirty_mark 都不会有）：
 * 双缓冲下后台缓冲里躺着的是"上上帧"，它欠的是【自这块缓冲上次显示以来的全部变化】
 * = 上一帧的脏区 ∪ 这一帧的脏区。上一帧那半只能靠这里补 —— 动画停住、本帧一点没标时
 * 更要补，否则这块后台缓冲就一直停在两帧前，翻过来会闪一下旧画面。
 *
 * 调用的位置：一帧开画之前（切好后台缓冲之后、推进动画之前）。
 * UI_DIRTY_KEEP_PREV=0 时本函数什么也不做，就是单缓冲行为 */
void ui_dirty_frame_begin(void)
{
#if UI_DIRTY_KEEP_PREV
  uint8_t i;

  /* 上一帧确实开过（prev_seeded）：把它的脏区收成"上一帧"。
   * 首帧不重置 ui_cur：开机那次整屏标脏要算进第一帧，才能在下一帧被补上 */
  if (ui_prev_seeded != 0u)
  {
    ui_prev_count = ui_cur_count;
    for (i = 0; i < ui_cur_count; i++)
    {
      ui_prev[i] = ui_cur[i];
    }
    ui_cur_count = 0u;
  }
  ui_prev_seeded = 1u;

  /* 把"上一帧的脏区"补进本帧的表 */
  for (i = 0; i < ui_prev_count; i++)
  {
    dirty_table_add(&ui_prev[i]);
  }
#endif
}

/* 取下一片要推的：先从脏矩形表里挑最靠上的一块，推不完就按行切片、分几次推完。
 * 每次调用给出一片：行范围 [start_y, end_y]、横向范围 [left_x, right_x]。
 * 一帧的脏块全取完（返回 0）才轮到动画推进，于是跨块/跨片的形状不会被拉斜。
 * 窄高的矩形（比如光标的竖边）宽度很小，一片就能把整条推完，一次 DMA 落位 */
uint8_t ui_dirty_take(ui_dirty_span_t *span)
{
  uint16_t width;
  uint16_t max_rows;
  uint16_t rows;
  uint8_t  i;
  uint8_t  best;

  if (span == NULL)
  {
    return 0;
  }

  if (ui_state.has_cur == 0u)
  {
    if (ui_state.count == 0u)
    {
      return 0;                        /* 这一帧的脏块都推完了 */
    }

    /* 挑最靠上的一块，保持"从上往下推"的手感 */
    best = 0;
    for (i = 1; i < ui_state.count; i++)
    {
      if (ui_state.rects[i].y0 < ui_state.rects[best].y0)
      {
        best = i;
      }
    }

    ui_state.cur = ui_state.rects[best];
    for (i = best; i + 1 < ui_state.count; i++)
    {
      ui_state.rects[i] = ui_state.rects[i + 1];
    }
    ui_state.count--;
    ui_state.cur_y   = ui_state.cur.y0;
    ui_state.has_cur = 1u;
  }

  /* 切一片：整宽 × 尽量多行，装不下就少几行。
   * draw_rows / ui_row_draw_rows() 现在是 uint16_t，一片切到 MAX_H(600) 行也不会被截断，
   * 所以不必再压到 255；只要保证不超过行缓冲容量、再夹到屏幕高度即可 */
  width    = (uint16_t)(ui_state.cur.x1 - ui_state.cur.x0 + 1);
  max_rows = (uint16_t)(BUF_PIXELS / width);
  if (max_rows < 1u) { max_rows = 1u; }
  /* 以防万一：一片的行数上限就是屏高，夹住它免得任何宽度算出来的 max_rows 溢出到超范围 */
  if (max_rows > (uint16_t)MAX_H) { max_rows = (uint16_t)MAX_H; }

  rows = (uint16_t)(ui_state.cur.y1 - ui_state.cur_y + 1);
  if (rows > max_rows)         { rows = max_rows; }
  if (rows > (uint16_t)MAX_H)  { rows = (uint16_t)MAX_H; }

  ui_state.chunk_rows = rows;

  span->left_x  = (uint16_t)ui_state.cur.x0;
  span->right_x = (uint16_t)ui_state.cur.x1;
  span->start_y = (uint16_t)ui_state.cur_y;
  span->end_y   = (uint16_t)(ui_state.cur_y + (int16_t)rows - 1);

  ui_state.cur_y = (int16_t)(ui_state.cur_y + (int16_t)rows);
  if (ui_state.cur_y > ui_state.cur.y1)
  {
    ui_state.has_cur = 0u;             /* 这一块推完了 */
  }
  return 1;
}

/* 这一"视觉帧"的脏区还有没有没推完的：表里还有块、或手上这块还没推完，都算没完。
 * 主循环靠它决定"动画能不能往下推进"：一帧的脏块要全按同一个动画时刻画出来并发出，
 * 不能让动画插在中间改几何，否则跨块/跨片的形状上下会错位、被拉斜 */
uint8_t ui_dirty_pending(void)
{
  return (ui_state.count > 0u || ui_state.has_cur != 0u) ? 1u : 0u;
}

/* 设置一帧最多攒多少个脏矩形：夹到 [UI_DIRTY_MAX_MIN, UI_DIRTY_MAX_CAP]。
 * 不调就用满静态容量。调小：每帧的扫描 / 合并更快，但更早触发"并进最划算那块 / 整屏重画"的退化。
 * 初始化阶段调一次即可（比如 LCD 起来之后、开始标脏之前） */
void ui_dirty_set_max(uint8_t max_rects)
{
  if (max_rects < (uint8_t)UI_DIRTY_MAX_MIN)
  {
    max_rects = (uint8_t)UI_DIRTY_MAX_MIN;
  }
  if (max_rects > (uint8_t)UI_DIRTY_MAX_CAP)
  {
    max_rects = (uint8_t)UI_DIRTY_MAX_CAP;
  }
  ui_state.max_rects = max_rects;
}

/* 设置实际屏幕尺寸(像素)：标脏按它夹边界。
 * 必须在开始标脏之前调一次(和 ui_dirty_set_max 一起，LCD 起来之后)。
 * 不调就退化成 MAX_W/MAX_H(驱动支持的最大屏) —— 真屏比它小时，
 * 屏外的坐标会被当合法脏区收进来，最后 DMA 越界写到别的行上 */
void ui_dirty_set_screen(int16_t w, int16_t h)
{
  if (w < 1 || w > (int16_t)MAX_W) { w = (int16_t)MAX_W; }
  if (h < 1 || h > (int16_t)MAX_H) { h = (int16_t)MAX_H; }

  ui_state.screen_w = w;
  ui_state.screen_h = h;
}

int16_t ui_dirty_screen_w(void)
{
  return ui_state.screen_w;
}

int16_t ui_dirty_screen_h(void)
{
  return ui_state.screen_h;
}

/* ==========================================================================
 *  推屏流水线：取脏条 -> 画一片 -> 交给硬件(两步交替，画和搬重叠)
 *
 *  流水线本身只和"脏条表 + 行缓冲"打交道，硬件(推屏后端)和场景(怎么画一片)
 *  都由外面用钩子接进来 —— 见 ui_dirty_areas.h 里 ui_flush_dev_t / ui_scene_draw_fn。
 *  每次调用只做一步，不阻塞，调用方(主循环)每轮调一次即可，等硬件的空档还能干别的。
 * ========================================================================== */
static const ui_flush_dev_t *s_rd_dev   = NULL;  /* 推屏后端(硬件的两头) */
static ui_scene_draw_fn      s_rd_scene = NULL;  /* 画一片场景 */
static ui_overlay_draw_fn    s_rd_overlay = NULL;/* 场景画完后的浮层(调试 HUD 等) */

static ui_row_chunk_t s_rd_hold;                 /* 画好、还没交给硬件的那一片 */
static uint8_t        s_rd_holding = 0u;         /* 1 = 手上有一片没发出去 */

void ui_render_set_overlay(ui_overlay_draw_fn fn)
{
  s_rd_overlay = fn;
}

void ui_render_init(const ui_flush_dev_t *dev, ui_scene_draw_fn scene)
{
  s_rd_dev     = dev;
  s_rd_scene   = scene;
  s_rd_holding = 0u;
}

void ui_render_slice(void)
{
  ui_dirty_span_t span;
  ui_row_chunk_t  chunk;

  if (s_rd_dev == NULL || s_rd_scene == NULL)
  {
    return;                                    /* 还没接上后端 / 场景 */
  }

  /* ① 手上还有"画好没发"的片：先确认硬件把它的前一片搬完(乒乓缓冲只有两块) */
  if (s_rd_holding != 0u)
  {
    if (s_rd_dev->busy() != 0u)
    {
      return;                                  /* 上一片还在搬，这一轮先等 */
    }

    s_rd_dev->start(&s_rd_hold);               /* 交给硬件 */
    s_rd_holding = 0u;
    return;
  }

  /* ② 手上没片：取下一片，画进【另一块】行缓冲 —— 这时上一片的搬运正在重叠进行 */
  if (ui_dirty_take(&span) == 0)
  {
    return;                                    /* 没有待推的脏区了 */
  }

  ui_row_buf_set_y(span.start_y);              /* 开一片 */
  s_rd_scene();                                /* 画这一片：铺底 + 按优先级画整屏对象 */

  if (s_rd_overlay != NULL)
  {
    s_rd_overlay();                            /* 浮层最后画：永远压在页面内容上面 */
  }

  chunk = ui_row_buf_swap();                   /* 交出手上这一片 */

  if (chunk.row_count == 0u)
  {
    return;                                    /* 空片不推：DMA2D 收到 ey<sy 会等 TC 等到超时(卡) */
  }

  /* 先存下来，下一轮(确认前一片搬完)再交给硬件 —— 这样"画下一片"挤在"搬上一片"期间做 */
  s_rd_hold    = chunk;
  s_rd_holding = 1u;
}

uint8_t ui_render_idle(void)
{
  if (s_rd_dev == NULL || s_rd_scene == NULL)
  {
    return 1u;                                 /* 没接后端就没什么可推的 */
  }

  if (ui_dirty_pending() != 0u)
  {
    return 0u;                                 /* 还有脏块没被取走 */
  }
  if (s_rd_holding != 0u)
  {
    return 0u;                                 /* 手上有片还没发 */
  }

  return (uint8_t)((s_rd_dev->busy() == 0u) ? 1u : 0u);   /* 硬件也搬完了才算推完 */
}
