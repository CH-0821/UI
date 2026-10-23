#include "./ui_create/ui_create.h"
#include "./ui_font/ui_font_data.h"   /* 内置字库点阵(没挂回调时兜底用) */
#include "./ui_tool/ui_tool.h"     /* ui_tool_round_rect_row_ex：矩形可选圆角 */

/* 由"中心 + 宽高"反推四边（含端点）。全工程唯一一份，见 ui_create.h 的声明说明 */
void ui_box_of(int16_t cx, int16_t cy, int16_t w, int16_t h,
               int16_t* x_min, int16_t* y_min, int16_t* x_max, int16_t* y_max)
{
  if (x_min == NULL || y_min == NULL || x_max == NULL || y_max == NULL)
  {
    return;
  }

  *x_min = (int16_t)(cx - (w >> 1));
  *y_min = (int16_t)(cy - (h >> 1));
  *x_max = (int16_t)(*x_min + w - 1);
  *y_max = (int16_t)(*y_min + h - 1);
}

/* 把一个矩形框的四条描边带登记成脏区，**不标内部**。
 * 上 / 下两条横带整宽，左 / 右两条竖带只标中间那些行（四角已被上下带盖住） */
static void mark_one_outline(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t b)
{
  if (x0 > x1 || y0 > y1)
  {
    return;
  }

  ui_dirty_mark(x0, y0, x1, (int16_t)(y0 + b - 1));                  /* 上带 */
  ui_dirty_mark(x0, (int16_t)(y1 - b + 1), x1, y1);                  /* 下带 */

  if ((y1 - y0 + 1) > (int16_t)(2 * b))                              /* 中间那些行才有竖带 */
  {
    ui_dirty_mark(x0, (int16_t)(y0 + b), (int16_t)(x0 + b - 1), (int16_t)(y1 - b));
    ui_dirty_mark((int16_t)(x1 - b + 1), (int16_t)(y0 + b), x1, (int16_t)(y1 - b));
  }
}

/* 矩形 / 光标"移动或形变"之后登记脏区：只标它真正画出来的四条描边带，
 * old 和 new 两个框的描边都包进去（几乎重合的两条会被自动并成一条），
 * **不标内部** —— 内部是背景，被框住的其它对象会各自把露出来的部分标脏。
 * 于是光标一帧只推"两条 3px 竖带 + 两条 3px 横带"，而不是整个 154×64 的大框；
 * 那两条竖带又窄又高，各是一片 DMA 就推完的一条，不会被切成 13 段 —— 台阶就是这么消掉的。
 * 参数给的是 old / new 两个框的四条边（含端点），border_width 是描边宽度 */
void ui_dirty_mark_rect_outline(int16_t x0_old, int16_t y0_old, int16_t x1_old, int16_t y1_old,
                                int16_t x0_new, int16_t y0_new, int16_t x1_new, int16_t y1_new,
                                int16_t border_width)
{
  int16_t b = (border_width < 1) ? 1 : border_width;

  mark_one_outline(x0_old, y0_old, x1_old, y1_old, b);
  mark_one_outline(x0_new, y0_new, x1_new, y1_new, b);
}

/* 矩形类对象"移动 / 缩放 / 形变"后的脏区登记：old / new 两个框都算进去。
 * 描边矩形 / 光标只画四条边带，标边带即可；实心填充矩形整框都是色，必须标整框 */
void ui_dirty_mark_rect_move(int16_t x0_old, int16_t y0_old, int16_t x1_old, int16_t y1_old,
                             int16_t x0_new, int16_t y0_new, int16_t x1_new, int16_t y1_new,
                             int16_t border_width, uint8_t filled)
{
  if (filled)
  {
    ui_dirty_mark(x0_old, y0_old, x1_old, y1_old);
    ui_dirty_mark(x0_new, y0_new, x1_new, y1_new);
    return;
  }
  ui_dirty_mark_rect_outline(x0_old, y0_old, x1_old, y1_old,
                             x0_new, y0_new, x1_new, y1_new, border_width);
}

/* 行缓冲做成乒乓：一片在画、另一片可以同时由 DMA 发出去。
 * 发送是按整片一起发的，所以画也按整片来：下面每个函数一次把 draw_rows 行全画完，
 * 片内每行自己算该行的内容，写到"块首 + 行号 × 行宽"的位置。
 * 帧层只负责：set_y 开一片 → clear 铺底 → draw_xxx 画满 → swap 取走发送。
 *
 * 行缓冲 / 脏条表都在 ui_dirty_areas 模块里；这里只通过它暴露的行级接口
 * （ui_row_draw_rows / screen_y / select / left / right / span）读写当前这一片。
 */
/* 各图元通用的带 alpha 上色：0(未设) / 255 走整段填色（一个字节都不变），
 * 其余逐像素和下层混合（半透明卡片 / 遮罩）。各 para 的 alpha 都是这套语义 */
void ui_span_alpha(int16_t x0, int16_t x1, ui_color_t color, uint8_t alpha)
{
  if (alpha == 0u || alpha >= 255u)
  {
    ui_row_draw_span(x0, x1, color);
  }
  else
  {
    ui_row_draw_span_blend(x0, x1, color, alpha);
  }
}

/* 矩形专用封装：颜色从参数给（渐变按行算），alpha 读 para->alpha */
void ui_rect_span(const ui_rect_para_t* para, int16_t x0, int16_t x1, ui_color_t color)
{
  ui_span_alpha(x0, x1, color, para->alpha);
}

void ui_draw_rect(void* p)
{
  ui_rect_para_t* para = p;
  if(para->head.type != UI_TYPE_RECT)
  {
    return;
  }
  /* 参数给的是中心点和宽高，先换算成上下左右四条边（含端点） */
  int16_t x_min = para->head.center_x - (para->w >> 1); /* >>1 = 除2*/
  int16_t x_max = x_min + para->w - 1;
  int16_t y_min = para->head.center_y - (para->h >> 1); /* >>1 = 除2*/
  int16_t y_max = y_min + para->h - 1;
  int16_t border = para->border_width;   /* 线宽，往内画 */

  /* 跟当前这条脏条横向都不相交，整段都不用画 */
  if (x_max < ui_row_draw_left() || x_min > ui_row_draw_right())
  {
    return;
  }

  for (uint16_t row = 0; row < ui_row_draw_rows(); row++)
  {
    /* 本行在屏幕上的行号 */
    int16_t y = ui_row_draw_screen_y(row);
    int16_t row_in_rect;
    int16_t x0, x1;

    /* 不在矩形的上下范围内，这行没它的事 */
    if (y < y_min || y > y_max)
    {
      continue;
    }

    /* 本行实际该涂的横向范围：设了 corners 就按圆角缩，没设就整宽
     * (corners = 0 这条分支和以前逐像素一致，不圆角的矩形一个字节都不变) */
    if (para->corners != 0u)
    {
      if (ui_tool_round_rect_row_ex(para->head.center_x, para->head.center_y,
                                    para->w, para->h, para->radius, y,
                                    para->corners, &x0, &x1) == 0u)
      {
        continue;
      }
    }
    else
    {
      x0 = x_min;
      x1 = x_max;
    }

    /* 把绘制游标指到这一行 */
    ui_row_draw_select(row);

    /* 实心填充：整行铺满，没有"描边 / 内部"之分 */
    if (para->fill)
    {
      ui_rect_span(para, x0, x1, para->color);
      continue;
    }

    row_in_rect = y - y_min;   /* 本行是矩形里的第几行，最上面一行是 0 */

    /* 落在上/下边框里：整行都是线 */
    if (row_in_rect < border || row_in_rect >= para->h - border)
    {
      ui_rect_span(para, x0, x1, para->color);
      continue;
    }

    /* 中间行：只有左右两条竖边。线宽往内画，太宽时夹回矩形内 */
    int16_t left_end = x0 + border - 1;
    int16_t right_begin = x1 - border + 1;
    if (left_end > x1)
    {
      left_end = x1;
    }
    if (right_begin < x0)
    {
      right_begin = x0;
    }

    ui_rect_span(para, x0, left_end, para->color);
    ui_rect_span(para, right_begin, x1, para->color);
  }
}

/* 整数平方根(向下取整)：算圆 / 圆环每行的横向半宽 = sqrt(r? - dy?)，避免引 math.h。
 * 经典逐位开方，int 范围内几次循环就完事 */
static int32_t ui_isqrt32(int32_t v)
{
  int32_t r = 0;
  int32_t b = 1 << 30;

  if (v <= 0) { return 0; }
  while (b > v) { b >>= 2; }
  while (b != 0)
  {
    if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
    else            { r >>= 1; }
    b >>= 2;
  }
  return r;
}

/* 圆环：把"到圆心距离落在 [internal_r, external_r] 之间"的点都画上。internal_r 传 0 就是实心圆。
 * 每行只扫圆自己那一小段 [cx ± sqrt(outer?-dy?)]（不再从脏条左端扫到右端），
 * 再按"外圆范围 ? 内圆范围"直接算成 1~2 段交给 ui_row_draw_span 涂（它自带脏条 / 裁剪）：
 *   实心圆、或这一行不穿内孔 -> 整段 [x_lo, x_hi]；
 *   穿内孔                    -> 左右两段 [x_lo, cx-半孔宽-1] 和 [cx+半孔宽+1, x_hi]。
 * 内孔半宽取 sqrt(inner?-dy?-1)：dx? < inner?-dy? 才算在孔里，和外圆口径逐像素一致 */
