#include "./ui_anim/ui_anim.h"
#include "./ui_control/ui_control.h"   /* ui_polyline_update_frame / ui_cursor_target_frame（搬去 ui_control 了） */

#include <string.h>   /* memset：注册时把状态清零 */


typedef struct
{
  int16_t x_min;
  int16_t x_max;
  int16_t y_min;
  int16_t y_max;
}dirty_rect_t;


static uint32_t ui_anim_ms = 0;

void ui_anim_tick(void *arg)
{
  (void)arg;                  /* 用不到：留着这个参数是为了能【直接】当定时器回调挂上去 */

  ui_anim_ms++;
}

uint32_t ui_anim_now_ms(void)
{
  return ui_anim_ms;          /* 32 位读在 Cortex-M 上是一条 LDR，原子，不用关中断 */
}

static ui_type_t ui_obj_analysis_type(ui_object_t *obj)
{
  if (obj == NULL || obj->para == NULL)
  {
    return UI_TYPE_NONE;
  }
  ui_head_t *head = (ui_head_t *)obj->para;
  return head->type;
}

static void ui_dirty_rect_set_chunk(dirty_rect_t* rect)
{
  int16_t x_min = rect->x_min;
  int16_t x_max = rect->x_max;
  int16_t y_min = rect->y_min;
  int16_t y_max = rect->y_max;
  ui_dirty_mark(x_min, y_min, x_max, y_max);
}

static dirty_rect_t ui_dirty_rect_unionset(dirty_rect_t* old_rect, dirty_rect_t* new_rect)
{
  dirty_rect_t temp_rect = {0};
  if (old_rect == NULL || new_rect == NULL)
  {
    return temp_rect;
  }
  if (old_rect->x_min > new_rect->x_max || old_rect->x_max < new_rect->x_min || old_rect->y_min > new_rect->y_max || old_rect->y_max < new_rect->y_min)
  {
    ui_dirty_mark(old_rect->x_min, old_rect->y_min, old_rect->x_max, old_rect->y_max);
    return *new_rect;
  }
  temp_rect.x_min = (old_rect->x_min <= new_rect->x_min) ? old_rect->x_min : new_rect->x_min;
  temp_rect.x_max = (old_rect->x_max >= new_rect->x_max) ? old_rect->x_max : new_rect->x_max;
  temp_rect.y_min = (old_rect->y_min <= new_rect->y_min) ? old_rect->y_min : new_rect->y_min;
  temp_rect.y_max = (old_rect->y_max >= new_rect->y_max) ? old_rect->y_max : new_rect->y_max;
  return temp_rect;
}

/* 由"锚点 + 锚点→基准中心偏移 × 因子"算出虚拟中心。
 * den 为 0（还没初始化）时按不缩放处理 */
static int16_t ui_xf_center(int16_t off, int16_t anchor, uint16_t num, uint16_t den)
{
  if (den == 0u)
  {
    return (int16_t)(anchor + off);
  }
  return (int16_t)((int32_t)anchor + (int32_t)off * (int32_t)num / (int32_t)den);
}

/* 平移 / 缩放之后把单体对象的虚拟中心刷新一遍：中心 = 锚点 + 偏移 × 因子 */
static void ui_xf_sync_center(ui_head_t* head, const ui_xform_t* xf)
{
  head->center_x = ui_xf_center(xf->anchor_off_x, xf->anchor_x, xf->scale_num, xf->scale_den);
  head->center_y = ui_xf_center(xf->anchor_off_y, xf->anchor_y, xf->scale_num, xf->scale_den);
}





/* 平移：把各自的几何中心按 *dx / *dy 挪走。
 * 形参统一成 void* + 位移指针，好挂进下面同一张表、按 type 直接取。
 * dx / dy 是输入，函数把实际生效的位移回写回去：这四种都是整段照搬，
 * 成功时就是原样，失败时清零 —— 调用者之后可以拿它接着算脏区 */
static uint8_t ui_move_rect(void* para, int16_t* dx, int16_t* dy)
{
  ui_rect_para_t* p = (ui_rect_para_t*)para;

  if (dx == NULL || dy == NULL)
  {
    return 0;
  }
  if (p == NULL)
  {
    *dx = 0;
    *dy = 0;
    return 0;
  }

  dirty_rect_t old_rect = {0};
  dirty_rect_t new_rect = {0};

  /* 描边带必须和 ui_draw_rect 画的边界**逐像素对齐**：那边是
   * x_min = 中心 - w/2、x_max = x_min + w - 1（y 同理），
   * 这里就用同一套算 —— 直接写 中心 ± w/2，w 为偶数时会差 1px，
   * 右描边最左一列 / 下描边最上一行就漏标、留一条擦不掉的旧线 */
  old_rect.x_min = p->head.center_x - (p->w >> 1);
  old_rect.x_max = (int16_t)(old_rect.x_min + p->w - 1);
  old_rect.y_min = p->head.center_y - (p->h >> 1);
  old_rect.y_max = (int16_t)(old_rect.y_min + p->h - 1);

  /* 只挪当前锚点；虚拟中心 = 锚点 + 偏移 × 因子 */
  p->xf.anchor_x = (int16_t)(p->xf.anchor_x + *dx);
  p->xf.anchor_y = (int16_t)(p->xf.anchor_y + *dy);
  ui_xf_sync_center(&p->head, &p->xf);

  new_rect.x_min = p->head.center_x - (p->w >> 1);
  new_rect.x_max = (int16_t)(new_rect.x_min + p->w - 1);
  new_rect.y_min = p->head.center_y - (p->h >> 1);
  new_rect.y_max = (int16_t)(new_rect.y_min + p->h - 1);

  /* 描边矩形只标四条描边带；填充矩形整框都画了，按 fill 由 helper 决定标边带还是整框 */
  ui_dirty_mark_rect_move(old_rect.x_min, old_rect.y_min, old_rect.x_max, old_rect.y_max,
                          new_rect.x_min, new_rect.y_min, new_rect.x_max, new_rect.y_max,
                          p->border_width, p->fill);

  return 1;
}

/* 进度条当前外接框（四边，含端点）：圆环按 2r+1、矩形按 w×h，中心取 head。
 * 和平移 / 缩放 / 复位 / 外接框查询同一套口径 */
static void ui_progress_box(const ui_progress_common_t* p,
                            int16_t* x0, int16_t* y0, int16_t* x1, int16_t* y1)
{
  int16_t w, h;

  if (p->shape == (uint8_t)UI_PROGRESS_SHAPE_CIRCLE)
  {
    int16_t r = ((const ui_circle_progress_bar_para_t*)p)->external_r;
    w = (int16_t)(r * 2 + 1);
    h = w;
  }
  else
  {
    const ui_rect_progress_bar_para_t* rp = (const ui_rect_progress_bar_para_t*)p;
    w = rp->w;
    h = rp->h;
  }

  ui_box_of(p->head.center_x, p->head.center_y, w, h, x0, y0, x1, y1);
}

/* 读进度条的当前 progress：圆环和矩形的 progress 字段偏移不同，按 shape 取 */
static uint16_t ui_progress_get_value(const ui_progress_common_t* p)
{
  if (p->shape == (uint8_t)UI_PROGRESS_SHAPE_CIRCLE)
  {
    return ((const ui_circle_progress_bar_para_t*)p)->progress;
  }
  return ((const ui_rect_progress_bar_para_t*)p)->progress;
}

/* 写进度条的当前 progress，并只标"新旧进度之间真正变了的那一段"的脏(几何见 ui_create.c)。
 * 值没变就直接返回：一个像素都没动，不写也不标脏 —— 动画停在终点后零开销。
 * 进度是"绝对当前值"，逐帧写不累积误差；脏区只覆盖变化那一段，不再整框重画 */
static void ui_progress_set_value(ui_progress_common_t* p, uint16_t value)
{
  uint16_t old_value = ui_progress_get_value(p);

  if (value == old_value)
  {
    return;
  }

  if (p->shape == (uint8_t)UI_PROGRESS_SHAPE_CIRCLE)
  {
    ((ui_circle_progress_bar_para_t*)p)->progress = value;
  }
  else
  {
    ((ui_rect_progress_bar_para_t*)p)->progress = value;
  }

  ui_progress_mark_value_change(p, old_value, value);
}

/* 进度条平移：和圆 / 矩形同一套 —— 只挪当前锚点，虚拟中心 = 锚点 + 偏移 × 因子；
 * 老框 / 新框都整块标脏（圆环外框或矩形外框按 shape 取） */
static uint8_t ui_move_progress(void* para, int16_t* dx, int16_t* dy)
{
  ui_progress_common_t* p = (ui_progress_common_t*)para;
  int16_t ox0, oy0, ox1, oy1, nx0, ny0, nx1, ny1;

  if (dx == NULL || dy == NULL)
  {
    return 0;
  }
  if (p == NULL)
  {
    *dx = 0;
    *dy = 0;
    return 0;
  }

  ui_progress_box(p, &ox0, &oy0, &ox1, &oy1);

  p->xf.anchor_x = (int16_t)(p->xf.anchor_x + *dx);
  p->xf.anchor_y = (int16_t)(p->xf.anchor_y + *dy);
  ui_xf_sync_center(&p->head, &p->xf);

  ui_progress_box(p, &nx0, &ny0, &nx1, &ny1);
  ui_dirty_mark(ox0, oy0, ox1, oy1);
  ui_dirty_mark(nx0, ny0, nx1, ny1);
  return 1;
}

static uint8_t ui_move_text(void* para, int16_t* dx, int16_t* dy)
{
  ui_text_para_t* p = (ui_text_para_t*)para;

  if (dx == NULL || dy == NULL)
  {
    return 0;
  }
  if (p == NULL)
  {
    *dx = 0;
    *dy = 0;
    return 0;
  }

  dirty_rect_t old_rect = {0};
  dirty_rect_t new_rect = {0};
  dirty_rect_t union_rect = {0};

  old_rect.x_min = p->head.center_x - (p->w >> 1);
  old_rect.x_max = p->head.center_x + (p->w >> 1);
  old_rect.y_min = p->head.center_y - (p->h >> 1);
  old_rect.y_max = p->head.center_y + (p->h >> 1);

  /* 只挪当前锚点；虚拟中心 = 锚点 + 偏移 × 因子 */
  p->xf.anchor_x = (int16_t)(p->xf.anchor_x + *dx);
  p->xf.anchor_y = (int16_t)(p->xf.anchor_y + *dy);
  ui_xf_sync_center(&p->head, &p->xf);

  new_rect.x_min = p->head.center_x - (p->w >> 1);
  new_rect.x_max = p->head.center_x + (p->w >> 1);
  new_rect.y_min = p->head.center_y - (p->h >> 1);
  new_rect.y_max = p->head.center_y + (p->h >> 1);

  /* 文字是实心点阵：整块都可能变，按老框 ∪ 新框整块标脏（不能只标描边） */
  union_rect = ui_dirty_rect_unionset(&old_rect, &new_rect);
  ui_dirty_rect_set_chunk(&union_rect);
  return 1;
}

static uint8_t ui_move_circle(void* para, int16_t* dx, int16_t* dy)
{
  ui_circle_para_t* p = (ui_circle_para_t*)para;

  if (dx == NULL || dy == NULL)
  {
    return 0;
  }
  if (p == NULL)
  {
    *dx = 0;
    *dy = 0;
    return 0;
  }

  dirty_rect_t old_rect = {0};
  dirty_rect_t new_rect = {0};
  dirty_rect_t union_rect = {0};

  old_rect.x_min = p->head.center_x - p->external_r;
  old_rect.x_max = p->head.center_x + p->external_r;
  old_rect.y_min = p->head.center_y - p->external_r;
  old_rect.y_max = p->head.center_y + p->external_r;

  /* 只挪当前锚点；虚拟中心 = 锚点 + 偏移 × 因子 */
  p->xf.anchor_x = (int16_t)(p->xf.anchor_x + *dx);
  p->xf.anchor_y = (int16_t)(p->xf.anchor_y + *dy);
  ui_xf_sync_center(&p->head, &p->xf);

  new_rect.x_min = p->head.center_x - p->external_r;
  new_rect.x_max = p->head.center_x + p->external_r;
  new_rect.y_min = p->head.center_y - p->external_r;
  new_rect.y_max = p->head.center_y + p->external_r;

  union_rect = ui_dirty_rect_unionset(&old_rect, &new_rect);
  ui_dirty_rect_set_chunk(&union_rect);
  return 1;
}

