#include "./ui_control/ui_control.h"

/* 对象控制：缩放锚点登记 / 确认更新（bake）/ 复位。
 * 语义与字段定义见 ui_control.h 与 ui_create.h 里的 ui_xform_t。 */

/* 前置声明：对象外接框查询（实现在文件末尾的"外接框查询"段）——
 * 视窗 bake / append 要先它拿子对象当前几何；也对外给组件用（见 ui_control.h） */
uint8_t ui_obj_frame(ui_object_t* obj, int16_t* cx, int16_t* cy, int16_t* w, int16_t* h);

/* ---- 内部工具 ------------------------------------------------------------ */

/* 给单体对象的变换块记下锚点与"锚点 → 当前中心"偏移。
 * 锚点同时备份进确定态(base_anchor)：复位要连锚点一起回退 */
static void ui_xf_set_anchor(ui_xform_t* xf, ui_head_t* head, int16_t ax, int16_t ay)
{
  xf->anchor_x = ax;
  xf->anchor_y = ay;
  xf->anchor_off_x = (int16_t)(head->center_x - ax);
  xf->anchor_off_y = (int16_t)(head->center_y - ay);
  xf->base_anchor_x = ax;
  xf->base_anchor_y = ay;
  xf->anchor_set = 1u;    /* 标记"设过锚点"：平移 / 缩放 / 形变才知道要绕它，而不是按"中心不动" */
}

/* 文本用"中心 ± w/2"这套记脏（跟它 move / scale 的记法一致） */
static void ui_ctrl_mark_box_sym(int16_t cx, int16_t cy, int16_t w, int16_t h)
{
  ui_dirty_mark((int16_t)(cx - (w >> 1)), (int16_t)(cy - (h >> 1)),
                (int16_t)(cx + (w >> 1)), (int16_t)(cy + (h >> 1)));
}

/* 圆：中心 ± 外半径 */
static void ui_ctrl_mark_radius(int16_t cx, int16_t cy, int16_t r)
{
  ui_dirty_mark((int16_t)(cx - r), (int16_t)(cy - r), (int16_t)(cx + r), (int16_t)(cy + r));
}

/* 进度条当前外接框（四边，含端点）：圆环按 2r+1、矩形按 w×h，中心取 head。
 * 复位 / 平移 / 缩放记脏都读它，口径和下面的 ui_progress_bounds 一致 */
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

/* 合并对象：把当前几何定为新的基准 —— 每个子对象 bake 一次（让它自带基准 = 当前几何），
 * 再重算子对象相对 union 锚点的位置基准。锚点不动。
 * 尺寸不在这里存：缩放直接从子对象自带基准现算 */
static void ui_union_bake(ui_union_para_t* p)
{
  ui_merge_obj_node_t* node;

  if (p == NULL)
  {
    return;
  }

  ListForEachEntry(node, &p->list, ui_merge_obj_node_t, node)
  {
    int16_t cx, cy, w, h;

    ui_obj_bake(node->obj);              /* 子对象自带基准 = 当前几何 */
    if (ui_cursor_target_frame(node->obj, 0, 0, &cx, &cy, &w, &h))
    {
      node->base_rel_x = (int16_t)(cx - p->anchor_x);
      node->base_rel_y = (int16_t)(cy - p->anchor_y);
    }
  }

  p->base_anchor_x = p->anchor_x;
  p->base_anchor_y = p->anchor_y;
}

/* ---- 对外：设锚点 -------------------------------------------------------- */

void ui_obj_set_anchor(ui_object_t *obj, int16_t ax, int16_t ay)
{
  ui_head_t* head;

  if (obj == NULL || obj->para == NULL)
  {
    return;
  }
  head = (ui_head_t*)obj->para;

  switch (head->type)
  {
    case UI_TYPE_RECT:     ui_xf_set_anchor(&((ui_rect_para_t*)head)->xf,   head, ax, ay); break;
    case UI_TYPE_TEXT:     ui_xf_set_anchor(&((ui_text_para_t*)head)->xf,   head, ax, ay); break;
    case UI_TYPE_CIRCLE:   ui_xf_set_anchor(&((ui_circle_para_t*)head)->xf, head, ax, ay); break;
    case UI_TYPE_POINT:    ui_xf_set_anchor(&((ui_point_para_t*)head)->xf,  head, ax, ay); break;
    case UI_TYPE_IMAGE:    ui_xf_set_anchor(&((ui_image_para_t*)head)->xf,  head, ax, ay); break;
    case UI_TYPE_PROGRESS_BAR: ui_xf_set_anchor(&((ui_progress_common_t*)head)->xf, head, ax, ay); break;
    case UI_TYPE_POLYLINE: ui_polyline_set_anchor((ui_polyline_para_t*)head, ax, ay);      break;
    case UI_TYPE_UNION:
    {
      /* 合并对象：换锚点要把子对象相对锚点的基准偏移同步挪一下，
       * 几何才不变（子对象位置 = 锚点 + 基准偏移 × 比例，比例 = 1 时完全等价） */
      ui_union_para_t* p = (ui_union_para_t*)head;
      ui_merge_obj_node_t* node;
      int16_t dax = (int16_t)(p->anchor_x - ax);
      int16_t day = (int16_t)(p->anchor_y - ay);

      ListForEachEntry(node, &p->list, ui_merge_obj_node_t, node)
      {
        node->base_rel_x = (int16_t)(node->base_rel_x + dax);
        node->base_rel_y = (int16_t)(node->base_rel_y + day);
      }
      p->anchor_x = ax;
      p->anchor_y = ay;
      p->base_anchor_x = ax;
      p->base_anchor_y = ay;
      break;
    }
    case UI_TYPE_VIEWPORT:
    {
      /* 视窗：和合并对象一样，换锚点要把子对象相对锚点的位置基准同步挪一下，
       * 几何才不变（子对象位置 = 锚点 + 基准偏移 × 比例，比例 = 1 时完全等价） */
      ui_viewport_para_t* p = (ui_viewport_para_t*)head;
      ui_merge_obj_node_t* node;
      int16_t dax = (int16_t)(p->anchor_x - ax);
      int16_t day = (int16_t)(p->anchor_y - ay);

      ListForEachEntry(node, &p->list, ui_merge_obj_node_t, node)
      {
        node->base_rel_x = (int16_t)(node->base_rel_x + dax);
        node->base_rel_y = (int16_t)(node->base_rel_y + day);
      }
      p->anchor_x = ax;
      p->anchor_y = ay;
      p->base_anchor_x = ax;
      p->base_anchor_y = ay;
      break;
    }
    default:
      break;
  }
}

/* ---- 对外：确认更新（bake） ---------------------------------------------- */