void ui_draw_circle(void* p)
{
  ui_circle_para_t* para = p;
  int32_t outer_r2, inner_r2;

  if (para->head.type != UI_TYPE_CIRCLE)
  {
    return;
  }
  if (para->external_r <= 0)
  {
    return;
  }

  outer_r2 = (int32_t)para->external_r * para->external_r;
  inner_r2 = (int32_t)para->internal_r * para->internal_r;
  if (inner_r2 > outer_r2) { inner_r2 = outer_r2; }

  for (uint16_t row = 0; row < ui_row_draw_rows(); row++)
  {
    int16_t y   = ui_row_draw_screen_y(row);
    int32_t dy  = (int32_t)y - para->head.center_y;
    int32_t dy2 = dy * dy;
    int16_t cx  = para->head.center_x;
    int32_t rx;
    int16_t x_lo, x_hi;

    if (dy2 > outer_r2)
    {
      continue;                                   /* 整行都在圆外 */
    }

    /* 只扫圆在这一行的横向范围 [cx ± sqrt(outer?-dy?)]，再夹到当前脏条 */
    rx   = ui_isqrt32(outer_r2 - dy2);
    x_lo = (int16_t)(cx - rx);
    x_hi = (int16_t)(cx + rx);
    if (x_lo < ui_row_draw_left())  { x_lo = ui_row_draw_left(); }
    if (x_hi > ui_row_draw_right()) { x_hi = ui_row_draw_right(); }
    if (x_lo > x_hi)
    {
      continue;
    }

    ui_row_draw_select(row);                      /* 涂之前把行游标指到这一行 */

    if (inner_r2 > 0 && dy2 < inner_r2)
    {
      /* 这一行穿内孔：挖掉 |dx| <= sqrt(inner?-dy?-1) 那段，左右各涂一段 */
      int16_t hh = (int16_t)ui_isqrt32(inner_r2 - dy2 - 1);
      int16_t l1 = (int16_t)(cx - hh - 1);
      int16_t r1 = (int16_t)(cx + hh + 1);

      if (x_lo <= l1) { ui_span_alpha(x_lo, l1, para->color, para->alpha); }
      if (r1 <= x_hi) { ui_span_alpha(r1, x_hi, para->color, para->alpha); }
    }
    else
    {
      ui_span_alpha(x_lo, x_hi, para->color, para->alpha);
    }
  }
}

/* 点：一个实心小方块，边长 = size（像素），画在中心处。
 * size = 1 就是单个像素点，每 +1 就是再往外包一层像素。
 * 边界跟 ui_draw_rect 同一套（x_min = 中心 - size/2、x_max = x_min + size - 1），
 * 这样脏区记账和画出来的像素才能逐像素对齐 */
void ui_draw_point(void* p)
{
  ui_point_para_t* para = p;
  int16_t x_min, x_max, y_min, y_max;

  if (para->head.type != UI_TYPE_POINT)
  {
    return;
  }

  x_min = (int16_t)(para->head.center_x - (para->size >> 1)); /* >>1 = 除2 */
  x_max = (int16_t)(x_min + para->size - 1);
  y_min = (int16_t)(para->head.center_y - (para->size >> 1));
  y_max = (int16_t)(y_min + para->size - 1);

  /* 跟当前这条脏条横向都不相交，整段都不用画 */
  if (x_max < ui_row_draw_left() || x_min > ui_row_draw_right())
  {
    return;
  }

  for (uint16_t row = 0; row < ui_row_draw_rows(); row++)
  {
    int16_t y = ui_row_draw_screen_y(row);

    /* 不在点的上下范围内，这行没它的事 */
    if (y < y_min || y > y_max)
    {
      continue;
    }

    /* 把绘制游标指到这一行，实心：整行铺满 */
    ui_row_draw_select(row);
    ui_span_alpha(x_min, x_max, para->color, para->alpha);
  }
}

/* 图片 / 图标：规格 w×h 的 RGB565 裸像素，画在"虚拟中心"处。
 * 平移走变换块 xf（虚拟中心 = 锚点 + 偏移 × 因子，和矩形同一套；
 * 当前不做缩放，因子恒为 1，但公式留着 —— 以后加缩放只登记分派表就行）。
 * 每个屏幕行只搬图片对应的那一行：几何（平移 / 纵向范围）在这里算，
 * 裁剪（出脏条 / 出视窗）全在 ui_tool 的按行搬运里做。
 * 像素来源二选一（见 ui_image_para_t）：pixels 直读片内点阵；row_fn 按行读片外图标 */
void ui_draw_image(void* p)
{
  ui_image_para_t* para = p;
  ui_color_t line_buf[UI_IMG_ROW_BUF_PIX];   /* 片外取行的落地缓冲（row_fn 用） */
  int16_t  x_min, x_max, y_min, y_max;
  int16_t  cx, cy;

  if (para->head.type != UI_TYPE_IMAGE)
  {
    return;
  }
  if (para->w <= 0 || para->h <= 0)
  {
    return;
  }
  if (para->pixels == NULL && para->row_fn == NULL)
  {
    return;                                /* 像素来源一个都没给 */
  }

  /* 虚拟中心：偏移按当前因子换算（借折线那个"锚点 + 偏移 × 因子"的换算，同一套语义） */
  cx = ui_poly_virtual(para->xf.anchor_off_x, para->xf.anchor_x,
                       para->xf.scale_num, para->xf.scale_den);
  cy = ui_poly_virtual(para->xf.anchor_off_y, para->xf.anchor_y,
                       para->xf.scale_num, para->xf.scale_den);

  /* 由中心 + 规格 反推四边（跟矩形同约定：x_max = x_min + w - 1） */
  x_min = (int16_t)(cx - (para->w >> 1));
  x_max = (int16_t)(x_min + para->w - 1);
  y_min = (int16_t)(cy - (para->h >> 1));
  y_max = (int16_t)(y_min + para->h - 1);

  /* 跟当前这条脏条横向都不相交，整段都不用画 */
  if (x_max < ui_row_draw_left() || x_min > ui_row_draw_right())
  {
    return;
  }

  for (uint16_t row = 0; row < ui_row_draw_rows(); row++)
  {
    int16_t y = ui_row_draw_screen_y(row);

    /* 不在图片纵向范围内，这行没它的事 */
    if (y < y_min || y > y_max)
    {
      continue;
    }

    if (para->pixels != NULL)
    {
      /* 片内点阵：本行首址 = 基址 + 行号 × 行宽；裁剪 / 搬运交给工具 */
      ui_tool_image_row(row, para->pixels + (int32_t)(y - y_min) * para->w,
                        x_min, x_max, para->key, para->key_on, para->alpha);
    }
    else
    {
      /* 片外图标：先经回调读进 line_buf 再搬（工具里分批读，规格再宽也不截断） */
      ui_tool_image_row_read(row, para->row_fn, para->img_id,
                             (int16_t)(y - y_min), x_min, x_max,
                             line_buf, (uint16_t)UI_IMG_ROW_BUF_PIX,
                             para->key, para->key_on, para->alpha);
    }
  }
}

/* 字形描述 ui_glyph_t 已提到 ui_create.h —— 汉字取模回调(ui_hz_glyph_fn_t)也要用它，
 * 绘制和字库两侧都认同一份定义 */

/* 取一个 ASCII 字符(0x20~0x7E)的字形。
 * 字库里 ASCII 按"字高"分档：12/16/24/32，字宽是字高的一半(6/8/12/16)，
 * 每行占用 ceil(字宽/8) 个字节，行优先、字节内低位在左（和 LCD_ShowChar 的取模顺序一致） */
static uint8_t ui_ascii_glyph(uint8_t ch, uint16_t font_size, ui_glyph_t *glyph)
{
  uint8_t glyph_w = (uint8_t)(font_size >> 1);   /* 字宽 = 字高的一半 */

  glyph->bitmap        = NULL;
  glyph->advance_x     = glyph_w;
  glyph->bytes_per_row = (uint8_t)((glyph_w + 7) >> 3);   /* 每行几个字节 = 字宽除以 8 向上取整 */
  glyph->height        = (uint8_t)font_size;               /* ASCII 字高 = 档位(case 里只放行 font_size) */
  glyph->width         = glyph_w;                          /* 1bpp：位图宽 = 步进 */
  glyph->bearing_x     = 0;
  glyph->bearing_y     = 0;
  glyph->gray          = 0u;

  if (ch < ' ' || ch > '~')        /* 不可见控制字符，字库里没有 */
  {
    return 0;
  }

  switch (font_size)
  {
    case 12: glyph->bitmap = ascii_1206[ch - ' ']; break;
    case 16: glyph->bitmap = ascii_1608[ch - ' ']; break;
    case 24: glyph->bitmap = ascii_2412[ch - ' ']; break;
    case 32: glyph->bitmap = ascii_3216[ch - ' ']; break;
    default: return 0;             /* 字库没有这一档 */
  }
  return 1;
}

/* 取一个汉字(GBK 两个字节)的字形。
 * 汉字是方块字，字宽 = 字高 = font_size，每行 ceil(font_size/8) 个字节；
 * 字库是数组，按 Index[0]/Index[1] 两个字节顺序查，查不到就不画、只占位 */