static uint8_t ui_move_point(void* para, int16_t* dx, int16_t* dy)
{
  ui_point_para_t* p = (ui_point_para_t*)para;

  if (dx == NULL || dy == NULL)
  {
    return 0;
  }
  if (p == NULL)
  {
    *dx = 0;
    *dy = 0;
    return 0;
  }

  dirty_rect_t old_rect = {0};
  dirty_rect_t new_rect = {0};
  dirty_rect_t union_rect = {0};

  /* 边界跟 ui_draw_point 那一套一致（x_max = x_min + size - 1），跟画出来的像素逐像素对齐 */
  old_rect.x_min = (int16_t)(p->head.center_x - (p->size >> 1));
  old_rect.x_max = (int16_t)(old_rect.x_min + p->size - 1);
  old_rect.y_min = (int16_t)(p->head.center_y - (p->size >> 1));
  old_rect.y_max = (int16_t)(old_rect.y_min + p->size - 1);

  /* 只挪当前锚点；虚拟中心 = 锚点 + 偏移 × 因子 */
  p->xf.anchor_x = (int16_t)(p->xf.anchor_x + *dx);
  p->xf.anchor_y = (int16_t)(p->xf.anchor_y + *dy);
  ui_xf_sync_center(&p->head, &p->xf);

  new_rect.x_min = (int16_t)(p->head.center_x - (p->size >> 1));
  new_rect.x_max = (int16_t)(new_rect.x_min + p->size - 1);
  new_rect.y_min = (int16_t)(p->head.center_y - (p->size >> 1));
  new_rect.y_max = (int16_t)(new_rect.y_min + p->size - 1);

  /* 点是实心块：按 老框 ∪ 新框 整块标脏 */
  union_rect = ui_dirty_rect_unionset(&old_rect, &new_rect);
  ui_dirty_rect_set_chunk(&union_rect);
  return 1;
}

/* 图片平移：和矩形同一套 —— 只挪当前锚点，虚拟中心 = 锚点 + 偏移 × 因子。
 * 图片当前不做缩放（缩放表里没登记它），规格恒定，框只随中心走；
 * 图片是实心块：按 老框 ∪ 新框 整块标脏 */
static uint8_t ui_move_image(void* para, int16_t* dx, int16_t* dy)
{
  ui_image_para_t* p = (ui_image_para_t*)para;

  if (dx == NULL || dy == NULL)
  {
    return 0;
  }
  if (p == NULL)
  {
    *dx = 0;
    *dy = 0;
    return 0;
  }

  dirty_rect_t old_rect = {0};
  dirty_rect_t new_rect = {0};
  dirty_rect_t union_rect = {0};

  /* 边界跟 ui_draw_image 那一套一致（x_max = x_min + w - 1），跟画出来的像素逐像素对齐 */
  old_rect.x_min = (int16_t)(p->head.center_x - (p->w >> 1));
  old_rect.x_max = (int16_t)(old_rect.x_min + p->w - 1);
  old_rect.y_min = (int16_t)(p->head.center_y - (p->h >> 1));
  old_rect.y_max = (int16_t)(old_rect.y_min + p->h - 1);

  /* 只挪当前锚点；虚拟中心 = 锚点 + 偏移 × 因子 */
  p->xf.anchor_x = (int16_t)(p->xf.anchor_x + *dx);
  p->xf.anchor_y = (int16_t)(p->xf.anchor_y + *dy);
  ui_xf_sync_center(&p->head, &p->xf);

  new_rect.x_min = (int16_t)(p->head.center_x - (p->w >> 1));
  new_rect.x_max = (int16_t)(new_rect.x_min + p->w - 1);
  new_rect.y_min = (int16_t)(p->head.center_y - (p->h >> 1));
  new_rect.y_max = (int16_t)(new_rect.y_min + p->h - 1);

  union_rect = ui_dirty_rect_unionset(&old_rect, &new_rect);
  ui_dirty_rect_set_chunk(&union_rect);
  return 1;
}

/* 光标按它自己的方框挪（center_x / center_y，与 ui_draw_cursor 同一套字段）；
 * 要不要跟着被框对象走是 ui_cursor_fit 的活，这里只管把方框平移 */
static uint8_t ui_move_cursor(void* para, int16_t* dx, int16_t* dy)
{
  ui_cursor_para_t* p = (ui_cursor_para_t*)para;

  if (dx == NULL || dy == NULL)
  {
    return 0;
  }
  if (p == NULL)
  {
    *dx = 0;
    *dy = 0;
    return 0;
  }

  dirty_rect_t old_rect = {0};
  dirty_rect_t new_rect = {0};

  /* 边界按 ui_draw_rect 那一套算（x_max = x_min + w - 1），好让描边带逐像素对齐 */
  old_rect.x_min = p->head.center_x - (p->w >> 1);
  old_rect.x_max = (int16_t)(old_rect.x_min + p->w - 1);
  old_rect.y_min = p->head.center_y - (p->h >> 1);
  old_rect.y_max = (int16_t)(old_rect.y_min + p->h - 1);

  p->head.center_x = (int16_t)(p->head.center_x + *dx);
  p->head.center_y = (int16_t)(p->head.center_y + *dy);

  new_rect.x_min = p->head.center_x - (p->w >> 1);
  new_rect.x_max = (int16_t)(new_rect.x_min + p->w - 1);
  new_rect.y_min = p->head.center_y - (p->h >> 1);
  new_rect.y_max = (int16_t)(new_rect.y_min + p->h - 1);

  /* 光标只描边、内部是背景：只标四条描边带（old / new 都包进去）。
   * 左右两条竖带各自是一次 DMA 推完的一条 —— 分条错位（台阶）就是这么消掉的 */
  ui_dirty_mark_rect_outline(old_rect.x_min, old_rect.y_min, old_rect.x_max, old_rect.y_max,
                             new_rect.x_min, new_rect.y_min, new_rect.x_max, new_rect.y_max,
                             (int16_t)p->border_width);
  return 1;
}

/* 折线整体平移：只挪当前锚点（顶点偏移是相对锚点的，原样不动），整条形状不做形变。
 * 锚点动完调 ui_polyline_update_frame 重算对象框（顺带更新 center），
 * 再按 老框 ∪ 新框 记脏区 —— 对象框是折线唯一的脏区依据 */
static uint8_t ui_move_polyline(void* para, int16_t* dx, int16_t* dy)
{
  ui_polyline_para_t* p = (ui_polyline_para_t*)para;

  if (dx == NULL || dy == NULL)
  {
    return 0;
  }
  if (p == NULL)
  {
    *dx = 0;
    *dy = 0;
    return 0;
  }

  dirty_rect_t old_rect = {0};
  dirty_rect_t new_rect = {0};
  dirty_rect_t union_rect = {0};

  old_rect.x_min = p->head.center_x - (p->w >> 1);
  old_rect.x_max = (int16_t)(old_rect.x_min + p->w - 1);
  old_rect.y_min = p->head.center_y - (p->h >> 1);
  old_rect.y_max = (int16_t)(old_rect.y_min + p->h - 1);

  /* 只挪锚点：顶点偏移是相对锚点的，平移时原样不动 */
  p->anchor_x = (int16_t)(p->anchor_x + *dx);
  p->anchor_y = (int16_t)(p->anchor_y + *dy);

  ui_polyline_update_frame(p);

  new_rect.x_min = p->head.center_x - (p->w >> 1);
  new_rect.x_max = (int16_t)(new_rect.x_min + p->w - 1);
  new_rect.y_min = p->head.center_y - (p->h >> 1);
  new_rect.y_max = (int16_t)(new_rect.y_min + p->h - 1);

  union_rect = ui_dirty_rect_unionset(&old_rect, &new_rect);
  ui_dirty_rect_set_chunk(&union_rect);
  return 1;
}



static uint8_t ui_move_union(void* para, int16_t* dx, int16_t* dy);   /* 前置：子对象递归要用 */
static uint8_t ui_move_viewport(void* para, int16_t* dx, int16_t* dy);   /* 前置：视窗也是容器，递归要用 */
static uint8_t ui_scale_viewport(void* para, uint16_t* num, uint16_t* den); /* 前置：同上 */

/* 合并对象 / 视窗的"中心 + 宽高 → 四边"都用公共的 ui_box_of（见 ui_create.h） */

/* 平移一个子对象：折线连起始点 + 顶点一起挪（借 ui_move_polyline）；
 * 合并对象 / 视窗递归整体平移；其余图元交给它们自己的平移分支 */
static void ui_union_move_child(ui_object_t* obj, int16_t dx, int16_t dy)
{
  ui_head_t* head;
  int16_t ddx, ddy;

  if (obj == NULL || obj->para == NULL)
  {
    return;
  }
  head = (ui_head_t*)obj->para;

  switch (head->type)
  {
    case UI_TYPE_POLYLINE:
      ddx = dx; ddy = dy;
      ui_move_polyline(head, &ddx, &ddy);
      break;
    case UI_TYPE_UNION:
      ddx = dx; ddy = dy;
      ui_move_union(head, &ddx, &ddy);
      break;
    case UI_TYPE_VIEWPORT:
      ddx = dx; ddy = dy;
      ui_move_viewport(head, &ddx, &ddy);
      break;
    default:
      /* 其余图元(矩形 / 文字 / 圆环 / 点 / 光标)：走它们自己的平移分支 ——
       * 带变换块(xf)的那几种语义是"挪锚点 + 由锚点重算中心(并把每条描边带标脏)"。
       * 这里若图省事直接写中心，就破坏了"中心 = 锚点 + 偏移"这个不变量：
       * 下一次它自己平移 / 缩放时，ui_xf_sync_center 会按旧锚点把这次容器带来的位移抹掉，
       * 表现就是"视窗拖走了、里面的元素又自己弹回原位" */
      ui_anim_move_obj(obj, dx, dy);
      break;
  }
}

/* 合并对象整体平移：锚点 + 每个子对象一起挪 dx / dy。
 * 锚点只按平移量走、**不重新从子对象取整推导**，所以不会每帧漂；
 * 子对象的基准偏移（相对锚点）不变，于是平移不会破坏按绝对比例的缩放。老框 ∪ 新框标脏 */
static uint8_t ui_move_union(void* para, int16_t* dx, int16_t* dy)
{
  ui_union_para_t* p = (ui_union_para_t*)para;
  ui_merge_obj_node_t* node;
  int16_t ox0 = 0, oy0 = 0, ox1 = 0, oy1 = 0;
  uint8_t had_old;

  if (dx == NULL || dy == NULL)
  {
    return 0;
  }
  if (p == NULL)
  {
    *dx = 0;
    *dy = 0;
    return 0;
  }
  if (*dx == 0 && *dy == 0)
  {
    return 1;
  }

  had_old = (uint8_t)(p->w > 0 && p->h > 0);
  if (had_old != 0u)
  {
    ui_box_of(p->head.center_x, p->head.center_y, p->w, p->h, &ox0, &oy0, &ox1, &oy1);
  }

  ListForEachEntry(node, &p->list, ui_merge_obj_node_t, node)
  {
    ui_union_move_child(node->obj, *dx, *dy);
  }

  /* 框中心和平移锚点一起走；w / h 不变（框中心和锚点都不靠子对象重推，所以不漂） */
  p->head.center_x = (int16_t)(p->head.center_x + *dx);
  p->head.center_y = (int16_t)(p->head.center_y + *dy);
  p->anchor_x = (int16_t)(p->anchor_x + *dx);
  p->anchor_y = (int16_t)(p->anchor_y + *dy);

  if (p->w > 0 && p->h > 0)
  {
    int16_t nx0, ny0, nx1, ny1;

    ui_box_of(p->head.center_x, p->head.center_y, p->w, p->h, &nx0, &ny0, &nx1, &ny1);
    if (had_old != 0u)
    {
      ui_dirty_mark(ox0, oy0, ox1, oy1);
    }
    ui_dirty_mark(nx0, ny0, nx1, ny1);
  }
  return 1;
}