void ui_obj_bake(ui_object_t *obj)
{
  ui_head_t* head;

  if (obj == NULL || obj->para == NULL)
  {
    return;
  }
  head = (ui_head_t*)obj->para;

  switch (head->type)
  {
    case UI_TYPE_RECT:
    {
      ui_rect_para_t* p = (ui_rect_para_t*)head;
      p->base_w = p->w;
      p->base_h = p->h;
      p->xf.base_cx = p->head.center_x;
      p->xf.base_cy = p->head.center_y;
      p->xf.base_anchor_x = p->xf.anchor_x;   /* 锚点也进确定态：复位要连它一起回退 */
      p->xf.base_anchor_y = p->xf.anchor_y;
      p->xf.anchor_off_x = (int16_t)(p->head.center_x - p->xf.anchor_x);
      p->xf.anchor_off_y = (int16_t)(p->head.center_y - p->xf.anchor_y);
      p->xf.scale_num = 1u;
      p->xf.scale_den = 1u;
      break;
    }
    case UI_TYPE_TEXT:
    {
      ui_text_para_t* p = (ui_text_para_t*)head;
      p->base_w = p->w;
      p->base_h = p->h;
      p->xf.base_cx = p->head.center_x;
      p->xf.base_cy = p->head.center_y;
      p->xf.base_anchor_x = p->xf.anchor_x;   /* 锚点也进确定态：复位要连它一起回退 */
      p->xf.base_anchor_y = p->xf.anchor_y;
      p->xf.anchor_off_x = (int16_t)(p->head.center_x - p->xf.anchor_x);
      p->xf.anchor_off_y = (int16_t)(p->head.center_y - p->xf.anchor_y);
      p->xf.scale_num = 1u;
      p->xf.scale_den = 1u;
      break;
    }
    case UI_TYPE_CIRCLE:
    {
      ui_circle_para_t* p = (ui_circle_para_t*)head;
      p->base_ext_r = p->external_r;
      p->base_int_r = p->internal_r;
      p->xf.base_cx = p->head.center_x;
      p->xf.base_cy = p->head.center_y;
      p->xf.base_anchor_x = p->xf.anchor_x;   /* 锚点也进确定态：复位要连它一起回退 */
      p->xf.base_anchor_y = p->xf.anchor_y;
      p->xf.anchor_off_x = (int16_t)(p->head.center_x - p->xf.anchor_x);
      p->xf.anchor_off_y = (int16_t)(p->head.center_y - p->xf.anchor_y);
      p->xf.scale_num = 1u;
      p->xf.scale_den = 1u;
      break;
    }
    case UI_TYPE_POINT:
    {
      ui_point_para_t* p = (ui_point_para_t*)head;
      p->base_size = p->size;
      p->xf.base_cx = p->head.center_x;
      p->xf.base_cy = p->head.center_y;
      p->xf.base_anchor_x = p->xf.anchor_x;   /* 锚点也进确定态：复位要连它一起回退 */
      p->xf.base_anchor_y = p->xf.anchor_y;
      p->xf.anchor_off_x = (int16_t)(p->head.center_x - p->xf.anchor_x);
      p->xf.anchor_off_y = (int16_t)(p->head.center_y - p->xf.anchor_y);
      p->xf.scale_num = 1u;
      p->xf.scale_den = 1u;
      break;
    }
    case UI_TYPE_IMAGE:
    {
      ui_image_para_t* p = (ui_image_para_t*)head;
      p->base_w = p->w;                       /* 规格也进确定态：现在恒等，留好以后加缩放的位 */
      p->base_h = p->h;
      p->xf.base_cx = p->head.center_x;
      p->xf.base_cy = p->head.center_y;
      p->xf.base_anchor_x = p->xf.anchor_x;   /* 锚点也进确定态：复位要连它一起回退 */
      p->xf.base_anchor_y = p->xf.anchor_y;
      p->xf.anchor_off_x = (int16_t)(p->head.center_x - p->xf.anchor_x);
      p->xf.anchor_off_y = (int16_t)(p->head.center_y - p->xf.anchor_y);
      p->xf.scale_num = 1u;
      p->xf.scale_den = 1u;
      break;
    }
    case UI_TYPE_PROGRESS_BAR:
    {
      ui_progress_common_t* p = (ui_progress_common_t*)head;

      if (p->shape == (uint8_t)UI_PROGRESS_SHAPE_CIRCLE)   /* 圆环：基准 = 当前半径 */
      {
        ui_circle_progress_bar_para_t* c = (ui_circle_progress_bar_para_t*)head;
        c->base_ext_r = c->external_r;
        c->base_int_r = c->internal_r;
      }
      else                                                 /* 矩形：基准 = 当前宽高 */
      {
        ui_rect_progress_bar_para_t* r = (ui_rect_progress_bar_para_t*)head;
        r->base_w = r->w;
        r->base_h = r->h;
      }

      p->xf.base_cx = p->head.center_x;
      p->xf.base_cy = p->head.center_y;
      p->xf.base_anchor_x = p->xf.anchor_x;   /* 锚点也进确定态：复位要连它一起回退 */
      p->xf.base_anchor_y = p->xf.anchor_y;
      p->xf.anchor_off_x = (int16_t)(p->head.center_x - p->xf.anchor_x);
      p->xf.anchor_off_y = (int16_t)(p->head.center_y - p->xf.anchor_y);
      p->xf.scale_num = 1u;
      p->xf.scale_den = 1u;
      break;
    }
    case UI_TYPE_POLYLINE:
      ui_polyline_bake((ui_polyline_para_t*)head);
      break;
    case UI_TYPE_UNION:
      ui_union_bake((ui_union_para_t*)head);
      break;
    case UI_TYPE_VIEWPORT:
    {
      /* 视窗：窗口几何定成基准；子对象各 bake 一次（自带基准 = 当前几何），
       * 再按"子对象中心相对视窗锚点"重算位置基准。锚点不动 */
      ui_viewport_para_t* p = (ui_viewport_para_t*)head;
      ui_merge_obj_node_t* node;

      p->base_w  = p->w;
      p->base_h  = p->h;
      p->base_cx = p->head.center_x;
      p->base_cy = p->head.center_y;

      ListForEachEntry(node, &p->list, ui_merge_obj_node_t, node)
      {
        int16_t cx, cy, w, h;

        ui_obj_bake(node->obj);
        if (ui_obj_frame(node->obj, &cx, &cy, &w, &h))
        {
          node->base_rel_x = (int16_t)(cx - p->anchor_x);
          node->base_rel_y = (int16_t)(cy - p->anchor_y);
        }
      }

      p->base_anchor_x = p->anchor_x;
      p->base_anchor_y = p->anchor_y;
      break;
    }
    default:
      break;
  }
}

/* ---- 对外：复位 ---------------------------------------------------------- */