static uint8_t ui_gb_glyph(const uint8_t *gbk, uint16_t font_size, ui_glyph_t *glyph)
{
  const uint8_t *font_base;
  uint16_t entry_size;
  uint16_t font_num;
  uint16_t idx;

  glyph->bitmap        = NULL;
  glyph->advance_x     = (uint8_t)font_size;
  glyph->bytes_per_row = (uint8_t)((font_size + 7) >> 3);
  glyph->height        = (uint8_t)font_size;   /* 方块字：字高 = 字宽 = font_size */
  glyph->width         = (uint8_t)font_size;
  glyph->bearing_x     = 0;
  glyph->bearing_y     = 0;
  glyph->gray          = 0u;

  switch (font_size)
  {
    case 12: font_base = (const uint8_t*)tfont12; entry_size = sizeof(typFNT_GB12); font_num = TFONT12_NUM; break;
    case 16: font_base = (const uint8_t*)tfont16; entry_size = sizeof(typFNT_GB16); font_num = TFONT16_NUM; break;
    case 24: font_base = (const uint8_t*)tfont24; entry_size = sizeof(typFNT_GB24); font_num = TFONT24_NUM; break;
    case 32: font_base = (const uint8_t*)tfont32; entry_size = sizeof(typFNT_GB32); font_num = TFONT32_NUM; break;
    default: return 0;             /* 字库没有这一档 */
  }

  /* 每个元素都是 Index[2] 打头、Msk 紧跟其后，都是 char、元素间没有填充，
   * 所以按固定步长扫一遍就行，四档字库不用各写一遍 */
  for (idx = 0; idx < font_num; idx++)
  {
    const uint8_t *entry = font_base + (uint32_t)idx * entry_size;

    if (entry[0] == gbk[0] && entry[1] == gbk[1])
    {
      glyph->bitmap = entry + 2;
      return 1;
    }
  }
  return 0;
}

/* 把一段亮点填进缓冲：先夹到文本可写范围 [clip_l, clip_r]，再交给 ui_span_alpha 夹到脏条。
 * 可写范围把文字真正限制在文本框内 —— 超出框的笔画在这里被裁掉 */
static void glyph_fill(int16_t from_x, int16_t to_x, int16_t clip_l, int16_t clip_r,
                       ui_color_t color, uint8_t alpha)
{
  if (from_x < clip_l) { from_x = clip_l; }
  if (to_x > clip_r)   { to_x = clip_r; }
  if (from_x > to_x)   { return; }
  ui_span_alpha(from_x, to_x, color, alpha);
}

/* 把字形的第 glyph_y 行铺进行缓冲。
 * 行内从左往右逐位看，连续点亮的一串攒成一段、一段一次性填掉，少调几次涂色；
 * 每段还要夹在可写范围 [clip_l, clip_r] 内 */
static void ui_glyph_row(const ui_glyph_t *glyph, uint8_t glyph_y, int16_t x, ui_color_t color,
                         int16_t clip_l, int16_t clip_r, uint8_t alpha)
{
  const uint8_t *bitmap_row = glyph->bitmap + (uint16_t)glyph_y * glyph->bytes_per_row;
  int16_t run_start = -1;         /* 当前这串连续亮点的起点列，-1 表示还没进串 */

  for (uint8_t col = 0; col < glyph->advance_x; col++)
  {
    if (bitmap_row[col >> 3] & (0x01 << (col & 0x07)))
    {
      if (run_start < 0)
      {
        run_start = col;
      }
    }
    else if (run_start >= 0)
    {
      glyph_fill(x + run_start, x + col - 1, clip_l, clip_r, color, alpha);
      run_start = -1;
    }
  }

  if (run_start >= 0)
  {
    glyph_fill(x + run_start, x + glyph->advance_x - 1, clip_l, clip_r, color, alpha);
  }
}

/* 灰度字形的第 gy 行：逐像素和行缓冲里已有的底色做 alpha 混合。
 * rowbuf 是 ui_row_draw_select() 返回的本行缓冲首地址，下标 = x - ui_row_draw_left()。
 * 和 1bpp 那条路(glyph_fill -> ui_span_alpha)一样，这里也要认【视口裁剪】：
 * 可写范围只夹文本框自己的边，容器(视窗)的裁剪矩形是另一层 —— 不夹的话
 * 灰度的字会画到视窗外面去(通气柱状图的序号 / 低良高就是从标注视窗里漏出来的)。
 * alpha 是文本框整体的半透明(0 = 不透明)：和字模自带的 a 相乘，边缘过渡不会丢 */
static void ui_gray_glyph_row(const ui_glyph_t *glyph, uint8_t gy, int16_t x,
                              ui_color_t *rowbuf, ui_color_t color,
                              int16_t clip_l, int16_t clip_r, uint8_t alpha)
{
  const uint8_t *src = glyph->bitmap + (uint16_t)gy * glyph->bytes_per_row;
  int16_t left  = ui_row_draw_left();       /* 缓冲下标基准：仍是脏条左端 */
  int16_t i;

  if (ui_row_draw_row_visible() == 0u)      /* 这一屏幕行不在裁剪矩形里 */
  {
    return;
  }
  if (clip_l < ui_row_clip_left())  { clip_l = ui_row_clip_left(); }
  if (clip_r > ui_row_clip_right()) { clip_r = ui_row_clip_right(); }

  for (i = 0; i < (int16_t)glyph->width; i++)
  {
    int16_t px = (int16_t)(x + i);
    uint8_t a;

    if (px < clip_l || px > clip_r)
    {
      continue;
    }
    a = src[i];
    if (a == 0u)
    {
      continue;
    }
    if (alpha != 0u && alpha < 255u)        /* 文本框半透明：和字模 alpha 相乘 */
    {
      a = (uint8_t)(((uint16_t)a * alpha) / 255u);
      if (a == 0u)
      {
        continue;
      }
    }
    if (a == 255u)
    {
      rowbuf[px - left] = color;
    }
    else
    {
      rowbuf[px - left] = ui_color_blend(rowbuf[px - left], color, a);
    }
  }
}

/* 量整串文字排出来的宽度估算：ASCII 前进 font_size/2，汉字前进 font_size。
 * 只是估算 —— 灰度 ASCII 字库(美好体)是比例字体，ui_draw_text 绘制时按字库给的
 * advance_x 推进笔位；这里算出来的宽度用来定水平居中起点 / 折行阈值(差几个像素)。
 * '\n' 是显式换行、只断行不占宽，也不计入；尾巴上剩半个汉字（凑不出字形）不计 */
int16_t ui_text_advance(const uint8_t *str, uint16_t len, uint16_t font_size)
{
  int16_t w = 0;
  uint16_t i = 0;

  while (i < len)
  {
    if (str[i] == '\n')            /* 显式换行：只负责断行、不占宽度 —— 和 ui_draw_text 一个口径 */
    {
      i += 1;
    }
    else if (str[i] < 0x80)
    {
      w += (int16_t)(font_size >> 1);
      i += 1;
    }
    else
    {
      if ((uint16_t)(i + 1) >= len)
      {
        break;
      }
      w += (int16_t)font_size;
      i += 2;
    }
  }
  return w;
}

/* 文本折行表：一次绘制最多排 UI_TEXT_MAX_LINES 行（数值在 ui_config.h），超出的截断。
 * 每行记「起始字节下标 / 字节数 / 行宽」，绘制时按屏幕行反查属于哪一行 */
typedef struct
{
  uint16_t start;
  uint16_t len;
  int16_t  adv;
}ui_text_line_t;

/* 文字：按行取模。每个字符在这一行只有一行点阵，把它铺进行缓冲对应的横向位置。
 * 底色已经由 ui_row_buf_clear 铺好，这里只对置位点写前景色（叠加模式），
 * 所以字库里没有的字、字形里的空白，留着底色就行。
 * 排版：先按可写范围宽度(para->w，即行宽)贪心折行，'\n' 强制换行；
 * 行高 = font_size + line_spacing。整块按 valign 纵向对齐（顶/中/底），行内按 align 左 / 中 / 右对齐。
 * 文字被限制在可写范围内：横向超出右边界的不画、纵向越界的整行不画
 * （= 文本框放不下就截断），超过 UI_TEXT_MAX_LINES 的行直接丢弃。
 * 逐段再由 ui_glyph_row 夹到范围左右边界。
 *
 * 取模顺序是"每行 -> 每字 -> 该字覆盖的屏幕行"：一个字的点阵每片只取一次，
 * 取一次就把它的 height 行分别铺进脏条里对应的那几行。
 * （反过来"每屏幕行 -> 每个字都重取一次"看着一样，但汉字走片外字库时
 *   每次取模都是一次 NAND 读，脏条高 25 行就是一个字读 25 次。）
 * 一条脏条是矩形，左右边界整条一致，所以 left/right 在外面取一次。 */