/* 视窗整体平移：窗口尺寸不变，锚点、窗口中心、每个子对象一起挪 dx / dy。
 * 子对象的位置基准（相对锚点）不变，于是平移不破坏按绝对比例的缩放。
 * 老窗 ∪ 新窗标脏 */
static uint8_t ui_move_viewport(void* para, int16_t* dx, int16_t* dy)
{
  ui_viewport_para_t* p = (ui_viewport_para_t*)para;
  ui_merge_obj_node_t* node;
  int16_t ox0 = 0, oy0 = 0, ox1 = 0, oy1 = 0;
  uint8_t had_old;

  if (dx == NULL || dy == NULL)
  {
    return 0;
  }
  if (p == NULL)
  {
    *dx = 0;
    *dy = 0;
    return 0;
  }
  if (*dx == 0 && *dy == 0)
  {
    return 1;
  }

  had_old = (uint8_t)(p->w > 0 && p->h > 0);
  if (had_old != 0u)
  {
    ui_box_of(p->head.center_x, p->head.center_y, p->w, p->h, &ox0, &oy0, &ox1, &oy1);
  }

  ListForEachEntry(node, &p->list, ui_merge_obj_node_t, node)
  {
    ui_union_move_child(node->obj, *dx, *dy);
  }

  /* 窗口中心、基准中心、锚点（含基准锚点）一起走，窗口尺寸不变 */
  p->head.center_x = (int16_t)(p->head.center_x + *dx);
  p->head.center_y = (int16_t)(p->head.center_y + *dy);
  p->base_cx = (int16_t)(p->base_cx + *dx);
  p->base_cy = (int16_t)(p->base_cy + *dy);
  p->anchor_x = (int16_t)(p->anchor_x + *dx);
  p->anchor_y = (int16_t)(p->anchor_y + *dy);
  p->base_anchor_x = (int16_t)(p->base_anchor_x + *dx);
  p->base_anchor_y = (int16_t)(p->base_anchor_y + *dy);

  if (p->w > 0 && p->h > 0)
  {
    int16_t nx0, ny0, nx1, ny1;

    ui_box_of(p->head.center_x, p->head.center_y, p->w, p->h, &nx0, &ny0, &nx1, &ny1);
    if (had_old != 0u)
    {
      ui_dirty_mark(ox0, oy0, ox1, oy1);
    }
    ui_dirty_mark(nx0, ny0, nx1, ny1);
  }
  return 1;
}

typedef uint8_t (*anim_move_obj_fn_t)(void*, int16_t*, int16_t*);//平移函数指针类型

static const anim_move_obj_fn_t anim_move_obj_tab[] =
{
  [UI_TYPE_NONE] = NULL,
  [UI_TYPE_RECT] = ui_move_rect,
  [UI_TYPE_TEXT] = ui_move_text,
  [UI_TYPE_CIRCLE] = ui_move_circle,
  [UI_TYPE_CURSOR] = ui_move_cursor,
  [UI_TYPE_POLYLINE] = ui_move_polyline,
  [UI_TYPE_UNION] = ui_move_union,
  [UI_TYPE_POINT] = ui_move_point,
  [UI_TYPE_VIEWPORT] = ui_move_viewport,
  [UI_TYPE_PROGRESS_BAR] = ui_move_progress,   /* 圆环 / 矩形进度条：按 para 里的 shape 分派 */
  [UI_TYPE_IMAGE] = ui_move_image,             /* 图片只做平移：缩放表没登记它，缩放接口对它不生效 */
};



void ui_anim_move_obj(ui_object_t *obj, int16_t delta_x, int16_t delta_y)
{
  ui_type_t type = ui_obj_analysis_type(obj);
  anim_move_obj_fn_t fn;
  int16_t dx = delta_x, dy = delta_y;

  if (type == UI_TYPE_NONE)
  {
    return;
  }

  /* type 可能没赋过值甚至被写坏，先把下标夹在表内再查 */
  if ((unsigned)type >= sizeof(anim_move_obj_tab) / sizeof(anim_move_obj_tab[0]))
  {
    return;
  }

  fn = anim_move_obj_tab[type];

  /* 表里没登记（NULL）：这个类型不支持平移，什么都不动 */
  if (fn == NULL)
  {
    return;
  }

  fn(obj->para, &dx, &dy);
}



static ui_anim_ext_t* ui_anim_ext_of(ui_object_t *obj);   /* 前置：取对象的 ui_anim 扩展 */

/* 把对象"正在跑的定点移动"整条轨迹平移 dx / dy：起点和终点一起挪，进度不重置。
 * 用途：对象所在容器(视窗 / 合并对象)被整体搬动时 —— 容器已经把对象搬过去了，
 * 这时如果只改终点，缓动会按【旧起点】算绝对位置、把对象往旧轨迹上拉；
 * 起点也挪同样多，对象就沿着平移后的轨迹继续走：不卡住、不重跑、不掉队。
 * 跟随档(ui_anim_move_obj_follow)内部对"每帧都在动的目标"用的就是这个手法。
 * 没注册平移状态就直接返回；dx / dy 都为 0 时什么都不做 */
void ui_anim_move_obj_shift(ui_object_t *obj, int16_t dx, int16_t dy)
{
  ui_anim_ext_t *ext;
  ui_move_anim_t *slot;

  if (obj == NULL || obj->para == NULL || (dx == 0 && dy == 0))
  {
    return;
  }

  ext  = ui_anim_ext_of(obj);
  slot = (ext != NULL) ? ext->move : NULL;
  if (slot == NULL)
  {
    return;
  }

  slot->start_x  = (int16_t)(slot->start_x  + dx);
  slot->start_y  = (int16_t)(slot->start_y  + dy);
  slot->target_x = (int16_t)(slot->target_x + dx);
  slot->target_y = (int16_t)(slot->target_y + dy);
}



/* 缩放：把各自的几何体按中心等比缩放，新尺寸 = 原尺寸 * num / den。
 * 形参统一成 void* + 比例指针，好挂进下面同一张表、按 type 直接取。
 * num / den 是输入，函数把实际生效的比例回写回去：这几种成功时就是原样，
 * 失败时把 num 清零 —— 调用者之后可以拿它接着算脏区 */
static uint8_t ui_scale_rect(void* para, uint16_t* num, uint16_t* den)
{
  ui_rect_para_t* p = (ui_rect_para_t*)para;

  if (num == NULL || den == NULL)
  {
    return 0;
  }
  if (p == NULL || *den == 0)
  {
    *num = 0;
    return 0;
  }

  dirty_rect_t old_rect = {0};
  dirty_rect_t new_rect = {0};

  /* 边界按 ui_draw_rect 那一套算（x_max = x_min + w - 1），好让描边带逐像素对齐 */
  old_rect.x_min = p->head.center_x - (p->w >> 1);
  old_rect.x_max = (int16_t)(old_rect.x_min + p->w - 1);
  old_rect.y_min = p->head.center_y - (p->h >> 1);
  old_rect.y_max = (int16_t)(old_rect.y_min + p->h - 1);

  /* 写因子、按"基准 × 因子"重算虚拟尺寸；锚点不动，中心由 锚点 + 偏移 × 因子 现算 */
  p->xf.scale_num = *num;
  p->xf.scale_den = *den;
  p->w = (int16_t)((int32_t)p->base_w * (*num) / (*den));
  p->h = (int16_t)((int32_t)p->base_h * (*num) / (*den));
  if (p->w < 1) { p->w = 1; }
  if (p->h < 1) { p->h = 1; }
  ui_xf_sync_center(&p->head, &p->xf);

  new_rect.x_min = p->head.center_x - (p->w >> 1);
  new_rect.x_max = (int16_t)(new_rect.x_min + p->w - 1);
  new_rect.y_min = p->head.center_y - (p->h >> 1);
  new_rect.y_max = (int16_t)(new_rect.y_min + p->h - 1);

  /* 描边矩形只标四条描边带；填充矩形整框都画了，按 fill 由 helper 决定标边带还是整框 */
  ui_dirty_mark_rect_move(old_rect.x_min, old_rect.y_min, old_rect.x_max, old_rect.y_max,
                          new_rect.x_min, new_rect.y_min, new_rect.x_max, new_rect.y_max,
                          p->border_width, p->fill);

  return 1;
}

/* 文字缩放：字号（12/16/24/32 四档）不跟着缩，只把"文本可写范围"这个框按比例缩放 ——
 * 新尺寸 = 原尺寸 * num / den，锚点是范围中心（center 不动）。
 * 文字在范围内水平居中 + 纵向居中，所以框缩放后文字自动重新排到新框的中心；
 * 框缩得比文字还小时，超出框的笔画由 ui_draw_text 裁掉。
 * 比例是绝对值，逐帧反复调用不会累积取整误差 */
static uint8_t ui_scale_text(void* para, uint16_t* num, uint16_t* den)
{
  ui_text_para_t* p = (ui_text_para_t*)para;

  if (num == NULL || den == NULL)
  {
    return 0;
  }
  if (p == NULL || *den == 0)
  {
    *num = 0;
    return 0;
  }

  dirty_rect_t old_rect = {0};
  dirty_rect_t new_rect = {0};
  dirty_rect_t union_rect = {0};

  /* 文字是实心点阵：按 老框 ∪ 新框 整块标脏，不能只标描边 */
  old_rect.x_min = p->head.center_x - (p->w >> 1);
  old_rect.x_max = p->head.center_x + (p->w >> 1);
  old_rect.y_min = p->head.center_y - (p->h >> 1);
  old_rect.y_max = p->head.center_y + (p->h >> 1);

  /* 写因子、按"基准 × 因子"重算虚拟尺寸；锚点不动，中心由 锚点 + 偏移 × 因子 现算 */
  p->xf.scale_num = *num;
  p->xf.scale_den = *den;
  p->w = (int16_t)((int32_t)p->base_w * (*num) / (*den));
  p->h = (int16_t)((int32_t)p->base_h * (*num) / (*den));
  if (p->w < 1) { p->w = 1; }
  if (p->h < 1) { p->h = 1; }
  ui_xf_sync_center(&p->head, &p->xf);

  new_rect.x_min = p->head.center_x - (p->w >> 1);
  new_rect.x_max = p->head.center_x + (p->w >> 1);
  new_rect.y_min = p->head.center_y - (p->h >> 1);
  new_rect.y_max = p->head.center_y + (p->h >> 1);

  union_rect = ui_dirty_rect_unionset(&old_rect, &new_rect);
  ui_dirty_rect_set_chunk(&union_rect);
  return 1;
}

