#include "./ui_tool/ui_tool.h"

/* ==================================================================
 * 圆角矩形
 *   框架按行画，圆角的全部工作就是"每行求出该涂的横向范围"。
 * ================================================================== */

/* 整数平方根(向下取整)：角点内缩量用它算 —— 避免引 math.h / 浮点。
 * 经典逐位开方，int 范围内几次循环就完事 */
static uint32_t round_isqrt(uint32_t v)
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

uint8_t ui_tool_round_rect_row_ex(int16_t cx, int16_t cy, int16_t w, int16_t h,
                                  int16_t radius, int16_t y, uint8_t corners,
                                  int16_t *x0, int16_t *x1)
{
  int16_t x_min, x_max, y_min, y_max, r;
  int16_t inset_l = 0, inset_r = 0;                 /* 左右内缩量分开算，才能只圆单个角 */

  if (w <= 0 || h <= 0 || x0 == 0 || x1 == 0)
  {
    return 0u;
  }

  x_min = (int16_t)(cx - (w >> 1));
  x_max = (int16_t)(x_min + w - 1);
  y_min = (int16_t)(cy - (h >> 1));
  y_max = (int16_t)(y_min + h - 1);

  if (y < y_min || y > y_max)                       /* 这一行在矩形外 */
  {
    return 0u;
  }

  r = radius;
  if (r > (int16_t)((w < h ? w : h) >> 1))          /* 半径不能超过半宽/半高 */
  {
    r = (int16_t)((w < h ? w : h) >> 1);
  }
  if (r < 0) { r = 0; }

  if (r > 0)
  {
    if (y < (int16_t)(y_min + r))                   /* 上端那几行：可按位圆左上 / 右上 */
    {
      int16_t dy = (int16_t)(y_min + r - y);
      int16_t d  = (int16_t)(r - round_isqrt((uint32_t)((int32_t)r * r - (int32_t)dy * dy)));

      if ((corners & UI_ROUND_TOP_LEFT) != 0u)  { inset_l = d; }
      if ((corners & UI_ROUND_TOP_RIGHT) != 0u) { inset_r = d; }
    }
    else if (y > (int16_t)(y_max - r))              /* 下端那几行：可按位圆左下 / 右下 */
    {
      int16_t dy = (int16_t)(y - (y_max - r));
      int16_t d  = (int16_t)(r - round_isqrt((uint32_t)((int32_t)r * r - (int32_t)dy * dy)));

      if ((corners & UI_ROUND_BOTTOM_LEFT) != 0u)  { inset_l = d; }
      if ((corners & UI_ROUND_BOTTOM_RIGHT) != 0u) { inset_r = d; }
    }
    /* 中间部分 / 没被指定的角 inset = 0：整宽 */
  }

  *x0 = (int16_t)(x_min + inset_l);
  *x1 = (int16_t)(x_max - inset_r);
  return 1u;
}

uint8_t ui_tool_round_rect_row(int16_t cx, int16_t cy, int16_t w, int16_t h,
                               int16_t radius, int16_t y,
                               int16_t *x0, int16_t *x1)
{
  return ui_tool_round_rect_row_ex(cx, cy, w, h, radius, y, UI_ROUND_ALL, x0, x1);
}


/* 把圆角矩形的每一行涂满：use_grad = 1 走竖直渐变，否则涂单色。
 * corners 决定圆哪些角（实心柱身只圆上角就传 UI_ROUND_TOP）。
 * 上色统一走 ui_rect_span，para->alpha 设了半透明就自动和下层混合 */
static void round_fill_rows(const ui_rect_para_t *para, int16_t radius, uint8_t corners,
                            ui_color_t c_bottom, ui_color_t c_top, uint8_t use_grad)
{
  int16_t  y_min = (int16_t)(para->head.center_y - (para->h >> 1));
  int16_t  y_max = (int16_t)(y_min + para->h - 1);
  uint16_t tmax  = (uint16_t)(para->h > 1 ? para->h - 1 : 1);

  for (uint16_t row = 0; row < ui_row_draw_rows(); row++)
  {
    int16_t y = ui_row_draw_screen_y(row);
    int16_t x0, x1;

    if (ui_tool_round_rect_row_ex(para->head.center_x, para->head.center_y,
                                  para->w, para->h, radius, y, corners, &x0, &x1) == 0u)
    {
      continue;
    }
    if (x1 < ui_row_draw_left() || x0 > ui_row_draw_right())     /* 跟当前脏条不相交 */
    {
      continue;
    }

    ui_row_draw_select(row);
    if (use_grad)
    {
      ui_rect_span(para, x0, x1, ui_color_lerp(c_bottom, c_top, (uint16_t)(y_max - y), tmax));
    }
    else
    {
      ui_rect_span(para, x0, x1, c_bottom);
    }
  }
}