void ui_draw_text(void* p)
{
  ui_text_para_t* para = p;
  if(para->head.type != UI_TYPE_TEXT)
  {
    return;
  }
  const uint8_t *str;
  int16_t box_left;
  int16_t box_right;
  int16_t box_top;
  int16_t box_bottom;

  /* 折行结果与整块文字几何 */
  ui_text_line_t lines[UI_TEXT_MAX_LINES];
  uint8_t  n_lines;
  uint16_t scan;
  int16_t  line_h;
  int16_t  block_h;
  int16_t  block_top;
  static uint8_t gbuf[UI_GLYPH_MAX_BYTES];   /* 取模落地缓冲(灰度 48×48 = 2304B)：
                                                * 放静态区，别占 2KB 的系统栈 */

  if (para == NULL || para->text == NULL || para->text_len == 0)
  {
    return;
  }

  /* 字号合法性：
   *   用内部字库(hz_glyph_fn == NULL)只有 12/16/24/32 四档，别的取不到点阵；
   *   用了取模回调的字库档位由那份字库自己定，这里不预设 —— 取不到就只占位 */
  if (para->hz_glyph_fn == NULL &&
      para->font_size != 12 && para->font_size != 16 &&
      para->font_size != 24 && para->font_size != 32)
  {
    return;
  }

  /* 可写范围（中心 + 宽高，边界约定跟矩形一致：x_max = x_min + w - 1） */
  box_left   = (int16_t)(para->head.center_x - (para->w >> 1));
  box_right  = (int16_t)(box_left + para->w - 1);
  box_top    = (int16_t)(para->head.center_y - (para->h >> 1));
  box_bottom = (int16_t)(box_top + para->h - 1);

  /* ---- 折行：行宽 = 可写范围宽 para->w；'\n' 强制换行，超宽自动断行 ----
   * 行数超过 UI_TEXT_MAX_LINES 的部分直接丢弃（文本框放不下就截断） */
  line_h  = (int16_t)(para->font_size + para->line_spacing);
  n_lines = 0;
  scan    = 0;
  while (scan < para->text_len && n_lines < UI_TEXT_MAX_LINES)
  {
    uint16_t line_start = scan;
    uint16_t line_end;
    int16_t  line_adv = 0;

    while (scan < para->text_len)
    {
      uint8_t step;
      int16_t cw;

      if (para->text[scan] == '\n')      /* 显式换行 */
      {
        scan++;
        break;
      }
      if (para->text[scan] < 0x80)
      {
        cw   = (int16_t)(para->font_size >> 1);
        step = 1;
      }
      else
      {
        if ((uint16_t)(scan + 1) >= para->text_len)
        {
          scan = para->text_len;         /* 尾巴上剩半个汉字，凑不出字形 */
          break;
        }
        cw   = (int16_t)para->font_size;
        step = 2;
      }
      if (line_adv + cw > (int16_t)para->w && line_adv > 0)
      {
        break;                           /* 这一行放不下，另起一行 */
      }
      line_adv += cw;
      scan     += step;
    }

    line_end = scan;
    if (line_end > line_start && para->text[line_end - 1] == '\n')
    {
      line_end--;                        /* 行内容不含 '\n' 本身 */
    }

    lines[n_lines].start = line_start;
    lines[n_lines].len   = (uint16_t)(line_end - line_start);
    lines[n_lines].adv   = line_adv;
    n_lines++;
  }

  if (n_lines == 0)
  {
    return;                              /* 没有可画的行 */
  }

  block_h = (int16_t)((int16_t)n_lines * line_h);

  /* 整块纵向对齐：顶 / 中 / 底（居中时结果和旧的"center_y 居中"完全一致） */
  if (para->valign == UI_TEXT_VALIGN_TOP)
  {
    block_top = box_top;
  }
  else if (para->valign == UI_TEXT_VALIGN_BOTTOM)
  {
    block_top = (int16_t)(box_bottom + 1 - block_h);
  }
  else
  {
    block_top = (int16_t)(para->head.center_y - (block_h >> 1));
  }

  /* 一条脏条是个矩形：左右边界整条一致，取一次就够 */
  int16_t  left  = ui_row_draw_left();
  int16_t  right = ui_row_draw_right();
  uint16_t rows  = ui_row_draw_rows();

  /* 每片都要先把"纵向到底画不画得到"判掉再决定要不要取模：
   * 绘制链是【每一片都把全部对象走一遍】(见 ui_draw_dispatch 的调用方)，没有按框剔除，
   * 所以图表/码表那些跟文字完全不搭边的脏条也会进到这里。片外字库取一次模就是一次
   * NAND 读(整页)，白取一趟的代价是实打实的 —— 原来按"每屏幕行"扫时是靠 t 越界 continue
   * 挡掉的，现在换成按字形纵向范围挡，粒度还更细。
   * 这一片真正可能画到的屏幕行 = 脏条行范围 ∩ 可写范围 */
  int16_t strip_y0 = ui_row_draw_screen_y(0);
  int16_t strip_y1 = ui_row_draw_screen_y((uint16_t)(rows - 1u));
  int16_t seg_y0   = (strip_y0 > box_top)    ? strip_y0   : box_top;
  int16_t seg_y1   = (strip_y1 < box_bottom) ? strip_y1   : box_bottom;
  int16_t glyph_h  = (int16_t)para->font_size;   /* 字形纵向高度 = 点阵高 */

  /* 整块文字纵向都不在这一片里：直接走人，一个字都不用取模 */
  if (rows == 0u || seg_y0 > seg_y1)
  {
    return;
  }

  /* 那一段折算成行下标：后面铺字只用扫这一小段(最多一个字高) */
  uint16_t row_lo = (uint16_t)(seg_y0 - strip_y0);
  uint16_t row_hi = (uint16_t)(seg_y1 - strip_y0);

  /* 每行 -> 每字 -> 该字覆盖的屏幕行：
   * 一个字的点阵只取一次，再把它的每一行铺到脏条里对应的屏幕行上 */
  for (uint8_t li = 0; li < n_lines; li++)
  {
    const ui_text_line_t *ln = &lines[li];
    int16_t line_left;
    int16_t line_top;
    uint16_t bytes_left;
    int16_t  pen_x;

    /* 行内水平对齐 */
    if (para->align == UI_TEXT_ALIGN_LEFT)
    {
      line_left = box_left;
    }
    else if (para->align == UI_TEXT_ALIGN_RIGHT)
    {
      line_left = (int16_t)(box_right + 1 - ln->adv);
    }
    else
    {
      line_left = (int16_t)(para->head.center_x - (ln->adv >> 1));
    }

    line_top = (int16_t)(block_top + (int16_t)li * line_h);   /* 该行字形第 0 行落在哪个屏幕行 */

    /* 这一行的字形纵向范围是 [line_top, line_top + glyph_h - 1]，
     * 跟这一片交不上就是整行都画不到 —— 别去取模 */
    if (line_top > seg_y1 || (int16_t)(line_top + glyph_h - 1) < seg_y0)
    {
      continue;
    }

    str        = para->text + ln->start;
    bytes_left = ln->len;
    pen_x      = line_left;

    /* 往右排到脏条右边界或可写范围右边界为止 */
    while (bytes_left > 0 && pen_x <= right && pen_x <= box_right)
    {
      ui_glyph_t glyph;
      uint8_t bytes = 1;          /* 这个字占几个字节 */
      uint8_t got   = 0;          /* 取模回调这次取到没有 */
      int16_t glyph_x;
      int16_t glyph_top;

      if (str[0] < 0x80)          /* ASCII：一个字节 */
      {
        /* 有回调就先交给它 —— ASCII 也能配成从外部字库取(回调认就它管)；
         * 回调说取不到(或压根没回调)再回退到内部 Flash 那套小数组 */
        if (para->hz_glyph_fn != NULL)
        {
          got = para->hz_glyph_fn(para->hz_font_id, str, para->font_size, gbuf, &glyph);
        }
        if (got == 0)
        {
          ui_ascii_glyph(str[0], para->font_size, &glyph);
        }
      }
      else                        /* 汉字：GBK 两个字节 */
      {
        if (bytes_left < 2)
        {
          break;                  /* 尾巴上剩半个汉字，凑不出字形 */
        }
        bytes = 2;

        if (para->hz_glyph_fn != NULL)
        {
          /* 走取模回调(如片外字库)：字模被读进 gbuf，glyph.bitmap 指向 gbuf */
          para->hz_glyph_fn(para->hz_font_id, str, para->font_size, gbuf, &glyph);
        }
        else
        {
          ui_gb_glyph(str, para->font_size, &glyph);
        }
      }

      /* 笔位 + 字形自身的左右/上下偏移(1bpp 的两个 bearing 恒为 0) */
      glyph_x   = (int16_t)(pen_x + glyph.bearing_x);
      glyph_top = (int16_t)(line_top + glyph.bearing_y);

      /* 整个字都落在当前这条脏条左边界的左边就跳过，接着往右挪笔；
       * 右边界由 while 条件收尾；可写范围的裁剪交给行绘制函数逐段夹 */
      if (glyph.bitmap != NULL && glyph_x + (int16_t)glyph.advance_x - 1 >= left)
      {
        for (uint16_t row = row_lo; row <= row_hi; row++)
        {
          int16_t   glyph_y = (int16_t)(ui_row_draw_screen_y(row) - glyph_top);
          ui_color_t *rowbuf;

          if (glyph_y < 0 || glyph_y >= (int16_t)glyph.height)
          {
            continue;                          /* 不在这个字的纵向范围内(含行距留白) */
          }

          /* 把绘制游标指到这一行，再把这个字的第 glyph_y 行铺进去 */
          rowbuf = ui_row_draw_select(row);
          if (glyph.gray != 0u)
          {
            /* 灰度字形：逐像素和行缓冲里的底色做 alpha 混合 */
            ui_gray_glyph_row(&glyph, (uint8_t)glyph_y, glyph_x, rowbuf,
                              para->color, box_left, box_right, para->alpha);
          }
          else
          {
            ui_glyph_row(&glyph, (uint8_t)glyph_y, glyph_x, para->color,
                         box_left, box_right, para->alpha);
          }
        }
      }

      pen_x += glyph.advance_x;
      str   += bytes;
      bytes_left -= bytes;
    }
  }
}

/* 把一行上"某段线段命中"的那些 x 填成 color。
 * 判据是"这个像素到线段的距离 <= 半线宽"，全部用平方比、不开方：
 *   投影落在线段内部     -> 比 4*垂直距离? <= 线宽? * 段长?（两边同乘段长?，把除法消掉）
 *   投影落在段外（端帽） -> 比 4*(点到该端点距离?) <= 线宽?
 * 中间量尽量压到 int32：坐标最大约 300，点积、叉积本身都在 int32 内；
 * 只有"4*叉积?"能到 ~1e11，那一步才用 int64，且右边那项每段只算一次。
 * 段长只有几个像素时退化成一个小圆点 */