static uint8_t ui_scale_circle(void* para, uint16_t* num, uint16_t* den)
{
  ui_circle_para_t* p = (ui_circle_para_t*)para;

  if (num == NULL || den == NULL)
  {
    return 0;
  }
  if (p == NULL || *den == 0)
  {
    *num = 0;
    return 0;
  }

  dirty_rect_t old_rect = {0};
  dirty_rect_t new_rect = {0};
  dirty_rect_t union_rect = {0};

  old_rect.x_min = p->head.center_x - p->external_r;
  old_rect.x_max = p->head.center_x + p->external_r;
  old_rect.y_min = p->head.center_y - p->external_r;
  old_rect.y_max = p->head.center_y + p->external_r;

  /* 写因子、按"基准半径 × 因子"重算虚拟半径；锚点不动，中心由 锚点 + 偏移 × 因子 现算 */
  p->xf.scale_num = *num;
  p->xf.scale_den = *den;
  p->internal_r = (int16_t)((int32_t)p->base_int_r * (*num) / (*den));
  p->external_r = (int16_t)((int32_t)p->base_ext_r * (*num) / (*den));
  if (p->external_r < 1) { p->external_r = 1; }   /* 外半径不许缩到 0，否则整个圆消失 */
  ui_xf_sync_center(&p->head, &p->xf);

  new_rect.x_min = p->head.center_x - p->external_r;
  new_rect.x_max = p->head.center_x + p->external_r;
  new_rect.y_min = p->head.center_y - p->external_r;
  new_rect.y_max = p->head.center_y + p->external_r;

  union_rect = ui_dirty_rect_unionset(&old_rect, &new_rect);
  ui_dirty_rect_set_chunk(&union_rect);

  return 1;
}

static uint8_t ui_scale_point(void* para, uint16_t* num, uint16_t* den)
{
  ui_point_para_t* p = (ui_point_para_t*)para;

  if (num == NULL || den == NULL)
  {
    return 0;
  }
  if (p == NULL || *den == 0)
  {
    *num = 0;
    return 0;
  }

  dirty_rect_t old_rect = {0};
  dirty_rect_t new_rect = {0};
  dirty_rect_t union_rect = {0};

  old_rect.x_min = (int16_t)(p->head.center_x - (p->size >> 1));
  old_rect.x_max = (int16_t)(old_rect.x_min + p->size - 1);
  old_rect.y_min = (int16_t)(p->head.center_y - (p->size >> 1));
  old_rect.y_max = (int16_t)(old_rect.y_min + p->size - 1);

  /* 写因子、按"基准边长 × 因子"重算虚拟边长；锚点不动，中心由 锚点 + 偏移 × 因子 现算 */
  p->xf.scale_num = *num;
  p->xf.scale_den = *den;
  p->size = (int16_t)((int32_t)p->base_size * (*num) / (*den));
  if (p->size < 1) { p->size = 1; }   /* 边长不许缩到 0，否则整个点消失 */
  ui_xf_sync_center(&p->head, &p->xf);

  new_rect.x_min = (int16_t)(p->head.center_x - (p->size >> 1));
  new_rect.x_max = (int16_t)(new_rect.x_min + p->size - 1);
  new_rect.y_min = (int16_t)(p->head.center_y - (p->size >> 1));
  new_rect.y_max = (int16_t)(new_rect.y_min + p->size - 1);

  union_rect = ui_dirty_rect_unionset(&old_rect, &new_rect);
  ui_dirty_rect_set_chunk(&union_rect);

  return 1;
}

/* 光标按它自己的方框等比缩放（center 不动，只有 w / h 变）；
 * 要不要跟着被框对象走是 ui_cursor_fit 的活，这里只管把方框缩 */
static uint8_t ui_scale_cursor(void* para, uint16_t* num, uint16_t* den)
{
  ui_cursor_para_t* p = (ui_cursor_para_t*)para;

  if (num == NULL || den == NULL)
  {
    return 0;
  }
  if (p == NULL || *den == 0)
  {
    *num = 0;
    return 0;
  }

  dirty_rect_t old_rect = {0};
  dirty_rect_t new_rect = {0};

  /* 边界按 ui_draw_rect 那一套算（x_max = x_min + w - 1），好让描边带逐像素对齐 */
  old_rect.x_min = p->head.center_x - (p->w >> 1);
  old_rect.x_max = (int16_t)(old_rect.x_min + p->w - 1);
  old_rect.y_min = p->head.center_y - (p->h >> 1);
  old_rect.y_max = (int16_t)(old_rect.y_min + p->h - 1);

  p->w = (int16_t)((int32_t)p->w * (*num) / (*den));
  p->h = (int16_t)((int32_t)p->h * (*num) / (*den));
  if (p->w < 1) { p->w = 1; }
  if (p->h < 1) { p->h = 1; }

  new_rect.x_min = p->head.center_x - (p->w >> 1);
  new_rect.x_max = (int16_t)(new_rect.x_min + p->w - 1);
  new_rect.y_min = p->head.center_y - (p->h >> 1);
  new_rect.y_max = (int16_t)(new_rect.y_min + p->h - 1);

  /* 光标只描边、内部是背景：只标四条描边带（old / new 都包进去） */
  ui_dirty_mark_rect_outline(old_rect.x_min, old_rect.y_min, old_rect.x_max, old_rect.y_max,
                             new_rect.x_min, new_rect.y_min, new_rect.x_max, new_rect.y_max,
                             (int16_t)p->border_width);

  return 1;
}

/* 折线缩放：把当前缩放因子直接写成 num / den（绝对值），顶点偏移一个都不动 ——
 * 绘制 / 取框时再按 锚点 + 偏移 * num / den 现算。
 * 锚点本身不参与缩放，等价于每段线段长度等比缩放（下一段的起点随之变化），整条线不会被搬走；
 * 写的是绝对因子而非增量，所以逐帧反复调用不会累积取整误差。
 * 因子变完重算对象框，再按 老框 ∪ 新框 记脏区 */
static uint8_t ui_scale_polyline(void* para, uint16_t* num, uint16_t* den)
{
  ui_polyline_para_t* p = (ui_polyline_para_t*)para;

  if (num == NULL || den == NULL)
  {
    return 0;
  }
  if (p == NULL || *den == 0)
  {
    *num = 0;
    return 0;
  }

  dirty_rect_t old_rect = {0};
  dirty_rect_t new_rect = {0};
  dirty_rect_t union_rect = {0};

  old_rect.x_min = p->head.center_x - (p->w >> 1);
  old_rect.x_max = (int16_t)(old_rect.x_min + p->w - 1);
  old_rect.y_min = p->head.center_y - (p->h >> 1);
  old_rect.y_max = (int16_t)(old_rect.y_min + p->h - 1);

  p->scale_num = *num;
  p->scale_den = *den;

  ui_polyline_update_frame(p);

  new_rect.x_min = p->head.center_x - (p->w >> 1);
  new_rect.x_max = (int16_t)(new_rect.x_min + p->w - 1);
  new_rect.y_min = p->head.center_y - (p->h >> 1);
  new_rect.y_max = (int16_t)(new_rect.y_min + p->h - 1);

  union_rect = ui_dirty_rect_unionset(&old_rect, &new_rect);
  ui_dirty_rect_set_chunk(&union_rect);
  return 1;
}



static uint8_t ui_scale_union(void* para, uint16_t* num, uint16_t* den);   /* 前置：子对象递归要用 */

/* 从"子对象自带的基准 × 比例"重设一个子对象：尺寸按比例缩、中心摆到 锚点 + 位置基准×比例。
 * 子对象自带的基准在 ui_union_append 时已对齐到成组那一刻的几何（那里对每个子对象 bake 过）。
 * 线宽 / 边框宽 / 字号不缩。光标不做 union 子对象，故无 CURSOR 分支 */
static void ui_union_scale_child(ui_merge_obj_node_t* node, uint16_t num, uint16_t den,
                                 int16_t ax, int16_t ay)
{
  ui_head_t* head = (ui_head_t*)node->obj->para;
  int16_t tcx = (int16_t)(ax + (int32_t)node->base_rel_x * num / den);
  int16_t tcy = (int16_t)(ay + (int32_t)node->base_rel_y * num / den);

  switch (head->type)
  {
    case UI_TYPE_RECT:
    {
      ui_rect_para_t* p = (ui_rect_para_t*)head;
      p->w = (int16_t)((int32_t)p->base_w * num / den);   /* 线宽 border_width 不缩 */
      p->h = (int16_t)((int32_t)p->base_h * num / den);
      if (p->w < 1) { p->w = 1; }
      if (p->h < 1) { p->h = 1; }
      p->head.center_x = tcx;
      p->head.center_y = tcy;
      break;
    }
    case UI_TYPE_TEXT:
    {
      ui_text_para_t* p = (ui_text_para_t*)head;
      p->w = (int16_t)((int32_t)p->base_w * num / den);   /* 字号不缩，只缩"可写范围"框 */
      p->h = (int16_t)((int32_t)p->base_h * num / den);
      if (p->w < 1) { p->w = 1; }
      if (p->h < 1) { p->h = 1; }
      p->head.center_x = tcx;
      p->head.center_y = tcy;
      break;
    }
    case UI_TYPE_CIRCLE:
    {
      ui_circle_para_t* p = (ui_circle_para_t*)head;
      p->internal_r = (int16_t)((int32_t)p->base_int_r * num / den);
      p->external_r = (int16_t)((int32_t)p->base_ext_r * num / den);
      if (p->external_r < 1) { p->external_r = 1; }       /* 外半径不许缩到 0 */
      p->head.center_x = tcx;
      p->head.center_y = tcy;
      break;
    }
    case UI_TYPE_POINT:
    {
      ui_point_para_t* p = (ui_point_para_t*)head;
      p->size = (int16_t)((int32_t)p->base_size * num / den);   /* 边长按比例缩 */
      if (p->size < 1) { p->size = 1; }
      p->head.center_x = tcx;
      p->head.center_y = tcy;
      break;
    }
    case UI_TYPE_POLYLINE:
    {
      ui_polyline_para_t* p = (ui_polyline_para_t*)head;
      int16_t ddx, ddy;

      p->scale_num = num;                   /* 形状靠它自己的比例缩（基准 = 成组时的偏移），线宽不缩 */
      p->scale_den = den;
      ui_polyline_update_frame(p);
      ddx = (int16_t)(tcx - p->head.center_x);   /* 缩完挪到目标中心（锚点 + 位置基准×比例） */
      ddy = (int16_t)(tcy - p->head.center_y);
      if (ddx != 0 || ddy != 0)
      {
        p->anchor_x = (int16_t)(p->anchor_x + ddx);
        p->anchor_y = (int16_t)(p->anchor_y + ddy);
        ui_polyline_update_frame(p);
      }
      break;
    }
    case UI_TYPE_UNION:
    {
      ui_union_para_t* p = (ui_union_para_t*)head;
      uint16_t n = num, d = den;
      int16_t ddx, ddy;

      ui_scale_union(p, &n, &d);            /* 嵌套合并对象：先整体缩，再挪到目标中心 */
      ddx = (int16_t)(tcx - p->head.center_x);
      ddy = (int16_t)(tcy - p->head.center_y);
      if (ddx != 0 || ddy != 0)
      {
        ui_move_union(p, &ddx, &ddy);
      }
      break;
    }
    case UI_TYPE_VIEWPORT:
    {
      ui_viewport_para_t* p = (ui_viewport_para_t*)head;
      uint16_t n = num, d = den;
      int16_t ddx, ddy;

      ui_scale_viewport(p, &n, &d);         /* 嵌套视窗：先整体缩，再挪到目标中心 */
      ddx = (int16_t)(tcx - p->head.center_x);
      ddy = (int16_t)(tcy - p->head.center_y);
      if (ddx != 0 || ddy != 0)
      {
        ui_move_viewport(p, &ddx, &ddy);
      }
      break;
    }
    default:
      break;
  }
}