void ui_tool_draw_round_grad(void *para, int16_t radius, ui_color_t c_bottom, ui_color_t c_top)
{
  ui_rect_para_t *p = (ui_rect_para_t *)para;

  if (p == 0 || p->head.type != UI_TYPE_RECT)
  {
    return;
  }
  round_fill_rows(p, radius, UI_ROUND_ALL, c_bottom, c_top, 1u);
}

void ui_tool_draw_round_rect(void *para, int16_t radius)
{
  ui_tool_draw_round_rect_ex(para, radius, UI_ROUND_ALL);
}

void ui_tool_draw_round_rect_ex(void *para, int16_t radius, uint8_t corners)
{
  ui_rect_para_t *p = (ui_rect_para_t *)para;
  int16_t border;

  if (p == 0 || p->head.type != UI_TYPE_RECT)
  {
    return;
  }

  /* 实心：直接一把涂满 */
  if (p->fill)
  {
    round_fill_rows(p, radius, corners, p->color, p->color, 0u);
    return;
  }

  /* 只描边：本行"外圆角范围"减去"内圆角范围"就是这一行的边框。
   * 内圆角矩形 = 中心不动、宽高各缩 2×border，半径缩 border */
  border = p->border_width;
  if (border < 1) { border = 1; }
  if (border * 2 >= p->w || border * 2 >= p->h)      /* 太粗就退化成实心，免得算穿 */
  {
    round_fill_rows(p, radius, corners, p->color, p->color, 0u);
    return;
  }

  {
    int16_t ir = (int16_t)(radius - border);
    if (ir < 0) { ir = 0; }

    for (uint16_t row = 0; row < ui_row_draw_rows(); row++)
    {
      int16_t y = ui_row_draw_screen_y(row);
      int16_t x0, x1, ix0, ix1;

      if (ui_tool_round_rect_row_ex(p->head.center_x, p->head.center_y,
                                    p->w, p->h, radius, y, corners, &x0, &x1) == 0u)
      {
        continue;
      }
      if (x1 < ui_row_draw_left() || x0 > ui_row_draw_right())
      {
        continue;
      }

      ui_row_draw_select(row);

      if (ui_tool_round_rect_row_ex(p->head.center_x, p->head.center_y,
                                    (int16_t)(p->w - border * 2), (int16_t)(p->h - border * 2),
                                    ir, y, corners, &ix0, &ix1) != 0u)
      {
        /* 中间行：左边一条、右边一条 */
        if (x0 <= (int16_t)(ix0 - 1)) { ui_rect_span(p, x0, (int16_t)(ix0 - 1), p->color); }
        if ((int16_t)(ix1 + 1) <= x1) { ui_rect_span(p, (int16_t)(ix1 + 1), x1, p->color); }
      }
      else
      {
        /* 上/下端盖所在的几行：圆角范围内整行都是边 */
        ui_rect_span(p, x0, x1, p->color);
      }
    }
  }
}

/* ==================================================================
 * 图片 / 图标按行搬运（裁剪在这里做，见 ui_tool.h）
 * ================================================================== */

/* 图片一行和"当前脏条 ∩ 视窗裁剪"求交：交上的屏幕列段写回 *sx0 / *sx1。
 * 返回 1 = 有交集；0 = 整行被裁掉（视窗纵向裁剪 / 横向不相交） */