static void ui_poly_seg_row(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                            int16_t y, int16_t r, int64_t lw2, ui_color_t color, uint8_t alpha)
{
  int16_t dx   = (int16_t)(x1 - x0);
  int16_t dy   = (int16_t)(y1 - y0);
  int32_t len2 = (int32_t)dx * dx + (int32_t)dy * dy;
  int64_t lw2len2 = lw2 * (int64_t)len2;                /* 垂直判定右边那项，每段只算一次 */
  int16_t xlo;
  int16_t xhi;
  int16_t run  = -1;                                    /* 当前连续命中段的起点列，-1 表示还没进段 */
  int16_t x;

  /* 本行真正可能命中的横向范围：
   * 斜线在每一行上只占很窄的一小段，只扫那一段 —— 别每行都扫整个线段包围盒。
   * 行 y 上中线所在列 = x0 + (yc - y0) * dx / dy（yc 夹到线段 y 范围内，端帽那几行也覆盖）；
   * 半线宽投影到水平方向 = r * 段长 / |dy|，用 r *(|dx|+|dy|)/|dy| 当上界；再各留 1px 余量。
   * 水平段（dy == 0）退化成扫整个包围盒 */
  if (dy != 0)
  {
    int16_t ya = (int16_t)((y0 < y1) ? y0 : y1);
    int16_t yb = (int16_t)((y0 < y1) ? y1 : y0);
    int16_t yc = (int16_t)((y < ya) ? ya : ((y > yb) ? yb : y));
    int16_t xc = (int16_t)((int32_t)x0 + ((int32_t)(yc - y0) * dx) / dy);
    int32_t adx = (dx > 0) ? dx : -dx;
    int32_t ady = (dy > 0) ? dy : -dy;
    int32_t halfw = ((int32_t)r * (adx + ady)) / ady + 1;
    int16_t bl = (int16_t)((x0 < x1 ? x0 : x1) - r);    /* 端帽不会超出线段包围盒 */
    int16_t bh = (int16_t)((x0 > x1 ? x0 : x1) + r);

    xlo = (int16_t)(xc - halfw);
    xhi = (int16_t)(xc + halfw);
    if (xlo < bl) { xlo = bl; }
    if (xhi > bh) { xhi = bh; }
  }
  else
  {
    xlo = (int16_t)((x0 < x1 ? x0 : x1) - r);
    xhi = (int16_t)((x0 > x1 ? x0 : x1) + r);
  }

  /* 再夹到当前这条脏条覆盖的横向范围 */
  if (xlo < ui_row_draw_left())  { xlo = ui_row_draw_left(); }
  if (xhi > ui_row_draw_right()) { xhi = ui_row_draw_right(); }
  if (xlo > xhi)
  {
    return;
  }

  for (x = xlo; x <= xhi; x++)
  {
    int32_t px  = (int32_t)x - x0;
    int32_t py  = (int32_t)y - y0;
    int32_t dot = px * dx + py * dy;
    uint8_t hit;

    if (len2 <= 0 || dot <= 0)          /* 退化点 / 落在起点端帽之外：比到起点的距离 */
    {
      hit = (4 * (px * px + py * py) <= (int32_t)lw2) ? 1u : 0u;
    }
    else if (dot >= len2)               /* 落在终点端帽之外：比到终点的距离 */
    {
      int32_t qx = (int32_t)x - x1;
      int32_t qy = (int32_t)y - y1;
      hit = (4 * (qx * qx + qy * qy) <= (int32_t)lw2) ? 1u : 0u;
    }
    else                                /* 投影在段内：比垂直距离 */
    {
      int32_t cross = px * dy - py * dx;   /* |叉积| <= 300*300*2，int32 装得下 */
      hit = ((int64_t)4 * cross * cross <= lw2len2) ? 1u : 0u;
    }

    if (hit != 0u)
    {
      if (run < 0) { run = x; }
    }
    else if (run >= 0)                  /* 一段连续命中到这结束，整段一次填掉 */
    {
      ui_span_alpha(run, (int16_t)(x - 1), color, alpha);
      run = -1;
    }
  }

  if (run >= 0)                         /* 扫到右端还在命中段里，收尾 */
  {
    ui_span_alpha(run, xhi, color, alpha);
  }
}

/* 把"相对锚点的偏移"按当前缩放因子换算成屏幕坐标：
 *   虚拟 = 锚点 + 偏移 * num / den
 * 锚点不参与缩放（缩放前后它自己不动）；den 为 0（还没初始化）时按不缩放处理 */
int16_t ui_poly_virtual(int16_t off, int16_t anchor, uint16_t num, uint16_t den)
{
  if (den == 0u)
  {
    return (int16_t)(anchor + off);
  }
  return (int16_t)((int32_t)anchor + (int32_t)off * (int32_t)num / (int32_t)den);
}

/* 由对象框 中心 + 宽高 反推四边（跟矩形同约定）：
 *   x_min = center_x - w/2;  x_max = x_min + w - 1
 * 绘制裁剪 / 标脏区都走它，保证几处对同一块面积的理解一致 */
void ui_poly_rect_of(const ui_polyline_para_t* para,
                     int16_t* x_min, int16_t* y_min, int16_t* x_max, int16_t* y_max)
{
  ui_box_of(para->head.center_x, para->head.center_y, para->w, para->h,
            x_min, y_min, x_max, y_max);
}

/* 折线：起始点 → 各顶点依次相连，整条当一个对象画。
 * 每段按"点到线段的距离落在半线宽以内"判定，所以拐角自然带圆帽、接缝不会缺口。
 * 线宽 1 时只点亮中轴那一像素，加粗是往外长、不往内收 */
void ui_draw_polyline(void* p)
{
  ui_polyline_para_t* para = p;
  int16_t r, ax, ay, x_min, y_min, x_max, y_max, px, py;
  int64_t lw2;
  ui_poly_node_t* node;

  if (para->head.type != UI_TYPE_POLYLINE)
  {
    return;
  }
  if (ListEmpty(&para->list))
  {
    return;                        /* 只有起始点，连不出线段 */
  }

  r   = (int16_t)((para->line_width + 1) >> 1);          /* 半线宽，向上取整 */
  lw2 = (int64_t)para->line_width * para->line_width;

  /* 对象框由 中心 + 宽高 反推（存的就是当前比例下的外接框） */
  ui_poly_rect_of(para, &x_min, &y_min, &x_max, &y_max);

  /* 整条折线的对象框跟当前这条脏条横向都不相交，整段都不用画 */
  if (x_max < ui_row_draw_left() || x_min > ui_row_draw_right())
  {
    return;
  }

  ax = para->anchor_x;                                   /* 缩放锚点：偏移绕它缩放，它自己不动 */
  ay = para->anchor_y;

  for (uint16_t row = 0; row < ui_row_draw_rows(); row++)
  {
    int16_t y = ui_row_draw_screen_y(row);

    /* 不在对象框的上下范围内，这行没它的事 */
    if (y < y_min || y > y_max)
    {
      continue;
    }

    /* 把绘制游标指到这一行 */
    ui_row_draw_select(row);

    /* 从起始点出发，沿顶点链一段一段画；
     * 每个顶点都按"锚点 + 偏移 × 因子"现算屏幕坐标，上一段的终点就是这一段的起点 */
    px = ui_poly_virtual(para->start.x, ax, para->scale_num, para->scale_den);
    py = ui_poly_virtual(para->start.y, ay, para->scale_num, para->scale_den);
    ListForEachEntry(node, &para->list, ui_poly_node_t, node)
    {
      int16_t nx = ui_poly_virtual(node->point.x, ax, para->scale_num, para->scale_den);
      int16_t ny = ui_poly_virtual(node->point.y, ay, para->scale_num, para->scale_den);

      ui_poly_seg_row(px, py, nx, ny, y, r, lw2, para->color, para->alpha);
      px = nx;
      py = ny;
    }
  }
}

/* 外接框查询 / 光标目标框 / 光标定框都搬到 ui_control 了（见 ui_control.h） */

/* 光标绘制回调：只照光标自己的 center / w / h 画一个方框，不看 obj。
 * 想跟着图案走就先 ui_cursor_fit；想自己定位置和宽高就直接填那几个字段 */
void ui_draw_cursor(void* p)
{
  ui_cursor_para_t* cursor_para = p;
  ui_rect_para_t cursor_rect;

  if (cursor_para->head.type != UI_TYPE_CURSOR)
  {
    return;
  }

  /* 方框几何直接用光标自己的；界面上就是一个普通矩形，借 ui_draw_rect 画出来 */
  cursor_rect.head.type = UI_TYPE_RECT;
  cursor_rect.head.center_x = cursor_para->head.center_x;
  cursor_rect.head.center_y = cursor_para->head.center_y;
  cursor_rect.w = cursor_para->w;
  cursor_rect.h = cursor_para->h;
  cursor_rect.border_width = cursor_para->border_width;
  cursor_rect.color = cursor_para->color;
  cursor_rect.fill = 0;        /* 光标只描边，不填充：必须显式清零，否则 ui_draw_rect 读到栈上垃圾值 */
  cursor_rect.corners = 0u;    /* 光标是直角方框（同样必须显式清零） */
  cursor_rect.radius = 0u;
  cursor_rect.alpha = cursor_para->alpha;

  ui_draw_rect(&cursor_rect);
}


/* ---- 合并对象（union）绘制 ----
 * 外接框查询 / 框重算 / append 等操作都搬到 ui_control 了（见 ui_control.h） */