/* 合并对象整体缩放：绕独立锚点 anchor（不是框中心），把每个子对象从基准按绝对值比例 num / den 重设 ——
 *   位置 = 锚点 + 基准偏移 × num / den，尺寸 = 基准尺寸 × num / den。
 * 锚点只随平移走、缩放一个字都不动它，子对象位置全由 锚点 + 基准偏移×比例 现算：
 * 既不漂（不会因为框中心重推出来的 ±1px 取整误差被反复喂回去），
 * 也不用"把框中心钉回锚点"那种别扭写法。
 * 比例是绝对值、基准不动，逐帧反复设同一个比例也不累积（幂等）。
 * 线宽 / 边框宽 / 字号不缩；几何变完**重新量出子对象并集框**写进 center / w / h，
 * 脏区直接读它、和画出来的像素严格对齐，再按 老框 ∪ 新框 标脏。
 * 注意：这里的比例是绝对比例而非增量 —— 和折线一样，反复调同一个比例不累积，
 * 跟矩形 / 圆那些"按当前尺寸乘增量"的缩放不同 */
static uint8_t ui_scale_union(void* para, uint16_t* num, uint16_t* den)
{
  ui_union_para_t* p = (ui_union_para_t*)para;
  ui_merge_obj_node_t* node;
  int16_t ox0 = 0, oy0 = 0, ox1 = 0, oy1 = 0;
  int16_t x_min = 0, y_min = 0, x_max = 0, y_max = 0;
  int16_t ax, ay;
  uint8_t had_old, has = 0;

  if (num == NULL || den == NULL)
  {
    return 0;
  }
  if (p == NULL || *den == 0u)
  {
    *num = 0;
    return 0;
  }

  ax = p->anchor_x;           /* 缩放只绕独立锚点；缩放一个字都不动它，也不靠框中心重推 */
  ay = p->anchor_y;

  had_old = (uint8_t)(p->w > 0 && p->h > 0);
  if (had_old != 0u)
  {
    ui_box_of(p->head.center_x, p->head.center_y, p->w, p->h, &ox0, &oy0, &ox1, &oy1);
  }

  /* 从基准重设每个子对象，同时量出新并集框 */
  ListForEachEntry(node, &p->list, ui_merge_obj_node_t, node)
  {
    int16_t cx, cy, w, h, x0, y0, x1, y1;

    ui_union_scale_child(node, *num, *den, ax, ay);

    if (ui_cursor_target_frame(node->obj, 0, 0, &cx, &cy, &w, &h))
    {
      ui_box_of(cx, cy, w, h, &x0, &y0, &x1, &y1);

      if (has == 0u) { x_min = x0; x_max = x1; y_min = y0; y_max = y1; has = 1u; }
      else
      {
        if (x0 < x_min) { x_min = x0; }
        if (x1 > x_max) { x_max = x1; }
        if (y0 < y_min) { y_min = y0; }
        if (y1 > y_max) { y_max = y1; }
      }
    }
  }

  if (has != 0u)
  {
    p->w = (int16_t)(x_max - x_min + 1);
    p->h = (int16_t)(y_max - y_min + 1);
    /* 框中心 = 真实并集框中心：脏区 / 光标都读它，和画出来的像素严格对齐（不再"钉"回去） */
    p->head.center_x = (int16_t)(x_min + (p->w >> 1));
    p->head.center_y = (int16_t)(y_min + (p->h >> 1));
  }
  else
  {
    p->w = 0;
    p->h = 0;
  }

  if (p->w > 0 && p->h > 0)
  {
    int16_t nx0, ny0, nx1, ny1;

    ui_box_of(p->head.center_x, p->head.center_y, p->w, p->h, &nx0, &ny0, &nx1, &ny1);
    if (had_old != 0u)
    {
      ui_dirty_mark(ox0, oy0, ox1, oy1);
    }
    ui_dirty_mark(nx0, ny0, nx1, ny1);
  }
  return 1;
}

/* 视窗整体缩放：窗口尺寸 = 基准尺寸 × 因子，窗口中心绕独立锚点按因子走；
 * 子对象也从自带基准、绕同一个锚点按绝对比例缩放（和合并对象共用 ui_union_scale_child）。
 * 和合并对象的区别：窗口宽高是显式给定的"可见窗口"，不从子对象并集重新量 ——
 * 所以子对象在里面滚来滚去，窗口大小不动。
 * 比例是绝对值、基准不动，逐帧反复设不累积。老窗 ∪ 新窗标脏 */
static uint8_t ui_scale_viewport(void* para, uint16_t* num, uint16_t* den)
{
  ui_viewport_para_t* p = (ui_viewport_para_t*)para;
  ui_merge_obj_node_t* node;
  int16_t ox0 = 0, oy0 = 0, ox1 = 0, oy1 = 0;
  uint8_t had_old;

  if (num == NULL || den == NULL)
  {
    return 0;
  }
  if (p == NULL || *den == 0u)
  {
    *num = 0;
    return 0;
  }

  had_old = (uint8_t)(p->w > 0 && p->h > 0);
  if (had_old != 0u)
  {
    ui_box_of(p->head.center_x, p->head.center_y, p->w, p->h, &ox0, &oy0, &ox1, &oy1);
  }

  ListForEachEntry(node, &p->list, ui_merge_obj_node_t, node)
  {
    ui_union_scale_child(node, *num, *den, p->anchor_x, p->anchor_y);
  }

  /* 窗口尺寸 = 基准 × 因子；窗口中心 = 锚点 + (基准中心 - 锚点) × 因子 */
  p->w = (int16_t)((int32_t)p->base_w * (*num) / (*den));
  p->h = (int16_t)((int32_t)p->base_h * (*num) / (*den));
  if (p->w < 1) { p->w = 1; }
  if (p->h < 1) { p->h = 1; }
  p->head.center_x = (int16_t)((int32_t)p->anchor_x +
                     ((int32_t)p->base_cx - (int32_t)p->anchor_x) * (int32_t)(*num) / (int32_t)(*den));
  p->head.center_y = (int16_t)((int32_t)p->anchor_y +
                     ((int32_t)p->base_cy - (int32_t)p->anchor_y) * (int32_t)(*num) / (int32_t)(*den));

  if (p->w > 0 && p->h > 0)
  {
    int16_t nx0, ny0, nx1, ny1;

    ui_box_of(p->head.center_x, p->head.center_y, p->w, p->h, &nx0, &ny0, &nx1, &ny1);
    if (had_old != 0u)
    {
      ui_dirty_mark(ox0, oy0, ox1, oy1);
    }
    ui_dirty_mark(nx0, ny0, nx1, ny1);
  }
  return 1;
}

/* 进度条缩放：shape 感知 —— 圆环按 base_ext_r / base_int_r 缩放、矩形按 base_w / base_h 缩放，
 * 都是"新尺寸 = 基准 × 因子"，锚点不动、中心由 锚点 + 偏移 × 因子 现算。
 * 线宽 / 端点圆弧不随缩放改变（和圆 / 矩形口径一致）。
 * 老框 / 新框都整块标脏（进度条是实心块，不能只标边带）。 */
static uint8_t ui_scale_progress(void* para, uint16_t* num, uint16_t* den)
{
  ui_progress_common_t* p = (ui_progress_common_t*)para;
  int16_t ox0, oy0, ox1, oy1, nx0, ny0, nx1, ny1;

  if (num == NULL || den == NULL)
  {
    return 0;
  }
  if (p == NULL || *den == 0)
  {
    *num = 0;
    return 0;
  }

  ui_progress_box(p, &ox0, &oy0, &ox1, &oy1);

  /* 写因子；锚点不动，中心由 锚点 + 偏移 × 因子 现算 */
  p->xf.scale_num = *num;
  p->xf.scale_den = *den;

  if (p->shape == (uint8_t)UI_PROGRESS_SHAPE_CIRCLE)
  {
    ui_circle_progress_bar_para_t* c = (ui_circle_progress_bar_para_t*)p;

    c->internal_r = (int16_t)((int32_t)c->base_int_r * (*num) / (*den));
    c->external_r = (int16_t)((int32_t)c->base_ext_r * (*num) / (*den));
    if (c->external_r < 1) { c->external_r = 1; }   /* 外半径不许缩到 0，否则整个环消失 */
  }
  else
  {
    ui_rect_progress_bar_para_t* r = (ui_rect_progress_bar_para_t*)p;

    r->w = (int16_t)((int32_t)r->base_w * (*num) / (*den));
    r->h = (int16_t)((int32_t)r->base_h * (*num) / (*den));
    if (r->w < 1) { r->w = 1; }
    if (r->h < 1) { r->h = 1; }
  }

  ui_xf_sync_center(&p->head, &p->xf);

  ui_progress_box(p, &nx0, &ny0, &nx1, &ny1);
  ui_dirty_mark(ox0, oy0, ox1, oy1);
  ui_dirty_mark(nx0, ny0, nx1, ny1);

  return 1;
}

typedef uint8_t (*anim_scale_obj_fn_t)(void*, uint16_t*, uint16_t*);//缩放函数指针类型

static const anim_scale_obj_fn_t anim_scale_obj_tab[] =
{
  [UI_TYPE_NONE] = NULL,
  [UI_TYPE_RECT] = ui_scale_rect,
  [UI_TYPE_TEXT] = ui_scale_text,
  [UI_TYPE_CIRCLE] = ui_scale_circle,
  [UI_TYPE_CURSOR] = ui_scale_cursor,
  [UI_TYPE_POLYLINE] = ui_scale_polyline,
  [UI_TYPE_UNION] = ui_scale_union,
  [UI_TYPE_POINT] = ui_scale_point,
  [UI_TYPE_VIEWPORT] = ui_scale_viewport,
  [UI_TYPE_PROGRESS_BAR] = ui_scale_progress,  /* 圆环 / 矩形进度条：按 para 里的 shape 分派 */
};



void ui_anim_scale_obj(ui_object_t *obj, uint16_t num, uint16_t den)
{
  ui_type_t type = ui_obj_analysis_type(obj);
  anim_scale_obj_fn_t fn;
  uint16_t n = num, d = den;

  if (type == UI_TYPE_NONE)
  {
    return;
  }

  /* type 可能没赋过值甚至被写坏，先把下标夹在表内再查 */
  if ((unsigned)type >= sizeof(anim_scale_obj_tab) / sizeof(anim_scale_obj_tab[0]))
  {
    return;
  }

  fn = anim_scale_obj_tab[type];

  /* 表里没登记（NULL）：这个类型不支持缩放，什么都不动 */
  if (fn == NULL)
  {
    return;
  }

  fn(obj->para, &n, &d);
}


/* ui_anim 在框架那边的扩展身份：拿本文件一个静态对象的地址当 key，
 * 只有 ui_anim 自己认得出，外部看不见 */
static const uint8_t ui_anim_ext_tag = 0u;

/* 按对象把 ui_anim 的扩展取回来；没注册过返回 NULL */
static ui_anim_ext_t* ui_anim_ext_of(ui_object_t *obj)
{
  return (ui_anim_ext_t *)UI_GetObject_Ext(obj, &ui_anim_ext_tag);
}

/* 动画状态注册：把 ext 挂到 obj 上，并一次登记好这个对象要用的动画状态，
 * 之后 obj 才能用 ui_anim_move_obj_to / follow / ui_anim_transform_rect_cursor_continuous。
 * 重复注册 = 重置：先解开 ext 原来挂的对象，再把状态清零重新登记。
 * ext 为 NULL 什么都不做；obj 为 NULL = 只解绑 ext、不绑新对象。
 * 返回框架注册结果；失败则什么都没绑定 */