void ui_obj_restore(ui_object_t *obj)
{
  ui_head_t* head;

  if (obj == NULL || obj->para == NULL)
  {
    return;
  }
  head = (ui_head_t*)obj->para;

  switch (head->type)
  {
    case UI_TYPE_RECT:
    {
      ui_rect_para_t* p = (ui_rect_para_t*)head;
      int16_t ox0 = (int16_t)(p->head.center_x - (p->w >> 1));
      int16_t oy0 = (int16_t)(p->head.center_y - (p->h >> 1));
      int16_t ox1 = (int16_t)(ox0 + p->w - 1);
      int16_t oy1 = (int16_t)(oy0 + p->h - 1);
      int16_t nx0, ny0, nx1, ny1;

      /* 锚点回基准锚点；偏移由"基准中心 - 基准锚点"重算，三者自洽 */
      p->xf.anchor_x = p->xf.base_anchor_x;
      p->xf.anchor_y = p->xf.base_anchor_y;
      p->xf.anchor_off_x = (int16_t)(p->xf.base_cx - p->xf.base_anchor_x);
      p->xf.anchor_off_y = (int16_t)(p->xf.base_cy - p->xf.base_anchor_y);
      p->xf.scale_num = 1u;
      p->xf.scale_den = 1u;
      p->w = p->base_w;
      p->h = p->base_h;
      p->head.center_x = p->xf.base_cx;
      p->head.center_y = p->xf.base_cy;

      nx0 = (int16_t)(p->head.center_x - (p->w >> 1));
      ny0 = (int16_t)(p->head.center_y - (p->h >> 1));
      nx1 = (int16_t)(nx0 + p->w - 1);
      ny1 = (int16_t)(ny0 + p->h - 1);

      ui_dirty_mark_rect_move(ox0, oy0, ox1, oy1, nx0, ny0, nx1, ny1, p->border_width, p->fill);
      break;
    }
    case UI_TYPE_TEXT:
    {
      ui_text_para_t* p = (ui_text_para_t*)head;

      ui_ctrl_mark_box_sym(p->head.center_x, p->head.center_y, p->w, p->h);   /* 老框 */

      /* 锚点回基准锚点；偏移由"基准中心 - 基准锚点"重算，三者自洽 */
      p->xf.anchor_x = p->xf.base_anchor_x;
      p->xf.anchor_y = p->xf.base_anchor_y;
      p->xf.anchor_off_x = (int16_t)(p->xf.base_cx - p->xf.base_anchor_x);
      p->xf.anchor_off_y = (int16_t)(p->xf.base_cy - p->xf.base_anchor_y);
      p->xf.scale_num = 1u;
      p->xf.scale_den = 1u;
      p->w = p->base_w;
      p->h = p->base_h;
      p->head.center_x = p->xf.base_cx;
      p->head.center_y = p->xf.base_cy;

      ui_ctrl_mark_box_sym(p->head.center_x, p->head.center_y, p->w, p->h);   /* 新框 */
      break;
    }
    case UI_TYPE_CIRCLE:
    {
      ui_circle_para_t* p = (ui_circle_para_t*)head;

      ui_ctrl_mark_radius(p->head.center_x, p->head.center_y, p->external_r); /* 老框 */

      /* 锚点回基准锚点；偏移由"基准中心 - 基准锚点"重算，三者自洽 */
      p->xf.anchor_x = p->xf.base_anchor_x;
      p->xf.anchor_y = p->xf.base_anchor_y;
      p->xf.anchor_off_x = (int16_t)(p->xf.base_cx - p->xf.base_anchor_x);
      p->xf.anchor_off_y = (int16_t)(p->xf.base_cy - p->xf.base_anchor_y);
      p->xf.scale_num = 1u;
      p->xf.scale_den = 1u;
      p->external_r = p->base_ext_r;
      p->internal_r = p->base_int_r;
      p->head.center_x = p->xf.base_cx;
      p->head.center_y = p->xf.base_cy;

      ui_ctrl_mark_radius(p->head.center_x, p->head.center_y, p->external_r); /* 新框 */
      break;
    }
    case UI_TYPE_POINT:
    {
      ui_point_para_t* p = (ui_point_para_t*)head;
      int16_t ox0 = (int16_t)(p->head.center_x - (p->size >> 1));
      int16_t oy0 = (int16_t)(p->head.center_y - (p->size >> 1));
      int16_t ox1 = (int16_t)(ox0 + p->size - 1);
      int16_t oy1 = (int16_t)(oy0 + p->size - 1);
      int16_t nx0, ny0, nx1, ny1;

      /* 位置、尺寸都读回基准；锚点回基准锚点，偏移由"基准中心 - 基准锚点"重算，因子复位 */
      p->xf.anchor_x = p->xf.base_anchor_x;
      p->xf.anchor_y = p->xf.base_anchor_y;
      p->xf.anchor_off_x = (int16_t)(p->xf.base_cx - p->xf.base_anchor_x);
      p->xf.anchor_off_y = (int16_t)(p->xf.base_cy - p->xf.base_anchor_y);
      p->xf.scale_num = 1u;
      p->xf.scale_den = 1u;
      p->size = p->base_size;
      if (p->size < 1) { p->size = 1; }
      p->head.center_x = p->xf.base_cx;
      p->head.center_y = p->xf.base_cy;

      nx0 = (int16_t)(p->head.center_x - (p->size >> 1));
      ny0 = (int16_t)(p->head.center_y - (p->size >> 1));
      nx1 = (int16_t)(nx0 + p->size - 1);
      ny1 = (int16_t)(ny0 + p->size - 1);

      /* 点是实心块：老框 / 新框都整块标脏 */
      ui_dirty_mark(ox0, oy0, ox1, oy1);
      ui_dirty_mark(nx0, ny0, nx1, ny1);
      break;
    }
    case UI_TYPE_IMAGE:
    {
      ui_image_para_t* p = (ui_image_para_t*)head;
      int16_t ox0, oy0, ox1, oy1, nx0, ny0, nx1, ny1;

      ui_box_of(p->head.center_x, p->head.center_y, p->w, p->h, &ox0, &oy0, &ox1, &oy1);

      /* 锚点回基准锚点；偏移由"基准中心 - 基准锚点"重算，因子复位、规格读回基准 */
      p->xf.anchor_x = p->xf.base_anchor_x;
      p->xf.anchor_y = p->xf.base_anchor_y;
      p->xf.anchor_off_x = (int16_t)(p->xf.base_cx - p->xf.base_anchor_x);
      p->xf.anchor_off_y = (int16_t)(p->xf.base_cy - p->xf.base_anchor_y);
      p->xf.scale_num = 1u;
      p->xf.scale_den = 1u;
      p->w = p->base_w;
      p->h = p->base_h;
      p->head.center_x = p->xf.base_cx;
      p->head.center_y = p->xf.base_cy;

      ui_box_of(p->head.center_x, p->head.center_y, p->w, p->h, &nx0, &ny0, &nx1, &ny1);

      /* 图片是实心块：老框 / 新框都整块标脏 */
      ui_dirty_mark(ox0, oy0, ox1, oy1);
      ui_dirty_mark(nx0, ny0, nx1, ny1);
      break;
    }
    case UI_TYPE_PROGRESS_BAR:
    {
      ui_progress_common_t* p = (ui_progress_common_t*)head;
      int16_t ox0, oy0, ox1, oy1, nx0, ny0, nx1, ny1;

      ui_progress_box(p, &ox0, &oy0, &ox1, &oy1);        /* 老框 */

      /* 锚点回基准锚点；偏移由"基准中心 - 基准锚点"重算，因子复位、尺寸读回基准 */
      p->xf.anchor_x = p->xf.base_anchor_x;
      p->xf.anchor_y = p->xf.base_anchor_y;
      p->xf.anchor_off_x = (int16_t)(p->xf.base_cx - p->xf.base_anchor_x);
      p->xf.anchor_off_y = (int16_t)(p->xf.base_cy - p->xf.base_anchor_y);
      p->xf.scale_num = 1u;
      p->xf.scale_den = 1u;

      if (p->shape == (uint8_t)UI_PROGRESS_SHAPE_CIRCLE)
      {
        ui_circle_progress_bar_para_t* c = (ui_circle_progress_bar_para_t*)head;
        c->external_r = c->base_ext_r;
        c->internal_r = c->base_int_r;
      }
      else
      {
        ui_rect_progress_bar_para_t* r = (ui_rect_progress_bar_para_t*)head;
        r->w = r->base_w;
        r->h = r->base_h;
      }

      p->head.center_x = p->xf.base_cx;
      p->head.center_y = p->xf.base_cy;

      ui_progress_box(p, &nx0, &ny0, &nx1, &ny1);        /* 新框 */
      ui_dirty_mark(ox0, oy0, ox1, oy1);
      ui_dirty_mark(nx0, ny0, nx1, ny1);
      break;
    }
    case UI_TYPE_POLYLINE:
      ui_polyline_restore((ui_polyline_para_t*)head);
      break;
    case UI_TYPE_UNION:
    {
      ui_union_para_t* p = (ui_union_para_t*)head;
      ui_merge_obj_node_t* node;
      int16_t ox0 = 0, oy0 = 0, ox1 = 0, oy1 = 0;
      uint8_t had_old = (uint8_t)(p->w > 0 && p->h > 0);

      if (had_old != 0u)
      {
        ui_box_of(p->head.center_x, p->head.center_y, p->w, p->h, &ox0, &oy0, &ox1, &oy1);
      }

      /* 锚点回基准锚点；每个子对象各自回到自带基准（= 成组那一刻的几何）。
       * 不走 ui_anim：只依赖 ui_control 自己，避免 ui_control 和 ui_anim 互相依赖 */
      p->anchor_x = p->base_anchor_x;
      p->anchor_y = p->base_anchor_y;
      ListForEachEntry(node, &p->list, ui_merge_obj_node_t, node)
      {
        if (node->obj != NULL && node->obj->para != NULL)
        {
          ui_obj_restore(node->obj);
        }
      }

      ui_union_update_frame(p);

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
      break;
    }
    case UI_TYPE_VIEWPORT:
    {
      /* 视窗：锚点回基准锚点、窗口几何回基准几何，子对象各自回自带基准 */
      ui_viewport_para_t* p = (ui_viewport_para_t*)head;
      ui_merge_obj_node_t* node;
      int16_t ox0 = 0, oy0 = 0, ox1 = 0, oy1 = 0;
      uint8_t had_old = (uint8_t)(p->w > 0 && p->h > 0);

      if (had_old != 0u)
      {
        ui_box_of(p->head.center_x, p->head.center_y, p->w, p->h, &ox0, &oy0, &ox1, &oy1);
      }

      p->anchor_x = p->base_anchor_x;
      p->anchor_y = p->base_anchor_y;
      p->w = p->base_w;
      p->h = p->base_h;
      p->head.center_x = p->base_cx;
      p->head.center_y = p->base_cy;

      ListForEachEntry(node, &p->list, ui_merge_obj_node_t, node)
      {
        if (node->obj != NULL && node->obj->para != NULL)
        {
          ui_obj_restore(node->obj);
        }
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
      break;
    }
    default:
      break;
  }
}


/* ---- 对外：激活 / 去激活 -------------------------------------------------- */

/* 递归设置：自己 + (是容器的话)所有子对象。
 * 容器套容器也一路走到底 —— 和 ui_draw_union / ui_draw_viewport 的层级一致 */
static void ui_ctrl_active_tree(ui_object_t* obj, ui_activation_t st);

/* 把"框里装着别的对象"的两类容器的子对象一起设成 st。
 * 子对象不单独进绘制链表、是被容器带出来画的；不一起设就会留下
 * "容器藏了、子对象却还能被操作链遍历到"的缝 */
static void ui_ctrl_active_children(ui_head_t* head, ui_activation_t st);

static void ui_ctrl_active_tree(ui_object_t* obj, ui_activation_t st)
{
  if(obj == NULL)
  {
    return;
  }

  obj->activation = st;                        /* 自己先定下来 */

  if(obj->para != NULL)                        /* para 还没配好就没有子对象可设 */
  {
    ui_ctrl_active_children((ui_head_t*)obj->para, st);
  }
}

static void ui_ctrl_active_children(ui_head_t* head, ui_activation_t st)
{
  ui_merge_obj_node_t* it;

  switch(head->type)
  {
    case UI_TYPE_UNION:                        /* 合并对象：子对象挂在 para->list 上 */
    {
      ui_union_para_t* p = (ui_union_para_t*)head;

      ListForEachEntry(it, &p->list, ui_merge_obj_node_t, node)
      {
        ui_ctrl_active_tree(it->obj, st);
      }
      break;
    }

    case UI_TYPE_VIEWPORT:                     /* 视窗：子对象同样挂在 para->list 上 */
    {
      ui_viewport_para_t* p = (ui_viewport_para_t*)head;

      ListForEachEntry(it, &p->list, ui_merge_obj_node_t, node)
      {
        ui_ctrl_active_tree(it->obj, st);
      }
      break;
    }

    default:                                   /* 别的类型里没有子对象 */
      break;
  }
}

uint8_t ui_obj_set_active(ui_object_t *obj, uint8_t on)
{
  ui_activation_t st = (on != 0u) ? activated : deactivated;
  int16_t cx, cy, w, h;

  if(obj == NULL)
  {
    return 0;
  }

  if(obj->activation == st)
  {
    return 0;                                  /* 状态没变：不重复标脏 */
  }

  /* 记脏要用外接框，它只跟几何有关、跟激活状态无关，所以放在改状态之前取。
   * 取不到框(para 还没配好)就不标 —— 它本来也没在屏幕上出现过 */
  if(ui_obj_frame(obj, &cx, &cy, &w, &h) != 0u)
  {
    ui_ctrl_mark_box_sym(cx, cy, w, h);
  }

  ui_ctrl_active_tree(obj, st);
  return 1;
}

/* ============================================================================
 * 半透明：统一设置一个对象的 alpha
 * ========================================================================== */

/* 按类型把 alpha 写进 para 的 alpha 字段；容器(合并 / 视窗)递归到子对象。
 * 返回 1 = 至少有一个对象的值确实变了 */
static uint8_t ui_ctrl_alpha_set(ui_head_t *head, uint8_t alpha)
{
  uint8_t changed = 0u;

  if (head == NULL)
  {
    return 0u;
  }

  switch (head->type)
  {
    case UI_TYPE_RECT:
    case UI_TYPE_MODULE:                       /* 组件的 box 就是 ui_rect_para_t 前缀 */
    {
      ui_rect_para_t *p = (ui_rect_para_t *)head;

      if (p->alpha != alpha) { p->alpha = alpha; changed = 1u; }
      break;
    }

    case UI_TYPE_CIRCLE:
    {
      ui_circle_para_t *p = (ui_circle_para_t *)head;

      if (p->alpha != alpha) { p->alpha = alpha; changed = 1u; }
      break;
    }

    case UI_TYPE_POINT:
    {
      ui_point_para_t *p = (ui_point_para_t *)head;

      if (p->alpha != alpha) { p->alpha = alpha; changed = 1u; }
      break;
    }

    case UI_TYPE_IMAGE:
    {
      ui_image_para_t *p = (ui_image_para_t *)head;

      if (p->alpha != alpha) { p->alpha = alpha; changed = 1u; }
      break;
    }

    case UI_TYPE_TEXT:
    {
      ui_text_para_t *p = (ui_text_para_t *)head;

      if (p->alpha != alpha) { p->alpha = alpha; changed = 1u; }
      break;
    }

    case UI_TYPE_CURSOR:
    {
      ui_cursor_para_t *p = (ui_cursor_para_t *)head;

      if (p->alpha != alpha) { p->alpha = alpha; changed = 1u; }
      break;
    }

    case UI_TYPE_POLYLINE:
    {
      ui_polyline_para_t *p = (ui_polyline_para_t *)head;

      if (p->alpha != alpha) { p->alpha = alpha; changed = 1u; }
      break;
    }

    case UI_TYPE_PROGRESS_BAR:                 /* 圆环 / 矩形共用这个类型，看 shape 分派 */
    {
      ui_progress_common_t *c = (ui_progress_common_t *)head;

      if (c->shape == (uint8_t)UI_PROGRESS_SHAPE_CIRCLE)
      {
        ui_circle_progress_bar_para_t *p = (ui_circle_progress_bar_para_t *)head;

        if (p->alpha != alpha) { p->alpha = alpha; changed = 1u; }
      }
      else
      {
        ui_rect_progress_bar_para_t *p = (ui_rect_progress_bar_para_t *)head;

        if (p->alpha != alpha) { p->alpha = alpha; changed = 1u; }
      }
      break;
    }

    case UI_TYPE_UNION:                        /* 容器自己不上色，递归子对象 */
    case UI_TYPE_VIEWPORT:
    {
      ui_merge_obj_node_t *it;
      list_head_t *list = (head->type == UI_TYPE_UNION)
                          ? &((ui_union_para_t *)head)->list
                          : &((ui_viewport_para_t *)head)->list;

      ListForEachEntry(it, list, ui_merge_obj_node_t, node)
      {
        if (it->obj != NULL && it->obj->para != NULL)
        {
          if (ui_ctrl_alpha_set((ui_head_t *)it->obj->para, alpha) != 0u)
          {
            changed = 1u;
          }
        }
      }
      break;
    }

    default:                                   /* 量不出 alpha 的类型：什么都不做 */
      break;
  }

  return changed;
}

uint8_t ui_obj_set_alpha(ui_object_t *obj, uint8_t alpha)
{
  ui_head_t *head;
  int16_t cx, cy, w, h;
  uint8_t changed;

  if (obj == NULL || obj->para == NULL)
  {
    return 0u;
  }

  head    = (ui_head_t *)obj->para;
  changed = ui_ctrl_alpha_set(head, alpha);
  if (changed == 0u)
  {
    return 0u;                                 /* 值没变 / 该类型不支持：不重复标脏 */
  }

  /* 半透明只改颜色、不改几何：按外接框标一次脏，下一帧重画这一块就生效。
   * 容器的框已经覆盖所有子对象，所以容器里面的变化也靠这一块重画 */
  if (ui_obj_frame(obj, &cx, &cy, &w, &h) != 0u)
  {
    ui_ctrl_mark_box_sym(cx, cy, w, h);
  }
  else if (head->type == UI_TYPE_CURSOR)
  {
    /* 光标不在取框表里(它框的是目标，自己不是"被框对象")：用光标自己的几何标脏 */
    ui_cursor_para_t *c = (ui_cursor_para_t *)head;

    ui_ctrl_mark_box_sym(c->head.center_x, c->head.center_y, c->w, c->h);
  }

  return 1u;
}

void ui_chain_set_active(ui_chain_t *chain, uint8_t on)
{
  if(chain == NULL)
  {
    return;
  }

  /* 只改一个标志，O(1)：链上对象的 activation 一个都不动、顺序和 z 序不变。
   * 门控在 ui_level_draw / ui_chain_visit / ui_hit_test 三条遍历里 ——
   * 链去激活后它的对象立刻"不画、不被点到"。
   * 这里不标脏：链没有自己的几何，切链影响哪块由调用方标（一般切一次标一次内容区） */
  chain->activation = (on != 0u) ? activated : deactivated;
}


/* ============================================================================
 * 外接框查询（搬自 ui_create.c）：按 type 查表取"中心 + 宽高"
 * ========================================================================== */

/* 取外接框的函数类型：把自己在屏幕上的中心 + 宽高报出来，算不出返回 0 */
typedef uint8_t (*ui_bounds_fn_t)(void*, int16_t*, int16_t*, int16_t*, int16_t*);

/* 各图元各自实现一个；对不上类型就返回 0。
 * 组件(UI_TYPE_MODULE)也走这里：para 首成员就是 ui_rect_para_t(见 ui_module.h 的组件约定)，
 * 按矩形前缀读框即可 —— 不认它的话组件量不出框，命中测试会整个失效 */
static uint8_t ui_rect_bounds(void* p, int16_t* cx, int16_t* cy, int16_t* w, int16_t* h)
{
  ui_rect_para_t* rect = p;
  if (rect->head.type != UI_TYPE_RECT && rect->head.type != UI_TYPE_MODULE)
  {
    return 0;
  }
  *cx = rect->head.center_x;
  *cy = rect->head.center_y;
  *w  = rect->w;
  *h  = rect->h;
  return 1;
}

static uint8_t ui_text_bounds(void* p, int16_t* cx, int16_t* cy, int16_t* w, int16_t* h)
{
  ui_text_para_t* text = p;
  if (text->head.type != UI_TYPE_TEXT)
  {
    return 0;
  }
  *cx = text->head.center_x;
  *cy = text->head.center_y;
  *w  = text->w;
  *h  = text->h;
  return 1;
}

static uint8_t ui_circle_bounds(void* p, int16_t* cx, int16_t* cy, int16_t* w, int16_t* h)
{
  ui_circle_para_t* circle = p;
  if (circle->head.type != UI_TYPE_CIRCLE)
  {
    return 0;
  }
  *cx = circle->head.center_x;
  *cy = circle->head.center_y;
  /* 圆按"像素到圆心距离 <= 外半径"画，横向 / 纵向最远各到 ±外半径，共 2r+1 个像素；
   * 框宽必须是 2r+1（不是 2r），否则最右 / 最下一列落在框外，
   * 合并对象的并集框会漏掉这 1px、脏区也跟着漏标 —— 就留残点 */
  *w  = (int16_t)(circle->external_r * 2 + 1);
  *h  = *w;
  return 1;
}

/* 点的外接框就是它自己那个实心方块：边长 size */
static uint8_t ui_point_bounds(void* p, int16_t* cx, int16_t* cy, int16_t* w, int16_t* h)
{
  ui_point_para_t* point = p;
  if (point->head.type != UI_TYPE_POINT)
  {
    return 0;
  }
  *cx = point->head.center_x;
  *cy = point->head.center_y;
  *w  = point->size;
  *h  = point->size;
  return 1;
}

/* 图片的外接框就是图片本身：规格 w×h */
static uint8_t ui_image_bounds(void* p, int16_t* cx, int16_t* cy, int16_t* w, int16_t* h)
{
  ui_image_para_t* image = p;
  if (image->head.type != UI_TYPE_IMAGE)
  {
    return 0;
  }
  *cx = image->head.center_x;
  *cy = image->head.center_y;
  *w  = image->w;
  *h  = image->h;
  return 1;
}

/* 折线的外接框就是它自己维护的对象框（已含线宽外扩），直接读，不用再扫一遍顶点 */
static uint8_t ui_polyline_bounds(void* p, int16_t* cx, int16_t* cy, int16_t* w, int16_t* h)
{
  ui_polyline_para_t* polyline = p;
  if (polyline->head.type != UI_TYPE_POLYLINE)
  {
    return 0;
  }
  *cx = polyline->head.center_x;
  *cy = polyline->head.center_y;
  *w  = polyline->w;
  *h  = polyline->h;
  return 1;
}

/* 合并对象的外接框就是它自己维护的对象框（所有子对象框的并集），直接读 */
static uint8_t ui_union_bounds(void* p, int16_t* cx, int16_t* cy, int16_t* w, int16_t* h)
{
  ui_union_para_t* union_para = p;
  if (union_para->head.type != UI_TYPE_UNION)
  {
    return 0;
  }
  *cx = union_para->head.center_x;
  *cy = union_para->head.center_y;
  *w  = union_para->w;
  *h  = union_para->h;
  return 1;
}

/* 视窗的外接框就是窗口本身：中心 + 宽高直接读（窗口尺寸是显式给的，不从子对象量） */
static uint8_t ui_viewport_bounds(void* p, int16_t* cx, int16_t* cy, int16_t* w, int16_t* h)
{
  ui_viewport_para_t* viewport = p;
  if (viewport->head.type != UI_TYPE_VIEWPORT)
  {
    return 0;
  }
  *cx = viewport->head.center_x;
  *cy = viewport->head.center_y;
  *w  = viewport->w;
  *h  = viewport->h;
  return 1;
}

/* 进度条外接框：圆环按 2r+1（和 ui_circle_bounds 一个口径）、矩形按 w×h，中心取 head */
static uint8_t ui_progress_bounds(void* p, int16_t* cx, int16_t* cy, int16_t* w, int16_t* h)
{
  ui_progress_common_t* para = p;
  if (para->head.type != UI_TYPE_PROGRESS_BAR)
  {
    return 0;
  }
  *cx = para->head.center_x;
  *cy = para->head.center_y;
  if (para->shape == (uint8_t)UI_PROGRESS_SHAPE_CIRCLE)
  {
    *w = (int16_t)(((ui_circle_progress_bar_para_t*)p)->external_r * 2 + 1);
    *h = *w;
  }
  else
  {
    ui_rect_progress_bar_para_t* r = (ui_rect_progress_bar_para_t*)p;
    *w = r->w;
    *h = r->h;
  }
  return 1;
}

/* 取框函数表：按 type 直接下标取，省掉 switch。
 * 没登记的位置（NONE / CURSOR）是 NULL —— "光标框光标"这条旧特判也由它兜住 */
static const ui_bounds_fn_t cursor_target_bounds[] =
{
  [UI_TYPE_NONE]     = NULL,
  [UI_TYPE_CIRCLE]   = ui_circle_bounds,
  [UI_TYPE_RECT]     = ui_rect_bounds,
  [UI_TYPE_TEXT]     = ui_text_bounds,
  [UI_TYPE_CURSOR]   = NULL,
  [UI_TYPE_POLYLINE] = ui_polyline_bounds,
  [UI_TYPE_UNION]    = ui_union_bounds,
  [UI_TYPE_POINT]    = ui_point_bounds,
  [UI_TYPE_VIEWPORT] = ui_viewport_bounds,
  [UI_TYPE_PROGRESS_BAR] = ui_progress_bounds,
  [UI_TYPE_IMAGE] = ui_image_bounds,
  /* 组件(UI_TYPE_MODULE)：para 首成员是 ui_rect_para_t(组件外接框，组件自己维护 w/h)，
   * 按矩形口径量框即可 —— 以后加组件不再动这张表(约定见 ui_module.h) */
  [UI_TYPE_MODULE] = ui_rect_bounds,
};

/* 取任意对象外接框（中心 + 宽高）：按 type 查上面那张取框表。
 * 合并对象重算自身框、append 时取子对象框、滚动视图找被点的子对象，都走它 */
uint8_t ui_obj_frame(ui_object_t* obj, int16_t* cx, int16_t* cy, int16_t* w, int16_t* h)
{
  ui_head_t* head;
  ui_bounds_fn_t bounds;

  if (obj == NULL || obj->para == NULL ||
      cx == NULL || cy == NULL || w == NULL || h == NULL)
  {
    return 0;
  }

  head = (ui_head_t*)obj->para;
  if ((unsigned)head->type >= sizeof(cursor_target_bounds) / sizeof(cursor_target_bounds[0]))
  {
    return 0;
  }

  bounds = cursor_target_bounds[head->type];
  if (bounds == NULL)
  {
    return 0;
  }
  return bounds(head, cx, cy, w, h);
}


/* ============================================================================
 * 命中测试
 * ========================================================================== */

/* 一个对象的【命中框】= 真实外框 + 最小可点尺寸补齐。
 * 命中测试和调试框共用这一份口径，避免两处算得不一样 */
static uint8_t ui_ctrl_hit_box(ui_object_t* obj, int16_t* x0, int16_t* y0, int16_t* x1, int16_t* y1)
{
  int16_t cx, cy, w, h, d;

  if (ui_obj_frame(obj, &cx, &cy, &w, &h) == 0u)   /* 量不出框(para 还没配好) */
  {
    return 0;
  }

  ui_box_of(cx, cy, w, h, x0, y0, x1, y1);

  /* 触摸友好：小图元按【最小可点尺寸】向四周补齐(尺寸见 ui_config.h 的 UI_HIT_MIN_SIZE) */
  if (w < UI_HIT_MIN_SIZE)
  {
    d = (int16_t)((UI_HIT_MIN_SIZE - w + 1) / 2);
    *x0 = (int16_t)(*x0 - d);
    *x1 = (int16_t)(*x1 + d);
  }
  if (h < UI_HIT_MIN_SIZE)
  {
    d = (int16_t)((UI_HIT_MIN_SIZE - h + 1) / 2);
    *y0 = (int16_t)(*y0 - d);
    *y1 = (int16_t)(*y1 + d);
  }

  return 1u;
}

/* 在一条子对象链里找命中（前向声明：下面的 ui_ctrl_hit_object 要递归调它） */
static ui_object_t* ui_ctrl_hit_children(list_head_t* list, int16_t x, int16_t y);

/* 判断一个对象是不是命中点，是的话让 *hit 指向它 —— 也可能指向它的某个子对象。
 * 规则（视窗转派 / 合并对象不转派）见 ui_control.h 里 ui_hit_test 的说明 */
static uint8_t ui_ctrl_hit_object(ui_object_t* obj, int16_t x, int16_t y, ui_object_t** hit)
{
  ui_head_t* head;
  int16_t x0, y0, x1, y1;

  if (obj == NULL || hit == NULL)
  {
    return 0;
  }

  if (ui_obj_is_active(obj) == 0u)                 /* 藏起来的不参与命中 */
  {
    return 0;
  }

  /* 对象所属的链被整条去激活时同样不参与命中（链 = 同一层级内的组开关，见 ui_framework.h）。
   * 只画不操作的对象(chain == NULL)不受链开关管 */
  if (obj->chain != NULL && obj->chain->activation == deactivated)
  {
    return 0;
  }

  if (ui_ctrl_hit_box(obj, &x0, &y0, &x1, &y1) == 0u)   /* 量不出框就不命中 */
  {
    return 0;
  }

  if (x < x0 || x > x1 || y < y0 || y > y1)
  {
    return 0;
  }

  head = (ui_head_t*)obj->para;

  /* 视窗：先问里面的子对象。能走到这一步说明点已经落在窗口矩形内了（视窗的框
   * 就是窗口本身），所以"窗口外的子对象不该被点到"和绘制裁剪天然一致，不用再夹一次。
   *
   * 【注意这里不看自己的 cb】—— 视窗自己没挂回调也必须先转派，否则它会把
   * 子对象的命中机会一起挡掉 */
  if (head->type == UI_TYPE_VIEWPORT)
  {
    ui_object_t* child;

    child = ui_ctrl_hit_children(&((ui_viewport_para_t*)head)->list, x, y);
    if (child != NULL)
    {
      *hit = child;
      return 1;
    }
    /* 子对象都没中，才轮到视窗自己 */
  }
  /* 合并对象不转派：它就是一个组件，只是由不同子对象渲染而来 */

  if (obj->cb == NULL)                             /* 自己没挂回调 = 不可命中，当透明穿透过去 */
  {
    return 0;
  }

  *hit = obj;
  return 1;
}

/* 在一条子对象链里找命中：倒着走 —— 后挂的子对象画在上层，也该先被点到 */
static ui_object_t* ui_ctrl_hit_children(list_head_t* list, int16_t x, int16_t y)
{
  ui_merge_obj_node_t* it;

  if (list == NULL)
  {
    return NULL;
  }

  ListForEachEntryReverse(it, list, ui_merge_obj_node_t, node)
  {
    ui_object_t* hit = NULL;

    if (ui_ctrl_hit_object(it->obj, x, y, &hit) != 0u)
    {
      return hit;
    }
  }

  return NULL;
}

ui_object_t *ui_hit_test(ui_hierarchy_t *level, int16_t x, int16_t y)
{
  ui_object_t* obj;

  if (level == NULL)
  {
    return NULL;
  }

  /* 倒着走绘制链表：z 序就挂在这条链上（后画的盖住先画的），从最后一个往前走
   * 就是"从最上层往下层"，第一个命中的就是该接这一下的 */
  ListForEachEntryReverse(obj, &level->draws, ui_object_t, draw_node)
  {
    ui_object_t* hit = NULL;

    if (ui_ctrl_hit_object(obj, x, y, &hit) != 0u)
    {
      return hit;
    }
  }

  return NULL;
}

/* 程序触发一次点击：合成 UI_EV_CLICK 交给对象的 cb，和真实点击同一条路径。
 * 只投递事件，不认识控件类型 —— 具体怎么响应由控件自己的回调决定 */
uint8_t ui_obj_click(ui_object_t* obj)
{
  ui_event_t ev;

  if (obj == NULL || obj->para == NULL || obj->cb == NULL)
  {
    return 0u;
  }

  ev.type    = UI_EV_CLICK;
  ev.id      = 0u;
  ev.x       = ((ui_head_t*)obj->para)->center_x;   /* 点在自己中心：点击类回调不看坐标，填个合理值 */
  ev.y       = ((ui_head_t*)obj->para)->center_y;
  ev.dx      = 0;
  ev.dy      = 0;
  ev.time_ms = 0u;

  return obj->cb(obj, &ev);
}

/* ---- 命中区调试框 ------------------------------------------------------------
 * 把当前层级里每个【可点对象】(cb != NULL)的命中框描一圈，用来目视核对点击范围
 * (尤其是 UI_HIT_MIN_SIZE 补齐之后到底多大)。只在 User/main.c 的逐条绘制回调里调：
 * 那时行缓冲和裁剪都是活的，画出来的框和普通矩形一样会被脏条裁掉。
 * 调用处受 ui_config.h 的 UI_HIT_DEBUG_DRAW 控制，正式固件关掉即可 */
void ui_hit_debug_draw(ui_hierarchy_t* level, ui_color_t color)
{
  static ui_rect_para_t box;
  ui_object_t* obj;
  int16_t x0, y0, x1, y1;

  if (level == NULL)
  {
    return;
  }

  /* 临时矩形参数：只描边；几何每一圈现填(中心和宽高由命中框反算) */
  box.head.type    = UI_TYPE_RECT;
  box.head.draw    = ui_draw_rect;
  box.border_width = 1;
  box.color        = color;
  box.fill         = 0u;
  box.corners      = 0u;
  box.radius       = 0u;
  box.xf.scale_num = 1u;
  box.xf.scale_den = 1u;

  ListForEachEntry(obj, &level->draws, ui_object_t, draw_node)
  {
    if (obj->cb == NULL || ui_obj_is_active(obj) == 0u)
    {
      continue;                                  /* 不可点的对象没有"触发范围"可看 */
    }

    if (ui_ctrl_hit_box(obj, &x0, &y0, &x1, &y1) == 0u)
    {
      continue;                                  /* 量不出框 */
    }

    box.head.center_x = (int16_t)(x0 + ((x1 - x0 + 1) >> 1));
    box.head.center_y = (int16_t)(y0 + ((y1 - y0 + 1) >> 1));
    box.w = box.base_w = (int16_t)(x1 - x0 + 1);
    box.h = box.base_h = (int16_t)(y1 - y0 + 1);

    ui_draw_rect(&box);
  }
}

/* ============================================================================
 * 光标操作（搬自 ui_create.c）
 * ========================================================================== */

uint8_t ui_cursor_target_frame(ui_object_t* obj, int16_t gap, int16_t border_width,
                               int16_t* cx, int16_t* cy, int16_t* w, int16_t* h)
{
  ui_head_t* target;
  ui_bounds_fn_t bounds;
  int16_t bx, by, bw, bh, pad;

  if (obj == NULL || obj->para == NULL ||
      cx == NULL || cy == NULL || w == NULL || h == NULL)
  {
    return 0;
  }

  target = (ui_head_t*)obj->para;

  /* type 可能没赋过值（默认 0）甚至被写坏，先把下标夹在表内再查 */
  if ((unsigned)target->type >= sizeof(cursor_target_bounds) / sizeof(cursor_target_bounds[0]))
  {
    return 0;
  }

  bounds = cursor_target_bounds[target->type];

  /* 不认识 / 光标框光标（表里是 NULL）：什么都不改 */
  if (bounds == NULL)
  {
    return 0;
  }

  /* 先把外接框取到手，取不到就一个字都不动 */
  if (!bounds(target, &bx, &by, &bw, &bh))
  {
    return 0;
  }

  pad = (int16_t)(gap + border_width);

  /* 中心照搬；宽高各加两个 pad，方框整体外扩一圈，边框内缘四边各离图案 gap 像素 */
  *cx = bx;
  *cy = by;
  *w  = (int16_t)(bw + pad * 2);
  *h  = (int16_t)(bh + pad * 2);

  return 1;
}

uint8_t ui_cursor_fit(ui_cursor_para_t* cursor_para)
{
  if (cursor_para == NULL)
  {
    return 0;
  }

  return ui_cursor_target_frame(cursor_para->obj, cursor_para->gap,
                                (int16_t)cursor_para->border_width,
                                &cursor_para->head.center_x, &cursor_para->head.center_y,
                                &cursor_para->w, &cursor_para->h);
}

/* ============================================================================
 * 折线操作（搬自 ui_create.c）
 * ========================================================================== */

void ui_polyline_update_frame(ui_polyline_para_t* para)
{
  ui_poly_node_t* node;
  int16_t ax, ay, x_min, x_max, y_min, y_max, pad, w;

  if (para == NULL)
  {
    return;
  }

  ax = para->anchor_x;
  ay = para->anchor_y;

  /* 先按"锚点 + 偏移 × 因子"算出起始点的屏幕坐标，再让每个顶点去撑 */
  x_min = x_max = ui_poly_virtual(para->start.x, ax, para->scale_num, para->scale_den);
  y_min = y_max = ui_poly_virtual(para->start.y, ay, para->scale_num, para->scale_den);

  ListForEachEntry(node, &para->list, ui_poly_node_t, node)
  {
    int16_t px = ui_poly_virtual(node->point.x, ax, para->scale_num, para->scale_den);
    int16_t py = ui_poly_virtual(node->point.y, ay, para->scale_num, para->scale_den);

    if (px < x_min) { x_min = px; }
    if (px > x_max) { x_max = px; }
    if (py < y_min) { y_min = py; }
    if (py > y_max) { y_max = py; }
  }

  pad = (int16_t)((para->line_width + 1) >> 1);          /* 半线宽，向上取整 */

  x_min = (int16_t)(x_min - pad);
  x_max = (int16_t)(x_max + pad);
  y_min = (int16_t)(y_min - pad);
  y_max = (int16_t)(y_max + pad);

  /* 存成 中心 + 宽高，并保证 center - w/2 == x_min、center + w - 1 == x_max 能精确还原 */
  w          = (int16_t)(x_max - x_min + 1);
  para->w    = w;
  para->h    = (int16_t)(y_max - y_min + 1);
  para->head.center_x = (int16_t)(x_min + (w >> 1));
  para->head.center_y = (int16_t)(y_min + (para->h >> 1));
}

uint8_t ui_polyline_append(ui_polyline_para_t* para, ui_poly_node_t* node)
{
  int16_t old_x_min, old_y_min, old_x_max, old_y_max;
  int16_t new_x_min, new_y_min, new_x_max, new_y_max;

  if (para == NULL || node == NULL)
  {
    return 0;
  }

  ui_poly_rect_of(para, &old_x_min, &old_y_min, &old_x_max, &old_y_max);

  /* 调用方按屏幕绝对坐标写 point，这里减掉当前锚点，存成"相对锚点的偏移" */
  node->point.x = (int16_t)(node->point.x - para->anchor_x);
  node->point.y = (int16_t)(node->point.y - para->anchor_y);

  ListAddTail(&para->list, &node->node);   /* 尾插：链上顺序 = 线段顺序 */
  para->count++;

  ui_polyline_update_frame(para);

  ui_poly_rect_of(para, &new_x_min, &new_y_min, &new_x_max, &new_y_max);

  /* 老框 + 新框都标脏：新线段要画出来，老框盖住的地方也重画一遍擦旧痕 */
  ui_dirty_mark(old_x_min, old_y_min, old_x_max, old_y_max);
  ui_dirty_mark(new_x_min, new_y_min, new_x_max, new_y_max);
  return 1;
}

uint8_t ui_polyline_append_rel(ui_polyline_para_t* para, ui_poly_node_t* node,
                               int16_t dx, int16_t dy)
{
  int16_t base_x, base_y;

  if (para == NULL || node == NULL)
  {
    return 0;
  }

  /* 基准点 = "上一个顶点"的绝对坐标：空链时是起始点 start，非空时是链尾顶点。
   * 这里的绝对坐标是"未缩放"的那套（锚点 + 偏移），跟 ui_polyline_append 的口径一致 */
  if (ListEmpty(&para->list))
  {
    base_x = (int16_t)(para->anchor_x + para->start.x);
    base_y = (int16_t)(para->anchor_y + para->start.y);
  }
  else
  {
    ui_poly_node_t* last = ListEntry(para->list.prev, ui_poly_node_t, node);
    base_x = (int16_t)(para->anchor_x + last->point.x);
    base_y = (int16_t)(para->anchor_y + last->point.y);
  }

  /* 增量 -> 绝对坐标，剩下完全复用绝对坐标版（内部再减锚点、存成偏移、标脏） */
  node->point.x = (int16_t)(base_x + dx);
  node->point.y = (int16_t)(base_y + dy);

  return ui_polyline_append(para, node);
}

ui_poly_node_t* ui_polyline_delete_last(ui_polyline_para_t* para)
{
  ui_poly_node_t* node;
  int16_t old_x_min, old_y_min, old_x_max, old_y_max;
  int16_t new_x_min, new_y_min, new_x_max, new_y_max;

  if (para == NULL || ListEmpty(&para->list))
  {
    return NULL;                     /* 链上没顶点，没段可删 */
  }

  ui_poly_rect_of(para, &old_x_min, &old_y_min, &old_x_max, &old_y_max);

  /* 环形双向链表：哨兵的 prev 就是链尾顶点 */
  node = ListEntry(para->list.prev, ui_poly_node_t, node);

  ListDel(&node->node);
  para->count--;

  ui_polyline_update_frame(para);

  ui_poly_rect_of(para, &new_x_min, &new_y_min, &new_x_max, &new_y_max);

  ui_dirty_mark(old_x_min, old_y_min, old_x_max, old_y_max);
  ui_dirty_mark(new_x_min, new_y_min, new_x_max, new_y_max);
  return node;
}

void ui_polyline_set_anchor(ui_polyline_para_t* para, int16_t ax, int16_t ay)
{
  ui_poly_node_t* node;
  int16_t old_ax, old_ay;
  int16_t old_x_min, old_y_min, old_x_max, old_y_max;
  int16_t new_x_min, new_y_min, new_x_max, new_y_max;

  if (para == NULL)
  {
    return;
  }

  ui_poly_rect_of(para, &old_x_min, &old_y_min, &old_x_max, &old_y_max);

  old_ax = para->anchor_x;
  old_ay = para->anchor_y;

  para->start.x = (int16_t)(old_ax + para->start.x - ax);
  para->start.y = (int16_t)(old_ay + para->start.y - ay);

  ListForEachEntry(node, &para->list, ui_poly_node_t, node)
  {
    node->point.x = (int16_t)(old_ax + node->point.x - ax);
    node->point.y = (int16_t)(old_ay + node->point.y - ay);
  }

  para->anchor_x = ax;
  para->anchor_y = ay;
  para->base_anchor_x = ax;      /* 设锚点即视为新的确定态锚点 */
  para->base_anchor_y = ay;

  ui_polyline_update_frame(para);

  ui_poly_rect_of(para, &new_x_min, &new_y_min, &new_x_max, &new_y_max);
  ui_dirty_mark(old_x_min, old_y_min, old_x_max, old_y_max);
  ui_dirty_mark(new_x_min, new_y_min, new_x_max, new_y_max);
}

void ui_polyline_bake(ui_polyline_para_t* para)
{
  ui_poly_node_t* node;
  uint16_t num, den;

  if (para == NULL)
  {
    return;
  }

  num = para->scale_num;
  den = (para->scale_den != 0u) ? para->scale_den : 1u;

  /* 偏移 ×= 因子：把"假"的当前比例落成确定态；随后复位因子 */
  para->start.x = (int16_t)((int32_t)para->start.x * num / den);
  para->start.y = (int16_t)((int32_t)para->start.y * num / den);
  ListForEachEntry(node, &para->list, ui_poly_node_t, node)
  {
    node->point.x = (int16_t)((int32_t)node->point.x * num / den);
    node->point.y = (int16_t)((int32_t)node->point.y * num / den);
  }

  para->scale_num = 1u;
  para->scale_den = 1u;
  para->base_anchor_x = para->anchor_x;
  para->base_anchor_y = para->anchor_y;

  ui_polyline_update_frame(para);
}

void ui_polyline_restore(ui_polyline_para_t* para)
{
  int16_t old_x_min, old_y_min, old_x_max, old_y_max;
  int16_t new_x_min, new_y_min, new_x_max, new_y_max;

  if (para == NULL)
  {
    return;
  }

  ui_poly_rect_of(para, &old_x_min, &old_y_min, &old_x_max, &old_y_max);

  para->anchor_x = para->base_anchor_x;
  para->anchor_y = para->base_anchor_y;
  para->scale_num = 1u;
  para->scale_den = 1u;

  ui_polyline_update_frame(para);

  ui_poly_rect_of(para, &new_x_min, &new_y_min, &new_x_max, &new_y_max);
  ui_dirty_mark(old_x_min, old_y_min, old_x_max, old_y_max);
  ui_dirty_mark(new_x_min, new_y_min, new_x_max, new_y_max);
}

/* ============================================================================
 * 合并对象操作（搬自 ui_create.c）
 * ========================================================================== */

void ui_union_update_frame(ui_union_para_t* para)
{
  ui_merge_obj_node_t* node;
  int16_t x_min = 0, y_min = 0, x_max = 0, y_max = 0;
  uint8_t has = 0;

  if (para == NULL)
  {
    return;
  }

  ListForEachEntry(node, &para->list, ui_merge_obj_node_t, node)
  {
    int16_t cx, cy, w, h, x0, y0, x1, y1;

    if (!ui_obj_frame(node->obj, &cx, &cy, &w, &h))
    {
      continue;                 /* 认不出的子对象不参与框 */
    }

    ui_box_of(cx, cy, w, h, &x0, &y0, &x1, &y1);

    if (has == 0u)
    {
      x_min = x0; x_max = x1; y_min = y0; y_max = y1; has = 1u;
    }
    else
    {
      if (x0 < x_min) { x_min = x0; }
      if (x1 > x_max) { x_max = x1; }
      if (y0 < y_min) { y_min = y0; }
      if (y1 > y_max) { y_max = y1; }
    }
  }

  if (has == 0u)                /* 空合并对象：框退化成 0，中心保持不动 */
  {
    para->w = 0;
    para->h = 0;
    return;
  }

  para->w = (int16_t)(x_max - x_min + 1);
  para->h = (int16_t)(y_max - y_min + 1);
  para->head.center_x = (int16_t)(x_min + (para->w >> 1));
  para->head.center_y = (int16_t)(y_min + (para->h >> 1));
}

uint8_t ui_union_append(ui_union_para_t* para, ui_merge_obj_node_t* node)
{
  ui_merge_obj_node_t* it;
  int16_t cx, cy, w, h;
  int16_t old_x_min = 0, old_y_min = 0, old_x_max = 0, old_y_max = 0;
  int16_t new_x_min, new_y_min, new_x_max, new_y_max;
  uint8_t had_old;

  if (para == NULL || node == NULL || node->obj == NULL || node->obj->para == NULL)
  {
    return 0;
  }

  /* 光标不做 union 子对象：它没有自带基准 */
  if (((ui_head_t*)node->obj->para)->type == UI_TYPE_CURSOR)
  {
    return 0;
  }

  if (!ui_obj_frame(node->obj, &cx, &cy, &w, &h))
  {
    return 0;                   /* 子对象类型不认识 / 取不到框：不挂 */
  }

  had_old = (uint8_t)(para->w > 0 && para->h > 0);
  if (had_old != 0u)
  {
    ui_box_of(para->head.center_x, para->head.center_y, para->w, para->h,
              &old_x_min, &old_y_min, &old_x_max, &old_y_max);
  }

  /* 给子对象种自带基准：让它自带的基准 = 成组这一刻的几何。
   * 之后 union 缩放就从这份自带基准现算，无需在节点里另存尺寸 */
  ui_obj_bake(node->obj);

  ListAddTail(&para->list, &node->node);

  ui_union_update_frame(para);   /* 先量出并集框（此刻就是基准框） */

  /* 按当前（未缩放）几何，重算所有子对象相对锚点的位置基准 */
  ListForEachEntry(it, &para->list, ui_merge_obj_node_t, node)
  {
    int16_t ccx, ccy, cw, ch;

    if (ui_obj_frame(it->obj, &ccx, &ccy, &cw, &ch))
    {
      it->base_rel_x = (int16_t)(ccx - para->head.center_x);
      it->base_rel_y = (int16_t)(ccy - para->head.center_y);
    }
  }

  /* 锚点 = 当前框中心；之后它只随整体平移走，缩放绕它进行、不动它。
   * 基准锚点同值，供复位读回 */
  para->anchor_x = para->head.center_x;
  para->anchor_y = para->head.center_y;
  para->base_anchor_x = para->anchor_x;
  para->base_anchor_y = para->anchor_y;

  ui_box_of(para->head.center_x, para->head.center_y, para->w, para->h,
            &new_x_min, &new_y_min, &new_x_max, &new_y_max);
  if (had_old != 0u)
  {
    ui_dirty_mark(old_x_min, old_y_min, old_x_max, old_y_max);
  }
  ui_dirty_mark(new_x_min, new_y_min, new_x_max, new_y_max);
  return 1;
}

/* ============================================================================
 * 视窗操作
 * ========================================================================== */

uint8_t ui_viewport_append(ui_viewport_para_t* para, ui_merge_obj_node_t* node)
{
  ui_merge_obj_node_t* it;
  int16_t cx, cy, w, h;
  int16_t x_min, y_min, x_max, y_max;

  if (para == NULL || node == NULL || node->obj == NULL || node->obj->para == NULL)
  {
    return 0;
  }

  /* 光标不做容器子对象：它没有自带基准，缩不回去 */
  if (((ui_head_t*)node->obj->para)->type == UI_TYPE_CURSOR)
  {
    return 0;
  }

  if (!ui_obj_frame(node->obj, &cx, &cy, &w, &h))
  {
    return 0;                   /* 子对象类型不认识 / 取不到框：不挂 */
  }

  /* 第一次挂子对象：锚点默认设成窗口中心（缩放绕中心，和合并对象一致）；
   * 想换缩放原点，先挂完再调 ui_obj_set_anchor（它会同步把子对象基准偏移挪好） */
  if (ListEmpty(&para->list))
  {
    para->anchor_x = para->head.center_x;
    para->anchor_y = para->head.center_y;
    para->base_anchor_x = para->anchor_x;
    para->base_anchor_y = para->anchor_y;
  }

  /* 给子对象种自带基准：让它自带的基准 = 挂进来这一刻的几何 */
  ui_obj_bake(node->obj);

  ListAddTail(&para->list, &node->node);

  /* 按当前几何，重算所有子对象相对视窗锚点的位置基准（供整体缩放从基准现算） */
  ListForEachEntry(it, &para->list, ui_merge_obj_node_t, node)
  {
    int16_t ccx, ccy, cw, ch;

    if (ui_obj_frame(it->obj, &ccx, &ccy, &cw, &ch))
    {
      it->base_rel_x = (int16_t)(ccx - para->anchor_x);
      it->base_rel_y = (int16_t)(ccy - para->anchor_y);
    }
  }

  /* 视窗内容变了：整块窗口标脏重画（窗口尺寸由调用方给定，不重算） */
  if (para->w > 0 && para->h > 0)
  {
    ui_box_of(para->head.center_x, para->head.center_y, para->w, para->h,
              &x_min, &y_min, &x_max, &y_max);
    ui_dirty_mark(x_min, y_min, x_max, y_max);
  }
  return 1;
}

/* 开一段"标脏裁剪"：把视窗窗口矩形压成标脏裁剪区，这段里 ui_dirty_mark 都夹到窗口内。
 * 给"在视窗里滚动 / 驱动子对象动画"用：子对象只画在窗口内，窗口外没有变化，
 * 不用为它标脏 —— 省掉滚出窗口那部分的推屏量。必须和 ui_viewport_dirty_end 成对 */
void ui_viewport_dirty_begin(const ui_viewport_para_t* para)
{
  int16_t x_min, y_min, x_max, y_max;

  ui_box_of(para->head.center_x, para->head.center_y, para->w, para->h,
            &x_min, &y_min, &x_max, &y_max);
  ui_dirty_clip_push(x_min, y_min, x_max, y_max);
}

void ui_viewport_dirty_end(void)
{
  ui_dirty_clip_pop();
}