/* 合并对象绘制：遍历子对象链，逐个派发（子对象自带绘制函数） */
void ui_draw_union(void* p)
{
  ui_union_para_t* para = p;
  ui_merge_obj_node_t* node;

  if (para == NULL || para->head.type != UI_TYPE_UNION)
  {
    return;
  }

  ListForEachEntry(node, &para->list, ui_merge_obj_node_t, node)
  {
    if (node->obj != NULL && node->obj->para != NULL)
    {
      ui_draw_dispatch(node->obj->para);
    }
  }
}

/* 视窗绘制：自己不画像素，只把窗口矩形压成裁剪区，再带着裁剪把子对象逐个派发出去，
 * 最后还原上一层裁剪（用 push / pop，多个视窗 / 容器嵌套时裁剪自动取交集）。
 * 边界约定跟矩形一致：x_min = 中心 - w/2、x_max = x_min + w - 1 */
void ui_draw_viewport(void* p)
{
  ui_viewport_para_t* para = p;
  ui_merge_obj_node_t* node;
  int16_t x_min, y_min, x_max, y_max;

  if (para == NULL || para->head.type != UI_TYPE_VIEWPORT)
  {
    return;
  }

  x_min = (int16_t)(para->head.center_x - (para->w >> 1));
  y_min = (int16_t)(para->head.center_y - (para->h >> 1));
  x_max = (int16_t)(x_min + para->w - 1);
  y_max = (int16_t)(y_min + para->h - 1);

  ui_row_clip_push(x_min, y_min, x_max, y_max);

  ListForEachEntry(node, &para->list, ui_merge_obj_node_t, node)
  {
    if (node->obj != NULL && node->obj->para != NULL)
    {
      ui_draw_dispatch(node->obj->para);
    }
  }

  ui_row_clip_pop();
}


/* ============================================================================
 * 进度条绘制（圆环 / 矩形）
 * 角度约定：0 = 右侧(+x)，顺时针为正（屏幕 y 向下）：90 = 下、180 = 左、270 = 上。
 * ========================================================================== */

/* sin(deg) × 1024 的定点值，1° 分辨率：0°~90° 查表，其余象限靠符号 / 余角折算。
 * 画圆弧只要方向、不要精度，整数表就够，省掉浮点三角函数 */
static const int16_t ui_sin_tab1024[91] =
{
      0,   18,   36,   54,   71,   89,  107,  125,  143,  160,
    178,  195,  213,  230,  248,  265,  282,  299,  316,  333,
    350,  367,  384,  400,  416,  433,  449,  465,  481,  496,
    512,  527,  543,  558,  573,  587,  602,  616,  630,  644,
    658,  672,  685,  698,  711,  724,  737,  749,  761,  773,
    784,  796,  807,  818,  828,  839,  849,  859,  868,  878,
    887,  896,  904,  912,  920,  928,  935,  943,  949,  956,
    962,  968,  974,  979,  984,  989,  994,  998, 1002, 1005,
   1008, 1011, 1014, 1016, 1018, 1020, 1022, 1023, 1023, 1024,
   1024
};

static int32_t ui_sin_deg1024(int32_t deg)
{
  int32_t d = deg % 360;
  int32_t sgn = 1;

  if (d < 0)   { d += 360; }
  if (d > 180) { d -= 180; sgn = -1; }   /* sin(180+x) = -sin(x) */
  if (d > 90)  { d = 180 - d; }          /* sin(180-x) = sin(x)，折回 [0,90] */
  return sgn * (int32_t)ui_sin_tab1024[d];
}

static int32_t ui_cos_deg1024(int32_t deg)
{
  return ui_sin_deg1024(deg + 90);
}

/* 方向 (dx,dy)（相对圆心）是否落在"从 a0 起、顺时针扫过 sweep 度"的扇区内。
 * 用两条边界射线和查询方向作叉积定正负 —— 全是整数乘加，不用三角函数。
 * sweep 在 [0,360]：<=0 恒不在，>=360 恒在 */
static uint8_t ui_deg_in_sector(int32_t dx, int32_t dy, int32_t a0, int32_t sweep)
{
  int32_t ux, uy, vx, vy;

  if (sweep <= 0)   { return 0; }
  if (sweep >= 360) { return 1; }

  ux = ui_cos_deg1024(a0);           uy = ui_sin_deg1024(a0);          /* 起始边界方向 */
  vx = ui_cos_deg1024(a0 + sweep);   vy = ui_sin_deg1024(a0 + sweep);  /* 结束边界方向 */

  if (sweep <= 180)
  {
    /* 顺时针角 u->d 要落在 [0,sweep]：cross(u,d) >= 0 且 cross(d,v) >= 0 */
    if ((ux * dy - uy * dx) < 0) { return 0; }
    if ((dx * vy - dy * vx) < 0) { return 0; }
    return 1;
  }

  /* 大扇区（>180°）：改判"是否落进跨度 <180° 的补扇区（v->u）"，落进去就不是 */
  if ((vx * dy - vy * dx) >= 0 && (dx * uy - dy * ux) >= 0) { return 0; }
  return 1;
}

/* 圆环进度条：背景环 + 前景进度环共心叠画。
 * 背景环：从 angle_start 起顺时针铺满 angle_end_bg 度，用 color_bg；
 * 前景环：同样从 angle_start 起，只铺 angle_end_bg × progress / max_value 度，用 color_fore。
 * 环的粗细由 internal_r / external_r 决定（与 ui_draw_circle 同一套），line_width 不参与几何。
 * rounded_end 非 0 时，前景弧两个端点各补一个半圆帽；背景弧在 angle_end_bg < 360
 * （确有端点）时两端也各补一个半圆帽。帽半径 = 环半厚、帽心在环中径上，前景帽压在背景帽之上。
 * 每行只扫圆环自己的横向范围，攒成"同色连续段"交给 ui_row_draw_span 涂，走标准涂色路径 */
void ui_draw_circle_progress_bar(void* p)
{
  ui_circle_progress_bar_para_t* para = p;
  int32_t outer_r2, inner_r2, a0, bg_sweep, fg_sweep, den;
  ui_color_t color_bg, color_fore;
  int32_t cap_r = 0, cap_dx0 = 0, cap_dy0 = 0, cap_dx1 = 0, cap_dy1 = 0;
  int32_t bg_cap_dx0 = 0, bg_cap_dy0 = 0, bg_cap_dx1 = 0, bg_cap_dy1 = 0;
  uint8_t use_cap = 0u, use_bg_cap = 0u;

  if (para == NULL || para->head.type != UI_TYPE_PROGRESS_BAR)
  {
    return;
  }
  if (para->external_r <= 0)
  {
    return;
  }

  outer_r2 = (int32_t)para->external_r * para->external_r;
  inner_r2 = (int32_t)para->internal_r * para->internal_r;
  if (inner_r2 > outer_r2) { inner_r2 = outer_r2; }   /* 内半径顶多等于外半径，退化也安全 */

  a0       = para->angle_start;
  bg_sweep = para->angle_end_bg;
  den      = (para->max_value != 0u) ? (int32_t)para->max_value : 1;
  fg_sweep = bg_sweep * (int32_t)para->progress / den;

  color_bg   = para->color_bg;
  color_fore = para->color_fore;

  /* 圆角帽半径 = 环半厚（前景 / 背景共用）；<=0 时两端都补不了帽 */
  if (para->rounded_end != 0u)
  {
    cap_r = ((int32_t)para->external_r - para->internal_r) / 2;
  }

  /* 前景弧的圆角帽：帽心在环中径、起止角上 */
  if (cap_r > 0 && fg_sweep > 0)
  {
    int32_t mid_r = ((int32_t)para->internal_r + para->external_r) / 2;

    cap_dx0 = mid_r * ui_cos_deg1024(a0) / 1024;
    cap_dy0 = mid_r * ui_sin_deg1024(a0) / 1024;
    cap_dx1 = mid_r * ui_cos_deg1024(a0 + fg_sweep) / 1024;
    cap_dy1 = mid_r * ui_sin_deg1024(a0 + fg_sweep) / 1024;
    use_cap = 1u;
  }

  /* 背景弧的圆角帽：同前景一套；只在背景确有端点(bg_sweep < 360)时补 */
  if (cap_r > 0 && bg_sweep > 0 && bg_sweep < 360)
  {
    int32_t mid_r = ((int32_t)para->internal_r + para->external_r) / 2;

    bg_cap_dx0 = mid_r * ui_cos_deg1024(a0) / 1024;
    bg_cap_dy0 = mid_r * ui_sin_deg1024(a0) / 1024;
    bg_cap_dx1 = mid_r * ui_cos_deg1024(a0 + bg_sweep) / 1024;
    bg_cap_dy1 = mid_r * ui_sin_deg1024(a0 + bg_sweep) / 1024;
    use_bg_cap = 1u;
  }

  for (uint16_t row = 0; row < ui_row_draw_rows(); row++)
  {
    int16_t y   = ui_row_draw_screen_y(row);
    int32_t dy  = (int32_t)y - para->head.center_y;
    int32_t dy2 = dy * dy;
    int32_t rx;
    int16_t x_lo, x_hi;
    int16_t run_start = -1;                 /* 当前同色连续段的起点，-1 = 还没进段 */
    ui_color_t run_color = color_bg;

    if (dy2 > outer_r2)
    {
      continue;                     /* 整行都在圆外 */
    }

    /* 只扫圆环在这一行的横向范围 [cx ± sqrt(outer?-dy?)]，不扫整条脏条 */
    rx   = ui_isqrt32(outer_r2 - dy2);
    x_lo = (int16_t)(para->head.center_x - rx);
    x_hi = (int16_t)(para->head.center_x + rx);
    if (x_lo < ui_row_draw_left())  { x_lo = ui_row_draw_left(); }
    if (x_hi > ui_row_draw_right()) { x_hi = ui_row_draw_right(); }
    if (x_lo > x_hi)
    {
      continue;
    }

    ui_row_draw_select(row);

    for (int16_t x = x_lo; x <= x_hi; x++)
    {
      int32_t dx = (int32_t)x - para->head.center_x;
      int32_t d2 = dx * dx + dy2;
      uint8_t paint = 0u;
      ui_color_t px = color_bg;

      if (d2 <= outer_r2 && d2 >= inner_r2)
      {
        uint8_t in_fg  = ui_deg_in_sector(dx, dy, a0, fg_sweep);
        uint8_t in_cap = 0u;
        uint8_t in_bg  = 0u;

        /* 前景圆角帽要先于背景判：弧尾那半个帽落在背景扇区里，若先判背景就被
         * color_bg 吃掉，只剩起点那半个帽、弧尾变成平头。帽圆本就在环带内，只多判一次距离 */
        if (use_cap != 0u && in_fg == 0u)
        {
          int32_t e0x = dx - cap_dx0;
          int32_t e0y = dy - cap_dy0;
          int32_t e1x = dx - cap_dx1;
          int32_t e1y = dy - cap_dy1;

          if (e0x * e0x + e0y * e0y <= cap_r * cap_r ||
              e1x * e1x + e1y * e1y <= cap_r * cap_r)
          {
            in_cap = 1u;
          }
        }

        if (in_fg != 0u || in_cap != 0u)
        {
          px = color_fore;                    /* 前景(含两端圆角帽)压在背景之上 */
          paint = 1u;
        }
        else
        {
          in_bg = ui_deg_in_sector(dx, dy, a0, bg_sweep);

          /* 背景圆角帽：落在背景扇区外、帽圆内就补背景色（前景已在上一步排除） */
          if (in_bg == 0u && use_bg_cap != 0u)
          {
            int32_t b0x = dx - bg_cap_dx0;
            int32_t b0y = dy - bg_cap_dy0;
            int32_t b1x = dx - bg_cap_dx1;
            int32_t b1y = dy - bg_cap_dy1;

            if (b0x * b0x + b0y * b0y <= cap_r * cap_r ||
                b1x * b1x + b1y * b1y <= cap_r * cap_r)
            {
              in_bg = 1u;
            }
          }

          if (in_bg != 0u)
          {
            px = color_bg;
            paint = 1u;
          }
        }
      }

      /* 攒成"同色连续段"，一段一起用 ui_row_draw_span 涂（它自带脏条 / 裁剪裁剪）：
       * 走标准涂色路径，不再自己直写缓冲 */
      if (paint != 0u)
      {
        if (run_start < 0)
        {
          run_start = x;
          run_color = px;
        }
        else if (px != run_color)
        {
          ui_span_alpha(run_start, (int16_t)(x - 1), run_color, para->alpha);
          run_start = x;
          run_color = px;
        }
      }
      else if (run_start >= 0)
      {
        ui_span_alpha(run_start, (int16_t)(x - 1), run_color, para->alpha);
        run_start = -1;
      }
    }

    if (run_start >= 0)
    {
      ui_span_alpha(run_start, x_hi, run_color, para->alpha);
    }
  }
}