ui_reg_result_t ui_anim_register(ui_anim_ext_t *ext, ui_object_t *obj, ui_move_anim_t *move, ui_transform_anim_t *shape, ui_poly_scale_anim_t *poly_scale, ui_progress_anim_t *progress)
{
  if (ext == NULL)
  {
    return UI_REG_FAIL;
  }

  /* 先解开 ext 原来挂的对象，免得旧对象那边还当我们挂着 */
  UI_UnregisterObject_Ext(&ext->ext);

  if (obj == NULL)                       /* 只解绑，不绑新对象 */
  {
    ext->move       = NULL;
    ext->shape      = NULL;
    ext->poly_scale = NULL;
    ext->progress   = NULL;
    return UI_REG_OK;
  }

  ext->move       = move;                /* 这个对象要用的动画状态；不用的填 NULL */
  ext->shape      = shape;
  ext->poly_scale = poly_scale;
  ext->progress   = progress;

  ext->ext.key  = &ui_anim_ext_tag;      /* 报身份：取回扩展就靠它 */
  ext->ext.data = ext;                   /* 框架不解释，指回整个 ext 就行 */

  if (UI_RegisterObject_Ext(obj, &ext->ext) != UI_REG_OK)
  {
    ext->move       = NULL;
    ext->shape      = NULL;
    ext->poly_scale = NULL;
    ext->progress   = NULL;
    return UI_REG_FAIL;
  }

  /* 状态清零：进度由 ui_anim 从零记起 */
  if (move != NULL)
  {
    memset(move, 0, sizeof(ui_move_anim_t));
  }

  if (shape != NULL)
  {
    memset(shape, 0, sizeof(ui_transform_anim_t));
  }

  if (poly_scale != NULL)
  {
    memset(poly_scale, 0, sizeof(ui_poly_scale_anim_t));
  }

  if (progress != NULL)
  {
    memset(progress, 0, sizeof(ui_progress_anim_t));
  }

  return UI_REG_OK;
}

/* 这个对象注册过【平移】动画状态吗(1=注册过)。说明见 ui_anim.h */
uint8_t ui_anim_has_move(const ui_object_t *obj)
{
  const ui_anim_ext_t *ext;

  if (obj == NULL || obj->inited != 1)
  {
    return 0u;
  }

  /* ext 的 key = 本文件那个 ui_anim_ext_tag(UI_AnimRegister 里填的)，按它取回 */
  ext = (const ui_anim_ext_t *)UI_GetObject_Ext((ui_object_t *)obj, &ui_anim_ext_tag);
  return ((ext != NULL) && (ext->move != NULL)) ? 1u : 0u;
}

/* 折线连续缩放：在 time 毫秒内把折线缩放比例从当前值连续变到 num / den（锚点仍是起始点）。
 * 折线缩放是绝对比例，这里按 1ms 基准在 起点比例 与 终点比例 之间线性插值出这一步的比例，
 * 再交给 ui_anim_scale_obj 绝对写入 —— 逐帧反复调也不累积取整误差。
 * 同一目标/时长重复调就是"接着走"；换了目标 / 时长则用当前比例重新起跑。
 * 前提：对象已用 ui_anim_register 注册过 poly_scale 状态；没注册什么都不做 */
void ui_anim_scale_polyline_continuous(ui_object_t *obj, uint16_t num, uint16_t den, uint32_t time)
{
  ui_head_t *head;
  ui_anim_ext_t *ext;
  ui_poly_scale_anim_t *anim;
  ui_polyline_para_t *p;
  uint32_t elapsed;
  uint16_t cur_num, cur_den;

  if (obj == NULL || obj->para == NULL || time == 0u || den == 0u)
  {
    return;
  }

  head = (ui_head_t *)obj->para;
  if (head->type != UI_TYPE_POLYLINE)
  {
    return;
  }

  ext  = ui_anim_ext_of(obj);
  anim = (ext != NULL) ? ext->poly_scale : NULL;
  if (anim == NULL)
  {
    return;
  }

  p = (ui_polyline_para_t *)head;

  /* 没在跑，或目标/时长变了：用当前比例重新起跑一段 */
  if (anim->active == 0u ||
      anim->target_num != num || anim->target_den != den ||
      anim->duration != time)
  {
    anim->start_num  = p->scale_num;
    anim->start_den  = (p->scale_den != 0u) ? p->scale_den : 1u;
    anim->target_num = num;
    anim->target_den = den;
    anim->start_ms   = ui_anim_ms;
    anim->duration   = time;
    anim->active     = 1u;
  }

  elapsed = ui_anim_ms - anim->start_ms;
  if (elapsed >= anim->duration)
  {
    elapsed = anim->duration;
    anim->active = 0u;
  }

  cur_num = (uint16_t)((int32_t)anim->start_num +
            ((int32_t)anim->target_num - (int32_t)anim->start_num) * (int32_t)elapsed / (int32_t)anim->duration);
  cur_den = (uint16_t)((int32_t)anim->start_den +
            ((int32_t)anim->target_den - (int32_t)anim->start_den) * (int32_t)elapsed / (int32_t)anim->duration);
  if (cur_den == 0u)
  {
    cur_den = 1u;
  }

  ui_anim_scale_obj(obj, cur_num, cur_den);
}

/* 进度条连续动画：在 time 毫秒内把进度从当前值连续变到 target_value（圆环 / 矩形通用）。
 * 进度是绝对当前值，这里按 1ms 基准在 起点进度 与 终点进度 之间线性插值，
 * 再交给 ui_progress_set_value 绝对写入 para->progress(并按外框整块标脏) ——
 * 逐帧反复调也不累积取整误差。
 * 同一目标/时长重复调就是"接着走"；换了目标 / 时长则用当前进度重新起跑一段。
 * 目标超过 max_value 时按 max_value 夹住。
 * 前提：对象已用 ui_anim_register 注册过 progress 状态；没注册什么都不做 */
void ui_anim_progress_bar_continuous(ui_object_t *obj, uint16_t target_value, uint32_t time)
{
  ui_head_t *head;
  ui_anim_ext_t *ext;
  ui_progress_anim_t *anim;
  ui_progress_common_t *p;
  uint32_t elapsed;
  uint16_t target;
  uint16_t cur;

  if (obj == NULL || obj->para == NULL || time == 0u)
  {
    return;
  }

  head = (ui_head_t *)obj->para;
  if (head->type != UI_TYPE_PROGRESS_BAR)
  {
    return;
  }

  ext  = ui_anim_ext_of(obj);
  anim = (ext != NULL) ? ext->progress : NULL;
  if (anim == NULL)
  {
    return;
  }

  p = (ui_progress_common_t *)head;

  /* 目标夹到 [0, max_value]：圆环 / 矩形 max_value 字段偏移相同，都紧跟在 progress 后面 */
  target = target_value;
  {
    uint16_t maxv = (p->shape == (uint8_t)UI_PROGRESS_SHAPE_CIRCLE)
                  ? ((ui_circle_progress_bar_para_t*)p)->max_value
                  : ((ui_rect_progress_bar_para_t*)p)->max_value;
    if (maxv != 0u && target > maxv)
    {
      target = maxv;
    }
  }

  /* 没在跑，或目标/时长变了：用当前进度重新起跑一段 */
  if (anim->active == 0u || anim->target_value != target || anim->duration != time)
  {
    anim->start_value  = ui_progress_get_value(p);
    anim->target_value = target;
    anim->start_ms     = ui_anim_ms;
    anim->duration     = time;
    anim->active       = 1u;
  }

  elapsed = ui_anim_ms - anim->start_ms;
  if (elapsed >= anim->duration)
  {
    elapsed = anim->duration;
    anim->active = 0u;
  }

  cur = (uint16_t)((int32_t)anim->start_value +
        ((int32_t)anim->target_value - (int32_t)anim->start_value) * (int32_t)elapsed / (int32_t)anim->duration);

  ui_progress_set_value(p, cur);
}

/* 单轴缓动求值：把 start→target 的直线进度按 ease 曲线重映射，返回 elapsed 时刻的坐标。
 * elapsed 由调用者夹到 [0, duration]；duration 为 0 直接取终点。
 * ease：0 线性；> 0 easeOutBack 超调回弹；< 0 easeOut 减速缓进。定点移动和跟随共用这一套。 */
static int16_t ui_move_ease_pos(int16_t start, int16_t target,
                                uint32_t elapsed, uint32_t duration, int16_t ease)
{
  int32_t d = (int32_t)target - (int32_t)start;

  if (duration == 0u || elapsed >= duration)
  {
    /* 到点直接贴终点，正好也是超调曲线的落点（f(1)=1），回正干净 */
    return target;
  }

  if (ease == 0)
  {
    /* 线性插值：pos = start + (target - start) * elapsed / duration */
    return (int16_t)((int32_t)start + d * (int32_t)elapsed / (int32_t)duration);
  }

  if (ease > 0)
  {
    /* 超调回弹（easeOutBack）：进度归一化到 0..1024(Q10)，算缓动值 f。
     * f(0)=0、f(1)=1，中途冲过 1（超调）再落回 1，于是"超过终点一点再回来"。
     * f = 1 + c3*u^3 + c1*u^2，u = t-1，c3 = c1+1；c1 越大超调越猛，由强度换算而来 */
    int32_t t_q  = (int32_t)(elapsed * 1024u / duration);   /* 0..1024 */
    int32_t u_q  = t_q - 1024;                              /* -1024..0 */
    int32_t u2_q = (u_q * u_q) / 1024;
    int32_t u3_q = (u2_q * u_q) / 1024;
    int32_t c1_q = (int32_t)ease * 174;                     /* 强度 10 -> 约 1.70(Q10) */
    int32_t c3_q = c1_q + 1024;
    int32_t f_q  = 1024 + (c3_q * u3_q) / 1024 + (c1_q * u2_q) / 1024;

    return (int16_t)((int32_t)start + d * f_q / 1024);
  }
  else
  {
    /* 减速缓进（easeOut）：从头一路减速到终点，全程不超调。
     * 进度归一化到 0..1024(Q10)，f = 1-(1-t)^2 = 2t - t^2：
     * 起步速度是均速的 2 倍，之后一路变慢，快到终点时趋于 0（经典 easeOutQuad）。
     * ease 只用来"选档"（取负即可），数值大小不再影响曲线。 */
    int32_t t_q = (int32_t)(elapsed * 1024u / duration);   /* 0..1023 */
    int32_t u_q = 1024 - t_q;                              /* (1-t) */
    int32_t f_q = 1024 - (u_q * u_q) / 1024;               /* 1-(1-t)^2 */

    return (int16_t)((int32_t)start + d * f_q / 1024);
  }
}

/* 定点移动：让对象在 time 毫秒内从当前中心连续走到 (target_x, target_y)。
 * 非阻塞：每帧从主循环调一次，自己按 ui_anim_ms 算进度、挪一小步，脏区由 ui_anim_move_obj 记。
 * 同一目标重复调就是"接着走"（无副作用）；目标一变就是换了目的地，从当前中心重新起跑一段。
 * ease 是缓动选择：0 线性走到底；> 0 走到底时先冲过终点一点再回正（easeOutBack 超调回弹），
 *   数值即强度；< 0 easeOut 减速缓进，从头一路减速到终点（二次缓出），全程不超调，
 *   数值只用来选档（取负即可）。
 * 换了时长 / 换了缓动选择 / 上一段是跟随档，都重新起跑（对象是注册死的，不会换）。
 * 对象得先用 ui_anim_register 注册过平移状态，这里才认；没注册直接返回。
 * time 为 0、obj/para 为空直接返回。前提：初始化时调过 ui_anim_tick()，ui_anim_ms 才会走 */