static uint8_t image_row_clip(int16_t img_x_min, int16_t img_x_max,
                              int16_t *sx0, int16_t *sx1)
{
  if (ui_row_draw_row_visible() == 0u)          /* 视窗纵向裁剪：这一行整行不画 */
  {
    return 0u;
  }

  /* 横向：先夹视窗裁剪，再夹当前脏条（没设裁剪时 clip_left/right 就是脏条本身） */
  *sx0 = (img_x_min > ui_row_clip_left())  ? img_x_min : ui_row_clip_left();
  *sx1 = (img_x_max < ui_row_clip_right()) ? img_x_max : ui_row_clip_right();
  return (uint8_t)(*sx0 <= *sx1);
}

/* 把"已裁好的屏幕段 [sx0, sx1]"搬进行缓冲：src 指向屏幕 sx0 那一列的像素。
 * 直写缓冲（不走 ui_row_draw_span），所以纵向裁剪必须在 image_row_clip 里判过。
 * key_on != 0 时值等于 key 的像素【跳过不写】：缓冲里那块保持原样，也就是让下层的
 * 卡片底透出来 —— 这就是片外图标的"透明"(RGB565 没有 alpha，透明靠约定键色)。
 * alpha != 0/255 时，非键色像素再和缓冲里已有的下层做半透明混合 */
static void image_row_blit(uint16_t strip_row, int16_t sx0, int16_t sx1,
                           const ui_color_t *src, ui_color_t key, uint8_t key_on, uint8_t alpha)
{
  ui_color_t *dst = ui_row_draw_select(strip_row) + (sx0 - ui_row_draw_left());
  int16_t n = (int16_t)(sx1 - sx0 + 1);

  while (n-- > 0)
  {
    ui_color_t c = *src++;

    if (key_on == 0u || c != key)
    {
      if (alpha == 0u || alpha >= 255u)
      {
        *dst = c;                   /* 不透明：直接写；键色像素跳过，dst 里保留下层内容 */
      }
      else
      {
        *dst = ui_color_blend(*dst, c, alpha);
      }
    }
    dst++;
  }
}

uint8_t ui_tool_image_row(uint16_t strip_row, const ui_color_t *row_px,
                          int16_t img_x_min, int16_t img_x_max,
                          ui_color_t key, uint8_t key_on, uint8_t alpha)
{
  int16_t sx0, sx1;

  /* 先把绘制游标指到这一行：image_row_clip 里的"行可见"判据看的是【当前行】，
   * 不先 select 就会拿上一行的 y 去判，窗口 / 视窗边沿会漏一行 */
  ui_row_draw_select(strip_row);

  if (row_px == NULL || image_row_clip(img_x_min, img_x_max, &sx0, &sx1) == 0u)
  {
    return 0u;
  }

  image_row_blit(strip_row, sx0, sx1, row_px + (sx0 - img_x_min), key, key_on, alpha);
  return 1u;
}

uint8_t ui_tool_image_row_read(uint16_t strip_row,
                               ui_img_row_fn_t row_fn, uint8_t img_id, int16_t img_y,
                               int16_t img_x_min, int16_t img_x_max,
                               ui_color_t *line_buf, uint16_t line_max,
                               ui_color_t key, uint8_t key_on, uint8_t alpha)
{
  int16_t sx0, sx1, col;

  /* 同 ui_tool_image_row：先指到这一行，行可见判据才用的是本行 */
  ui_row_draw_select(strip_row);

  if (row_fn == NULL || line_buf == NULL || line_max == 0u ||
      image_row_clip(img_x_min, img_x_max, &sx0, &sx1) == 0u)
  {
    return 0u;
  }

  /* 可见段对应的图片列范围，一段一段读、读一段搬一段：
   * 回调每次最多读 line_max 个像素，规格比缓冲宽的图片自动分几段 */
  col = (int16_t)(sx0 - img_x_min);
  while (col <= (int16_t)(sx1 - img_x_min))
  {
    int16_t len = (int16_t)((sx1 - img_x_min) - col + 1);

    if (len > (int16_t)line_max)
    {
      len = (int16_t)line_max;
    }
    if (row_fn(img_id, img_y, col, (int16_t)(col + len - 1), line_buf) == 0u)
    {
      return 0u;                    /* 回调取不到：剩余部分不画 */
    }
    image_row_blit(strip_row, (int16_t)(img_x_min + col),
                   (int16_t)(img_x_min + col + len - 1), line_buf, key, key_on, alpha);
    col = (int16_t)(col + len);
  }
  return 1u;
}