/* 矩形进度条：底条 + 前景进度条 + 外框留边。
 * 底条：整块矩形铺 color_bg；前景：在内区（扣掉 border_width 的留边）里，
 * 按 dir 从一端沿增长方向铺 fill_len = 内边长 × progress / max_value 长，用 color_fore。
 * 底条没被前景盖住的留边和空轨道，自然显现成外框 / 底条。
 * 走 ui_row_draw_span，横向自动裁到当前脏条 */
void ui_draw_rect_progress_bar(void* p)
{
  ui_rect_progress_bar_para_t* para = p;
  int16_t x_min, x_max, y_min, y_max, border;
  int16_t in_x0, in_x1, in_y0, in_y1;
  int16_t fx0 = 0, fx1 = -1, fy0 = 0, fy1 = -1;   /* 前景区，默认空 */
  int32_t den, span, fill_len;
  uint8_t has_fore = 0u;
  uint8_t round_end;
  int16_t r_bar = 0;                              /* 圆弧端半径 = 条厚的一半 */

  if (para == NULL || para->head.type != UI_TYPE_PROGRESS_BAR)
  {
    return;
  }
  if (para->w <= 0 || para->h <= 0)
  {
    return;
  }

  x_min = (int16_t)(para->head.center_x - (para->w >> 1));
  x_max = (int16_t)(x_min + para->w - 1);
  y_min = (int16_t)(para->head.center_y - (para->h >> 1));
  y_max = (int16_t)(y_min + para->h - 1);

  if (x_max < ui_row_draw_left() || x_min > ui_row_draw_right())
  {
    return;
  }

  border = para->border_width;
  if (border < 0) { border = 0; }

  in_x0 = (int16_t)(x_min + border);
  in_x1 = (int16_t)(x_max - border);
  in_y0 = (int16_t)(y_min + border);
  in_y1 = (int16_t)(y_max - border);

  den  = (para->max_value != 0u) ? (int32_t)para->max_value : 1;
  span = (int32_t)para->progress;
  if (span > den) { span = den; }

  /* 前景区：内区里沿 dir 从一端铺 fill_len 长 */
  if (in_x1 >= in_x0 && in_y1 >= in_y0 && span > 0)
  {
    switch (para->dir)
    {
      case UI_PROGRESS_DIR_UP:
      case UI_PROGRESS_DIR_DOWN:
      {
        fill_len = (int32_t)(in_y1 - in_y0 + 1) * span / den;
        if (fill_len > 0)
        {
          fx0 = in_x0;
          fx1 = in_x1;
          if (para->dir == UI_PROGRESS_DIR_UP)
          {
            fy1 = in_y1;
            fy0 = (int16_t)(in_y1 - fill_len + 1);
          }
          else
          {
            fy0 = in_y0;
            fy1 = (int16_t)(in_y0 + fill_len - 1);
          }
          has_fore = 1u;
        }
        break;
      }
      case UI_PROGRESS_DIR_LEFT:
      case UI_PROGRESS_DIR_RIGHT:
      default:
      {
        fill_len = (int32_t)(in_x1 - in_x0 + 1) * span / den;
        if (fill_len > 0)
        {
          fy0 = in_y0;
          fy1 = in_y1;
          if (para->dir == UI_PROGRESS_DIR_LEFT)
          {
            fx1 = in_x1;
            fx0 = (int16_t)(in_x1 - fill_len + 1);
          }
          else
          {
            fx0 = in_x0;
            fx1 = (int16_t)(in_x0 + fill_len - 1);
          }
          has_fore = 1u;
        }
        break;
      }
    }
  }

  /* 两端为圆弧：半径取短边一半（横向条 = 高的一半，纵向条 = 宽的一半），两端各成半圆。
   * 逐行范围交给 ui_tool 的圆角行函数算，不在这里重造圆角几何 */
  round_end = para->rounded_end;
  if (round_end != 0u)
  {
    r_bar = (int16_t)((para->w < para->h ? para->w : para->h) >> 1);
  }

  for (uint16_t row = 0; row < ui_row_draw_rows(); row++)
  {
    int16_t y = ui_row_draw_screen_y(row);
    int16_t bx0, bx1;               /* 本行底条该涂的范围 */

    if (y < y_min || y > y_max)
    {
      continue;
    }

    if (round_end != 0u)
    {
      if (ui_tool_round_rect_row_ex(para->head.center_x, para->head.center_y,
                                    para->w, para->h, r_bar, y,
                                    UI_ROUND_ALL, &bx0, &bx1) == 0u)
      {
        continue;
      }
    }
    else
    {
      bx0 = x_min;
      bx1 = x_max;
    }

    if (bx1 < ui_row_draw_left() || bx0 > ui_row_draw_right())
    {
      continue;
    }

    ui_row_draw_select(row);

    /* 底条 */
    ui_span_alpha(bx0, bx1, para->color_bg, para->alpha);

    /* 前景：本行落进前景区才铺 */
    if (has_fore != 0u && y >= fy0 && y <= fy1)
    {
      if (round_end != 0u)
      {
        /* 前景也做成两端半圆：按前景自己那个框取行范围，再夹回底条范围 */
        int16_t fw  = (int16_t)(fx1 - fx0 + 1);
        int16_t fh  = (int16_t)(fy1 - fy0 + 1);
        int16_t fcx = (int16_t)(fx0 + (fw >> 1));
        int16_t fcy = (int16_t)(fy0 + (fh >> 1));
        int16_t rf  = (int16_t)((fw < fh ? fw : fh) >> 1);
        int16_t ox0, ox1;

        if (ui_tool_round_rect_row_ex(fcx, fcy, fw, fh, rf, y,
                                      UI_ROUND_ALL, &ox0, &ox1) != 0u)
        {
          if (ox0 < bx0) { ox0 = bx0; }
          if (ox1 > bx1) { ox1 = bx1; }
          ui_span_alpha(ox0, ox1, para->color_fore, para->alpha);
        }
      }
      else
      {
        ui_span_alpha(fx0, fx1, para->color_fore, para->alpha);
      }
    }
  }
}


/* ---- 进度值变化对应的脏区 --------------------------------------------------
 * 进度每动一点就把整个外框重画太浪费：其实只有"新旧进度之间那一段"变了。
 *   圆环：变的是 a_old 到 a_new 之间的环扇区；
 *   矩形：变的是前景末端从旧位置挪到新位置之间那条内区带。
 * 两端的圆角帽/圆弧都落在各自外接框内，再加一点余量兜住取整误差。
 * 只多推、绝不漏：值没变直接返回，几何退化时退回整框 */