void ui_anim_move_obj_to(ui_object_t *obj, int16_t target_x, int16_t target_y, uint32_t time, int16_t ease)
{
  ui_head_t *head;
  ui_anim_ext_t *ext;
  ui_move_anim_t *slot;
  uint32_t elapsed;
  uint8_t  restart;
  int16_t want_x, want_y, dx, dy;

  if (obj == NULL || obj->para == NULL || time == 0u)
  {
    return;
  }
  head = (ui_head_t *)obj->para;

  /* 状态从对象的扩展里取；没注册过（或没挂平移状态）就不是我们的活 */
  ext  = ui_anim_ext_of(obj);
  slot = (ext != NULL) ? ext->move : NULL;
  if (slot == NULL)
  {
    return;
  }

  /* 缓动强度封顶：换算成缓动系数时别把中间量顶出 32 位 */
  if (ease > 30)
  {
    ease = 30;
  }
  else if (ease < -30)
  {
    ease = -30;
  }

  /* 该不该为这次调用起跑新的一段：
   *   换了时长 / 换了缓动选择 / 上一段是跟随档 —— 该；
   *   定点档位且目标变了                        —— 该：换目的地了，重新起跑一段；
   *   目标没变                                  —— 不该：接着走原来那段 */
  if (slot->duration != time || slot->follow != 0u ||
      (int16_t)slot->ease != ease)
  {
    restart = 1u;
  }
  else if (slot->target_x != target_x || slot->target_y != target_y)
  {
    restart = 1u;
  }
  else
  {
    restart = 0u;
  }

  /* 起跑新轨迹：以当前中心为出发点，重新记 0 时刻 */
  if (restart != 0u)
  {
    slot->follow   = 0u;
    slot->ease     = (uint16_t)ease;
    slot->start_x  = head->center_x;
    slot->start_y  = head->center_y;
    slot->target_x = target_x;
    slot->target_y = target_y;
    slot->start_ms = ui_anim_ms;
    slot->duration = time;
  }

  /* 已流逝时间；无符号减法，ui_anim_ms 回绕也安全。到点就夹到总时长，于是位置正好贴在终点上 */
  elapsed = ui_anim_ms - slot->start_ms;
  if (elapsed >= slot->duration)
  {
    elapsed = slot->duration;
  }

  want_x = ui_move_ease_pos(slot->start_x, slot->target_x, elapsed, slot->duration, (int16_t)slot->ease);
  want_y = ui_move_ease_pos(slot->start_y, slot->target_y, elapsed, slot->duration, (int16_t)slot->ease);

  /* 只走这一帧要走的差值，ui_anim_move_obj 内部会算旧框/新框并记脏区 */
  dx = (int16_t)(want_x - head->center_x);
  dy = (int16_t)(want_y - head->center_y);
  if (dx != 0 || dy != 0)
  {
    ui_anim_move_obj(obj, dx, dy);
  }
}

/* 跟随移动：让对象咬住一个会连续移动的目标（比如横向往复的文本）。
 * 非阻塞：每帧从主循环调一次。
 * 起跑后按 time 毫秒用 ease 缓动追上目标 —— 和其他切换共用同一套缓动，观感一致；
 * 到点后 elapsed 夹在 time，want 正好落在目标上，之后每帧锁定实时目标、精确贴住不再拖尾。
 * 追的过程中目标每帧在动时，把整条轨迹平移同样的量，于是不掉队也不重置进度。
 * ease 是缓动选择（三种，只能选一种）：
 *   0 = 线性匀速；> 0 = easeOutBack 超调回弹（数值即强度）；
 *   < 0 = easeOut 减速缓进，从头一路减速到终点（二次缓出），数值只用来选档（取负即可）。
 * 目标位置本身必须是连续的；目标会跳变时请改用 ui_anim_move_obj_to。
 * 换了时长 / 换了缓动选择 / 上一段是定点档，都重新起跑（对象是注册死的，不会换）。
 * 对象得先用 ui_anim_register 注册过平移状态，这里才认；没注册直接返回。
 * time 为 0、obj/para 为空直接返回。前提：初始化时调过 ui_anim_tick()，ui_anim_ms 才会走 */
void ui_anim_move_obj_follow(ui_object_t *obj, int16_t target_x, int16_t target_y, uint32_t time, int16_t ease)
{
  ui_head_t *head;
  ui_anim_ext_t *ext;
  ui_move_anim_t *slot;
  uint32_t elapsed;
  uint8_t  restart;
  int16_t want_x, want_y, dx, dy;

  if (obj == NULL || obj->para == NULL || time == 0u)
  {
    return;
  }
  head = (ui_head_t *)obj->para;

  /* 状态从对象的扩展里取；没注册过（或没挂平移状态）就不是我们的活 */
  ext  = ui_anim_ext_of(obj);
  slot = (ext != NULL) ? ext->move : NULL;
  if (slot == NULL)
  {
    return;
  }

  /* 缓动强度封顶：换算成缓动系数时别把中间量顶出 32 位 */
  if (ease > 30)
  {
    ease = 30;
  }
  else if (ease < -30)
  {
    ease = -30;
  }

  /* 该不该为这次调用起跑新的一段：
   *   换了时长 / 换了缓动选择 / 上一段是定点档 —— 该；
   *   目标变了                                   —— 不该：把整条轨迹平移过去跟住它；
   *   目标没变                                   —— 不该：接着走原来那段 */
  if (slot->duration != time || slot->follow != 1u ||
      (int16_t)slot->ease != ease)
  {
    restart = 1u;
  }
  else
  {
    restart = 0u;
  }

  if (restart != 0u)
  {
    /* 起跑新的一段：以当前中心为出发点，重新记 0 时刻 */
    slot->follow   = 1u;
    slot->ease     = (uint16_t)ease;
    slot->start_x  = head->center_x;
    slot->start_y  = head->center_y;
    slot->target_x = target_x;
    slot->target_y = target_y;
    slot->start_ms = ui_anim_ms;
    slot->duration = time;
  }
  else if (slot->target_x != target_x || slot->target_y != target_y)
  {
    /* 目标在动：出发点和终点一起平移同样的量，整条轨迹跟着挪，
     * 于是既跟上目标的移动，又保留这段缓动的进度（间距 = 起点间距 × (1 - f)） */
    slot->start_x  = (int16_t)(slot->start_x + (int16_t)(target_x - slot->target_x));
    slot->start_y  = (int16_t)(slot->start_y + (int16_t)(target_y - slot->target_y));
    slot->target_x = target_x;
    slot->target_y = target_y;
  }

  /* 已流逝时间；到点夹到总时长，于是位置正好贴在目标上，之后每帧锁定实时目标 */
  elapsed = ui_anim_ms - slot->start_ms;
  if (elapsed >= slot->duration)
  {
    elapsed = slot->duration;
  }

  want_x = ui_move_ease_pos(slot->start_x, slot->target_x, elapsed, slot->duration, (int16_t)slot->ease);
  want_y = ui_move_ease_pos(slot->start_y, slot->target_y, elapsed, slot->duration, (int16_t)slot->ease);

  /* 只走这一帧要走的差值，ui_anim_move_obj 内部会算旧框/新框并记脏区 */
  dx = (int16_t)(want_x - head->center_x);
  dy = (int16_t)(want_y - head->center_y);
  if (dx != 0 || dy != 0)
  {
    ui_anim_move_obj(obj, dx, dy);
  }
}


/* 由 中心 + 宽高 反推四边用公共的 ui_box_of（见 ui_create.h），与 ui_draw_rect 同约定 */
void ui_anim_transform_rect_cursor(ui_object_t *obj, int16_t w, int16_t h, int16_t border_width, ui_color_t color, int16_t gap, ui_type_t type)
{
  ui_head_t *head;
  dirty_rect_t old_rect = {0};
  dirty_rect_t new_rect = {0};
  int16_t old_w, old_h;

  if (obj == NULL || obj->para == NULL)
  {
    return;
  }
  head = (ui_head_t *)obj->para;

  /* 只处理矩形和光标，且对象实际类型要和调用者声明的 type 对得上 */
  if (head->type != type)
  {
    return;
  }
  if (type != UI_TYPE_RECT && type != UI_TYPE_CURSOR)
  {
    return;
  }

  if (w < 1) { w = 1; }
  if (h < 1) { h = 1; }

  /* 先把旧框取出来（边界按 ui_draw_rect 那套算，描边带才能逐像素对齐） */
  old_w = (type == UI_TYPE_CURSOR) ? ((ui_cursor_para_t *)head)->w : ((ui_rect_para_t *)head)->w;
  old_h = (type == UI_TYPE_CURSOR) ? ((ui_cursor_para_t *)head)->h : ((ui_rect_para_t *)head)->h;
  ui_box_of(head->center_x, head->center_y, old_w, old_h,
            &old_rect.x_min, &old_rect.y_min, &old_rect.x_max, &old_rect.y_max);

  /* 赋新参数：方框尺寸 + 线宽 + 颜色（光标还要内缘间距） */
  if (type == UI_TYPE_CURSOR)
  {
    ui_cursor_para_t *p = (ui_cursor_para_t *)head;

    p->w            = w;
    p->h            = h;
    p->gap          = gap;
    p->border_width = (uint16_t)border_width;
    p->color        = color;
    /* 光标没有变换块 xf，没有锚点可言，中心不动 */
  }
  else
  {
    ui_rect_para_t *p = (ui_rect_para_t *)head;
    ui_xform_t     *xf = &p->xf;

    p->w            = w;
    p->h            = h;
    p->border_width = border_width;
    p->color        = color;

    /* 绕【对象自带的锚点】(ui_obj_set_anchor 设过的话)：
     * 把锚点当成盒子上的一点，它到四边的"点距"随尺寸等比缩放（点距用 (尺寸-1) 作分母）。
     * 这样锚点恰好落在某条边上时（比如下边中点），那条边【严格不动】，
     * 和尺寸的奇偶无关 —— 不会出现"偶数高底边在 239、奇数高在 240"那种 1px 抖动。
     * 结果只跟当前尺寸有关，反复调幂等、不累积漂移。
     * 没设过锚点（anchor_set == 0）就中心不动，与旧行为一致 */
    if (xf->anchor_set != 0u)
    {
      int32_t span_old_x = (int32_t)old_w - 1;   /* 旧盒子的"点宽/点高" */
      int32_t span_old_y = (int32_t)old_h - 1;
      int32_t span_new_x = (int32_t)w - 1;       /* 新盒子的"点宽/点高" */
      int32_t span_new_y = (int32_t)h - 1;
      int32_t d_left, d_top, x_min_new, y_min_new;

      /* 锚点到左/上边的"点距"是等比缩放的基准量：点距随尺寸等比缩放，锚点才一直落在同一条边上。
       * 【退化】旧框宽/高 == 1 时，盒子的上边和下边(左/右边)重合成一行，点距恒为 0——
       * 从当前框已经看不出锚点原来在哪条边上。这时改用【基准框】(形变前 bake 下来的正常几何)
       * 那一对值：点距和点高一起取自基准，比例才准。
       * 不这么兜的话，退化后点距变 0、上边被钉死在锚点上，再放大只会【往下长】，永远长不回去
       * (底边锚点的矩形尤其明显：一退化成 1px，之后就再也不会往上长) */
      if (span_old_x >= 1)
      {
        d_left = (int32_t)xf->anchor_x - (int32_t)old_rect.x_min;
      }
      else
      {
        span_old_x = (int32_t)p->base_w - 1;                            /* 基准框的"点宽" */
        d_left     = (int32_t)xf->base_anchor_x
                   - ((int32_t)xf->base_cx - ((int32_t)p->base_w >> 1));
        if (span_old_x < 1) { span_old_x = 1; d_left = 0; }             /* 基准也退化：退回"中心不动" */
      }

      if (span_old_y >= 1)
      {
        d_top = (int32_t)xf->anchor_y - (int32_t)old_rect.y_min;
      }
      else
      {
        span_old_y = (int32_t)p->base_h - 1;                            /* 基准框的"点高" */
        d_top      = (int32_t)xf->base_anchor_y
                   - ((int32_t)xf->base_cy - ((int32_t)p->base_h >> 1));
        if (span_old_y < 1) { span_old_y = 1; d_top = 0; }              /* 基准也退化：退回"中心不动" */
      }

      x_min_new = (int32_t)xf->anchor_x - d_left * span_new_x / span_old_x;
      y_min_new = (int32_t)xf->anchor_y - d_top  * span_new_y / span_old_y;

      /* 由新框左上角反推中心（与画法约定一致：x_min = 中心 - w/2） */
      head->center_x = (int16_t)(x_min_new + (w >> 1));
      head->center_y = (int16_t)(y_min_new + (h >> 1));

      /* 形变只改"当前几何"：把"锚点 → 基准中心"的偏移同步刷新，
       * 让"中心 = 锚点 + 偏移 × 因子"这个不变量继续成立 ——
       * 不同步的话，下一次平移 / 缩放会按旧偏移把中心拉回形变前的位置(底边就飘了)。
       * base_*（确定态）不动：形变属于未确定态，复位照样回到形变前的那份几何 */
      xf->anchor_off_x = (int16_t)(head->center_x - xf->anchor_x);
      xf->anchor_off_y = (int16_t)(head->center_y - xf->anchor_y);
    }
  }

  /* 新框：按新中心 + 新宽高重算 */
  ui_box_of(head->center_x, head->center_y, w, h,
            &new_rect.x_min, &new_rect.y_min, &new_rect.x_max, &new_rect.y_max);

  /* old / new 两个框一起登记：描边矩形 / 光标标四条边带，填充矩形标整框 */
  {
    uint8_t filled = 0;
    if (type == UI_TYPE_RECT)
    {
      filled = ((ui_rect_para_t *)head)->fill;
    }
    ui_dirty_mark_rect_move(old_rect.x_min, old_rect.y_min, old_rect.x_max, old_rect.y_max,
                            new_rect.x_min, new_rect.y_min, new_rect.x_max, new_rect.y_max,
                            border_width, filled);
  }
}