/* 圆环：从 lo 角度扫到 hi 角度的外圆外接框。
 * 环扇区的极值点只可能落在外圆上(外圆半径最大)，所以按角度采样外圆取 min/max 即可 */
static void progress_circle_arc_box(const ui_circle_progress_bar_para_t* c,
                                    uint16_t old_value, uint16_t new_value,
                                    int16_t* x0, int16_t* y0, int16_t* x1, int16_t* y1)
{
  int32_t den   = (c->max_value != 0u) ? (int32_t)c->max_value : 1;
  int32_t a0    = c->angle_start;
  int32_t sweep = c->angle_end_bg;
  int32_t v_old = (int32_t)old_value;
  int32_t v_new = (int32_t)new_value;
  int32_t a_old, a_new, lo, hi, span, i, step, margin;
  int32_t minx, maxx, miny, maxy;

  if (v_old > den) { v_old = den; }                     /* 和绘制同一套口径：超量程按满 */
  if (v_new > den) { v_new = den; }

  a_old = a0 + sweep * v_old / den;
  a_new = a0 + sweep * v_new / den;

  if (a_new < a_old) { lo = a_new; hi = a_old; }
  else               { lo = a_old; hi = a_new; }

  margin = (int32_t)c->external_r - c->internal_r;      /* 余量取环厚度，盖住两端帽和取整误差 */
  if (margin < 1) { margin = 1; }

  minx = maxx = (int32_t)c->head.center_x + (int32_t)c->external_r * ui_cos_deg1024(lo) / 1024;
  miny = maxy = (int32_t)c->head.center_y + (int32_t)c->external_r * ui_sin_deg1024(lo) / 1024;

  span = hi - lo;
  step = 4;                                             /* 4 度一采样，够密 */
  for (i = step; i < span; i += step)
  {
    int32_t a  = lo + i;
    int32_t px = (int32_t)c->head.center_x + (int32_t)c->external_r * ui_cos_deg1024(a) / 1024;
    int32_t py = (int32_t)c->head.center_y + (int32_t)c->external_r * ui_sin_deg1024(a) / 1024;

    if (px < minx) { minx = px; }
    if (px > maxx) { maxx = px; }
    if (py < miny) { miny = py; }
    if (py > maxy) { maxy = py; }
  }

  {   /* 收尾：把终点也算上 */
    int32_t px = (int32_t)c->head.center_x + (int32_t)c->external_r * ui_cos_deg1024(hi) / 1024;
    int32_t py = (int32_t)c->head.center_y + (int32_t)c->external_r * ui_sin_deg1024(hi) / 1024;

    if (px < minx) { minx = px; }
    if (px > maxx) { maxx = px; }
    if (py < miny) { miny = py; }
    if (py > maxy) { maxy = py; }
  }

  *x0 = (int16_t)(minx - margin);
  *y0 = (int16_t)(miny - margin);
  *x1 = (int16_t)(maxx + margin);
  *y1 = (int16_t)(maxy + margin);
}

/* 矩形：前景末端只沿增长方向挪。脏区 = 内区(扣 border)里、两个末端之间的那条带，
 * 但两端是圆角帽时得把"帽的斜肩"也算进去(否则旧帽擦不掉 -> 鬼影锯齿)；
 * 前景比条厚还短时帽半径会随长度变，改用新旧前景框的并集 */
static void progress_rect_delta_box(const ui_rect_progress_bar_para_t* r,
                                    uint16_t old_value, uint16_t new_value,
                                    int16_t* x0, int16_t* y0, int16_t* x1, int16_t* y1)
{
  int32_t den    = (r->max_value != 0u) ? (int32_t)r->max_value : 1;
  int32_t v_old  = (int32_t)old_value;
  int32_t v_new  = (int32_t)new_value;
  int32_t border = r->border_width;
  int16_t x_min, x_max, y_min, y_max, in_x0, in_x1, in_y0, in_y1;
  int32_t len_h, len_v, thick, margin, fill_old, fill_new, e_old, e_new, lo, hi;
  uint8_t vertical;

  if (v_old > den) { v_old = den; }
  if (v_new > den) { v_new = den; }
  if (border < 0) { border = 0; }

  x_min = (int16_t)(r->head.center_x - (r->w >> 1));
  x_max = (int16_t)(x_min + r->w - 1);
  y_min = (int16_t)(r->head.center_y - (r->h >> 1));
  y_max = (int16_t)(y_min + r->h - 1);

  in_x0 = (int16_t)(x_min + border);
  in_x1 = (int16_t)(x_max - border);
  in_y0 = (int16_t)(y_min + border);
  in_y1 = (int16_t)(y_max - border);

  if (in_x1 < in_x0 || in_y1 < in_y0)      /* 内区退化成空：退回整框，保证不漏 */
  {
    *x0 = x_min; *y0 = y_min; *x1 = x_max; *y1 = y_max;
    return;
  }

  len_h    = (int32_t)(in_x1 - in_x0 + 1);
  len_v    = (int32_t)(in_y1 - in_y0 + 1);
  vertical = ((r->dir == UI_PROGRESS_DIR_UP) || (r->dir == UI_PROGRESS_DIR_DOWN)) ? 1u : 0u;

  /* 条厚 = 增长方向之外的横截面(内区)，帽半径最多是它的一半。
   * 帽是半圆：端点那几行的边缘要比中间行往里缩最多"帽半径"，
   * 脏区必须把这个斜肩算进去，否则旧的帽擦不掉、一步步攒成一串鬼影帽(锯齿)。
   * 再 +1 兜住取整 */
  thick  = vertical ? len_h : len_v;
  margin = thick / 2 + 1;

  fill_old = (vertical ? len_v : len_h) * v_old / den;
  fill_new = (vertical ? len_v : len_h) * v_new / den;

  /* 末端位置和绘制口径一致：RIGHT/DOWN 从内区起点长；LEFT/UP 从内区终点长 */
  if (vertical)
  {
    e_old = (r->dir == UI_PROGRESS_DIR_UP) ? (in_y1 - fill_old + 1) : (in_y0 + fill_old - 1);
    e_new = (r->dir == UI_PROGRESS_DIR_UP) ? (in_y1 - fill_new + 1) : (in_y0 + fill_new - 1);
  }
  else
  {
    e_old = (r->dir == UI_PROGRESS_DIR_LEFT) ? (in_x1 - fill_old + 1) : (in_x0 + fill_old - 1);
    e_new = (r->dir == UI_PROGRESS_DIR_LEFT) ? (in_x1 - fill_new + 1) : (in_x0 + fill_new - 1);
  }

  /* 两个末端之间那一段，再按帽的斜肩向两侧各放宽 margin */
  lo = ((e_old < e_new) ? e_old : e_new) - margin;
  hi = ((e_old > e_new) ? e_old : e_new) + margin;

  /* 前景长度比条厚还短时，帽半径 = min(长,厚)/2 会随长度变 —— 整块前景的形状都在动，
   * 这时从"固定起点那一侧"一路放宽到内区起点(即新旧前景框的并集)，最稳 */
  if (fill_old < thick || fill_new < thick)
  {
    if (r->dir == UI_PROGRESS_DIR_RIGHT || r->dir == UI_PROGRESS_DIR_DOWN)
    {
      lo = vertical ? in_y0 : in_x0;        /* 起点钉在内区起点 */
    }
    else
    {
      hi = vertical ? in_y1 : in_x1;        /* 起点钉在内区终点 */
    }
  }

  if (vertical)
  {
    if (lo < in_y0) { lo = in_y0; }
    if (hi > in_y1) { hi = in_y1; }

    *x0 = in_x0; *x1 = in_x1;
    *y0 = (int16_t)lo; *y1 = (int16_t)hi;
  }
  else
  {
    if (lo < in_x0) { lo = in_x0; }
    if (hi > in_x1) { hi = in_x1; }

    *y0 = in_y0; *y1 = in_y1;
    *x0 = (int16_t)lo; *x1 = (int16_t)hi;
  }
}

/* 对外：进度条从 old_value 变到 new_value，只标出真正变了的那个区域 */
void ui_progress_mark_value_change(const ui_progress_common_t* p,
                                   uint16_t old_value, uint16_t new_value)
{
  int16_t x0, y0, x1, y1;

  if (p == NULL || old_value == new_value)     /* 值没变：一个像素都没动 */
  {
    return;
  }

  if (p->shape == (uint8_t)UI_PROGRESS_SHAPE_CIRCLE)
  {
    progress_circle_arc_box((const ui_circle_progress_bar_para_t*)p,
                            old_value, new_value, &x0, &y0, &x1, &y1);
  }
  else
  {
    progress_rect_delta_box((const ui_rect_progress_bar_para_t*)p,
                            old_value, new_value, &x0, &y0, &x1, &y1);
  }

  ui_dirty_mark(x0, y0, x1, y1);
}


/* 统一派发：首成员是 ui_head_t 的任何 para 都走这里，
 * 按 head->type 认出来、再调当初挂在 head->draw 上的那个绘制函数。
 * 漏填 type 或 draw 就什么都不画，不会误当成某一种形状静默画出来 */
void ui_draw_dispatch(void* para)
{
  ui_head_t *head;

  if (para == NULL)
  {
    return;
  }

  head = (ui_head_t*)para;

  if (head->type == UI_TYPE_NONE || head->draw == NULL)
  {
    return;
  }

  head->draw(para);
}