/* 让矩形/光标在 time 毫秒内连续形变到目标尺寸/线宽/颜色/间距。
 * 非阻塞：每帧从主循环调一次，函数自己按 ui_anim_ms 算进度、插值出一小步，
 * 再调 ui_anim_transform_rect_cursor 落地，旧框/新框与脏区由它记。
 * 同一目标/时长的重复调用就是"接着走"；换了目标 / 时长则用当前参数重新起跑。
 * 对象得先用 ui_anim_register 注册过形变状态，这里才认；没注册直接返回。
 * time 为 0、obj/para 为空直接返回。
 * 前提：初始化时调过 ui_anim_tick()，ui_anim_ms 才会走 */
void ui_anim_transform_rect_cursor_continuous(ui_object_t *obj, int16_t w, int16_t h, int16_t border_width, ui_color_t color, int16_t gap, ui_type_t type, uint32_t time, int16_t ease)
{
  ui_head_t *head;
  ui_anim_ext_t *ext;
  ui_transform_anim_t *anim;
  uint32_t elapsed;
  int16_t cur_w, cur_h, cur_border_width, cur_gap;
  ui_color_t cur_color;

  if (obj == NULL || obj->para == NULL || time == 0u)
  {
    return;
  }
  head = (ui_head_t *)obj->para;

  /* 状态从对象的扩展里取；没注册过（或没挂形变状态）就不是我们的活 */
  ext  = ui_anim_ext_of(obj);
  anim = (ext != NULL) ? ext->shape : NULL;
  if (anim == NULL)
  {
    return;
  }

  /* 只处理矩形和光标，且对象实际类型要和调用者声明的 type 对得上 */
  if (head->type != type)
  {
    return;
  }
  if (type != UI_TYPE_RECT && type != UI_TYPE_CURSOR)
  {
    return;
  }

  /* 缓动强度封顶：换算成缓动系数时别把中间量顶出 32 位 */
  if (ease > 30)       { ease = 30; }
  else if (ease < -30) { ease = -30; }

  /* 不是同一次动画（换目标 / 换时长 / 换缓动 / 上一段是跟随档）就用当前参数重新起跑 */
  if (anim->active == 0u ||
      anim->follow != 0u ||
      anim->target_w != w || anim->target_h != h ||
      anim->target_border_width != border_width ||
      anim->target_color != color ||
      anim->target_gap != gap ||
      anim->duration != time ||
      anim->ease != ease)
  {
    anim->target_w            = w;
    anim->target_h            = h;
    anim->target_border_width = border_width;
    anim->target_color        = color;
    anim->target_gap          = gap;
    anim->start_ms            = ui_anim_ms;
    anim->duration            = time;
    anim->ease                = ease;
    anim->active              = 1u;
    anim->follow              = 0u;

    /* 起点 = 此刻对象自身的参数 */
    if (type == UI_TYPE_CURSOR)
    {
      ui_cursor_para_t *p = (ui_cursor_para_t *)head;
      anim->start_w            = p->w;
      anim->start_h            = p->h;
      anim->start_border_width = (int16_t)p->border_width;
      anim->start_color        = p->color;
      anim->start_gap          = p->gap;
    }
    else
    {
      ui_rect_para_t *p = (ui_rect_para_t *)head;
      anim->start_w            = p->w;
      anim->start_h            = p->h;
      anim->start_border_width = p->border_width;
      anim->start_color        = p->color;
      anim->start_gap          = 0;
    }
  }

  /* 已流逝时间；无符号减法，ui_anim_ms 回绕也安全。到点就夹到总时长并收尾 */
  elapsed = ui_anim_ms - anim->start_ms;
  if (elapsed >= anim->duration)
  {
    elapsed      = anim->duration;
    anim->active = 0u;
  }

  /* 按 ease 曲线插值：和平移动画同一套缓动，于是中心动 / 尺寸动两条动画可以同进度 */
  cur_w = ui_move_ease_pos(anim->start_w, anim->target_w, elapsed, anim->duration, anim->ease);
  cur_h = ui_move_ease_pos(anim->start_h, anim->target_h, elapsed, anim->duration, anim->ease);
  cur_border_width = ui_move_ease_pos(anim->start_border_width, anim->target_border_width, elapsed, anim->duration, anim->ease);
  cur_color = ui_color_lerp(anim->start_color, anim->target_color, (uint16_t)elapsed, (uint16_t)anim->duration);
  cur_gap = ui_move_ease_pos(anim->start_gap, anim->target_gap, elapsed, anim->duration, anim->ease);

  /* 用插值出的这一帧参数落地：旧框/新框与脏区由 ui_anim_transform_rect_cursor 记
   * （对象设过锚点就绕锚点走，没设过就中心不动） */
  ui_anim_transform_rect_cursor(obj, cur_w, cur_h, cur_border_width, cur_color, cur_gap, type);
}

/* 跟随形变（声明与用法见 ui_anim.h）：
 * 和定点档一样按 1ms 基准插值，区别是目标在变时把整条轨迹（起点 + 终点）平移、进度不重置，
 * 于是能咬住"尺寸一直在变"的目标（比如往复缩放的矩形外框），不会每帧重跑而掉队。
 * 到点后 elapsed 夹在总时长，插值结果正好落在实时目标上，之后每帧精确贴住。
 * 对象得先用 ui_anim_register 注册过形变状态，这里才认；没注册直接返回。
 * time 为 0、obj/para 为空直接返回。前提：初始化时调过 ui_anim_tick()，ui_anim_ms 才会走 */
void ui_anim_transform_rect_cursor_follow(ui_object_t *obj, int16_t w, int16_t h, int16_t border_width, ui_color_t color, int16_t gap, ui_type_t type, uint32_t time, int16_t ease)
{
  ui_head_t *head;
  ui_anim_ext_t *ext;
  ui_transform_anim_t *anim;
  uint32_t elapsed;
  int16_t cur_w, cur_h, cur_border_width, cur_gap;
  ui_color_t cur_color;

  if (obj == NULL || obj->para == NULL || time == 0u)
  {
    return;
  }
  head = (ui_head_t *)obj->para;

  /* 状态从对象的扩展里取；没注册过（或没挂形变状态）就不是我们的活 */
  ext  = ui_anim_ext_of(obj);
  anim = (ext != NULL) ? ext->shape : NULL;
  if (anim == NULL)
  {
    return;
  }

  /* 只处理矩形和光标，且对象实际类型要和调用者声明的 type 对得上 */
  if (head->type != type)
  {
    return;
  }
  if (type != UI_TYPE_RECT && type != UI_TYPE_CURSOR)
  {
    return;
  }

  /* 缓动强度封顶：换算成缓动系数时别把中间量顶出 32 位 */
  if (ease > 30)       { ease = 30; }
  else if (ease < -30) { ease = -30; }

  /* 不是同一段跟随（换时长 / 换缓动 / 上一段是定点档）就用当前参数重新起跑 */
  if (anim->follow != 1u || anim->duration != time || anim->ease != ease)
  {
    anim->follow   = 1u;
    anim->active   = 1u;
    anim->start_ms = ui_anim_ms;
    anim->duration = time;
    anim->ease     = ease;

    /* 起点 = 此刻对象自身的参数 */
    if (type == UI_TYPE_CURSOR)
    {
      ui_cursor_para_t *p = (ui_cursor_para_t *)head;
      anim->start_w            = p->w;
      anim->start_h            = p->h;
      anim->start_border_width = (int16_t)p->border_width;
      anim->start_color        = p->color;
      anim->start_gap          = p->gap;
    }
    else
    {
      ui_rect_para_t *p = (ui_rect_para_t *)head;
      anim->start_w            = p->w;
      anim->start_h            = p->h;
      anim->start_border_width = p->border_width;
      anim->start_color        = p->color;
      anim->start_gap          = 0;
    }

    anim->target_w            = w;
    anim->target_h            = h;
    anim->target_border_width = border_width;
    anim->target_color        = color;
    anim->target_gap          = gap;
  }
  else if (anim->target_w != w || anim->target_h != h ||
           anim->target_border_width != border_width ||
           anim->target_color != color ||
           anim->target_gap != gap)
  {
    /* 目标在变：起点和终点一起平移同样的量，整条轨迹跟着挪。
     * 起点到目标的间距 = 原间距 × (1 - 进度)，进度不变，于是既不重置也不掉队 */
    int16_t dw = (int16_t)(w - anim->target_w);
    int16_t dh = (int16_t)(h - anim->target_h);
    int16_t db = (int16_t)(border_width - anim->target_border_width);
    int32_t dc = (int32_t)color - (int32_t)anim->target_color;
    int16_t dg = (int16_t)(gap - anim->target_gap);

    anim->start_w            = (int16_t)(anim->start_w + dw);
    anim->start_h            = (int16_t)(anim->start_h + dh);
    anim->start_border_width = (int16_t)(anim->start_border_width + db);
    anim->start_color        = (ui_color_t)((int32_t)anim->start_color + dc);
    anim->start_gap          = (int16_t)(anim->start_gap + dg);

    anim->target_w            = w;
    anim->target_h            = h;
    anim->target_border_width = border_width;
    anim->target_color        = color;
    anim->target_gap          = gap;
  }
  /* 目标没变：接着走原来那段 */

  /* 已流逝时间；无符号减法，ui_anim_ms 回绕也安全。到点就夹到总时长 */
  elapsed = ui_anim_ms - anim->start_ms;
  if (elapsed >= anim->duration)
  {
    elapsed = anim->duration;
  }

  /* 按 ease 曲线插值：和平移动画同一套缓动，于是中心动 / 尺寸动两条动画可以同进度 */
  cur_w = ui_move_ease_pos(anim->start_w, anim->target_w, elapsed, anim->duration, anim->ease);
  cur_h = ui_move_ease_pos(anim->start_h, anim->target_h, elapsed, anim->duration, anim->ease);
  cur_border_width = ui_move_ease_pos(anim->start_border_width, anim->target_border_width, elapsed, anim->duration, anim->ease);
  cur_color = ui_color_lerp(anim->start_color, anim->target_color, (uint16_t)elapsed, (uint16_t)anim->duration);
  cur_gap = ui_move_ease_pos(anim->start_gap, anim->target_gap, elapsed, anim->duration, anim->ease);

  /* 用插值出的这一帧参数落地：旧框/新框与脏区由 ui_anim_transform_rect_cursor 记
   * （对象设过锚点就绕锚点走，没设过就中心不动） */
  ui_anim_transform_rect_cursor(obj, cur_w, cur_h, cur_border_width, cur_color, cur_gap, type);
}


