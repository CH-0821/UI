/**
 ****************************************************************************************************
 * @file        ui_module.c
 * @brief       UI 组件层：按钮 / 多档滑块(兼滚动条)
 * @note        组件约定(怎么加新组件)见 ui_module.h 文件头
 ****************************************************************************************************
 */

#include "./ui_module/ui_module.h"
#include "./ui_anim/ui_anim.h"            /* ui_anim_has_move / ui_anim_move_obj_to：吸附动画(挂载式) */
#include "./ui_control/ui_control.h"      /* ui_obj_set_anchor：滑块平移动画要先有锚点 */

#include <string.h>                       /* strlen：ui_button_set_text 传 0 时按它算长度 */

#if UI_DEBUG_HUD
#include <stdio.h>                        /* snprintf：格式化 HUD 的两行文字 */
#include "./ui_event/ui_event.h"          /* ui_event_last_touch：最近触点(调试 HUD 用) */
#endif

/* ==================================================================
 *  小工具
 * ================================================================== */

/**
 * @brief       读一个"盒子型"零件的几何(中心 + 宽高)
 * @note        矩形 / 图片 / 文本三种 para 的前缀同布局(head + xf + base_w/base_h + w/h)，
 *              按矩形口径读即可(组件约定第 3 条)；圆 / 进度条没有 w/h，不能当零件
 */
static void ui_mod_box_get(const ui_object_t *o, int16_t *cx, int16_t *cy, int16_t *w, int16_t *h)
{
  const ui_head_t      *head = (const ui_head_t *)o->para;
  const ui_rect_para_t *box  = (const ui_rect_para_t *)o->para;

  *cx = head->center_x;
  *cy = head->center_y;
  *w  = box->w;
  *h  = box->h;
}

/* ==================================================================
 *  档位 <-> 几何换算
 * ================================================================== */

/* 档位 -> 滑块中心(轴向)：行程按档数均分，0 档贴 c_min、最后一档贴 c_max */
static int16_t ui_slider_center_of(const ui_slider_para_t *s, uint8_t v)
{
  int32_t span = (int32_t)s->c_max - (int32_t)s->c_min;

  if (span <= 0 || s->segments <= 1u)
  {
    return s->c_min;
  }
  return (int16_t)((int32_t)s->c_min + span * (int32_t)v / (int32_t)(s->segments - 1u));
}

/* 轴向位置 -> 最近档位(四舍五入 + 夹取) */
static uint8_t ui_slider_value_of(const ui_slider_para_t *s, int16_t c)
{
  int32_t span = (int32_t)s->c_max - (int32_t)s->c_min;
  int32_t v;

  if (span <= 0 || s->segments <= 1u)
  {
    return 0u;
  }
  v = ((int32_t)(c - s->c_min) * (int32_t)(s->segments - 1u) + span / 2) / span;
  if (v < 0)
  {
    v = 0;
  }
  if (v > (int32_t)s->segments - 1)
  {
    v = (int32_t)s->segments - 1;
  }
  return (uint8_t)v;
}

/* ==================================================================
 *  滑块落位(动画接口留在这一个点：要吸附动画就把它换成 ui_anim 的定点移动)
 * ================================================================== */

/* 把滑块挪到指定中心(轴向)：直接写中心，老框 ∪ 新框各标一次脏(精确，不整块刷) */
static void ui_slider_place(ui_slider_para_t *s, int16_t center)
{
  ui_head_t      *th;
  ui_rect_para_t *tb;
  int16_t ocx, ocy, ncx, ncy;
  int16_t ax0, ay0, ax1, ay1;
  int16_t bx0, by0, bx1, by1;

  if (s->thumb == NULL || s->thumb->para == NULL)
  {
    return;
  }
  th = (ui_head_t *)s->thumb->para;
  tb = (ui_rect_para_t *)s->thumb->para;

  ocx = th->center_x;
  ocy = th->center_y;
  ncx = (s->horizontal != 0u) ? center : ocx;
  ncy = (s->horizontal != 0u) ? ocy : center;
  if (ncx == ocx && ncy == ocy)
  {
    return;                                    /* 没动：一个像素都没变，不标脏 */
  }

  th->center_x = ncx;
  th->center_y = ncy;

  ui_box_of(ocx, ocy, tb->w, tb->h, &ax0, &ay0, &ax1, &ay1);
  ui_box_of(ncx, ncy, tb->w, tb->h, &bx0, &by0, &bx1, &by1);
  ui_dirty_mark(ax0, ay0, ax1, ay1);
  ui_dirty_mark(bx0, by0, bx1, by1);
}

/* 吸附落位(动画版 / 无动画版自适应)：滑块注册过平移动画 -> 记下目标交给 ui_slider_task
 * 每帧推(动画版)；没注册 -> 一步到位(无动画版)。
 * 只在"松手吸附"和"程序设定(ui_slider_set_value)"走这里 —— 拖动跟手永远走 place(即时，不能有延迟)。
 * 为什么不在这儿直接调 ui_anim_move_obj_to 完事：ui_anim 的定时移动是轮询式的 ——
 * 每帧调一次才走一步，调一次就停，所以动画必须有每帧的驱动(ui_slider_task) */
static void ui_slider_settle(ui_slider_para_t *s, int16_t center)
{
  if (s->thumb == NULL || s->thumb->para == NULL)
  {
    return;
  }

  if (ui_anim_has_move(s->thumb))
  {
    s->settle_c = center;
    s->settling = 1u;                          /* 剩下的交给 ui_slider_task 每帧推 */
    return;
  }

  ui_slider_place(s, center);
}

/* 拖动：滑块中心跟到手指(夹在行程里)，档位变了就记下并通知 */
static void ui_slider_drag(ui_object_t *obj, ui_slider_para_t *s, int16_t axis)
{
  int16_t center = (int16_t)(axis - s->grab_off);
  uint8_t v;

  if (center < s->c_min)
  {
    center = s->c_min;
  }
  if (center > s->c_max)
  {
    center = s->c_max;
  }

  ui_slider_place(s, center);

  v = ui_slider_value_of(s, center);
  if (v != s->value)
  {
    s->value = v;
    if (s->on_change != NULL)
    {
      s->on_change(obj, v);
    }
  }
}

/* ==================================================================
 *  绘制 / 事件
 * ================================================================== */

/* 组件的绘制回调：先槽后滑块，零件由这里带画(它们不在任何绘制表里，z 序天然正确) */
static void ui_slider_draw(void *p)
{
  ui_slider_para_t *s = (ui_slider_para_t *)p;

  if (s->track != NULL && s->track->para != NULL)
  {
    ui_draw_dispatch(s->track->para);
  }
  if (s->thumb != NULL && s->thumb->para != NULL)
  {
    ui_draw_dispatch(s->thumb->para);
  }
}

/* 组件的事件回调：整块控件一个拖动语义(零件没挂回调，命中全落到组件上) */
static uint8_t ui_slider_cb(ui_object_t *obj, const ui_event_t *ev)
{
  ui_slider_para_t *s = (ui_slider_para_t *)obj->para;
  ui_rect_para_t   *tb;
  ui_head_t        *th;
  int16_t axis, center, half;

  if (s == NULL || s->thumb == NULL || s->thumb->para == NULL)
  {
    return 0;
  }
  th = (ui_head_t *)s->thumb->para;
  tb = (ui_rect_para_t *)s->thumb->para;

  switch (ev->type)
  {
    case UI_EV_DOWN:
      s->grabbing = 1u;
      s->settling = 0u;                          /* 手按上来了：没吸完的吸附作废，跟手要紧 */
      axis   = (s->horizontal != 0u) ? ev->x : ev->y;
      center = (s->horizontal != 0u) ? th->center_x : th->center_y;
      half   = (s->horizontal != 0u) ? (int16_t)(tb->w >> 1) : (int16_t)(tb->h >> 1);
      /* 按在滑块上：记下偏移，从按住的地方拖(不掉手)；按在槽上：中心直接跟到手指 */
      s->grab_off = ((axis >= center - half) && (axis <= center + half))
                  ? (int16_t)(axis - center) : 0;
      ui_slider_drag(obj, s, axis);
      return 1u;

    case UI_EV_MOVE:
      if (s->grabbing != 0u)
      {
        axis = (s->horizontal != 0u) ? ev->x : ev->y;
        ui_slider_drag(obj, s, axis);
      }
      return 1u;                                 /* 不拖也吃掉：整块控件自己管手势 */

    case UI_EV_UP:
      if (s->grabbing != 0u)
      {
        s->grabbing = 0u;
        ui_slider_settle(s, ui_slider_center_of(s, s->value));  /* 松手吸附到最近档(有动画就滑过去) */
      }
      return 1u;

    case UI_EV_CANCEL:
      s->grabbing = 0u;                          /* 移出后松手：停住，不吸附(没在拖了) */
      return 1u;

    default:                                     /* CLICK 已被 DOWN/UP 覆盖；长按不认 */
      return 1u;
  }
}

/* ==================================================================
 *  对外接口
 * ================================================================== */

void ui_slider_init(ui_object_t *obj, ui_slider_para_t *para,
                    ui_object_t *track, ui_object_t *thumb,
                    uint8_t segments, uint8_t horizontal, uint8_t travel,
                    uint8_t value_init)
{
  int16_t tcx, tcy, tw, th;
  int16_t hcx, hcy, hw, hh;
  int16_t t_edge0, t_edge1;                      /* 槽在轴向上的起/止边 */
  int16_t e_x0, e_y0, e_x1, e_y1;                /* 滑块极端框(并集用临时) */
  int16_t X0, Y0, X1, Y1;                        /* 并集 */
  int16_t c_min, c_max;
  ui_head_t *th_head;

  if (obj == NULL || para == NULL ||
      track == NULL || track->para == NULL ||
      thumb == NULL || thumb->para == NULL)
  {
    return;
  }
  if (segments < 2u)
  {
    segments = 2u;                               /* 一档构不成"滑块" */
  }

  ui_mod_box_get(track, &tcx, &tcy, &tw, &th);
  ui_mod_box_get(thumb, &hcx, &hcy, &hw, &hh);

  /* ---- 滑块中心的行程端点：按行程模型算(轴向：槽的几何中心 ± 半宽/半高得两条边) ---- */
  if (horizontal != 0u)
  {
    t_edge0 = (int16_t)(tcx - (tw >> 1));        /* 槽起点边 */
    t_edge1 = (int16_t)(t_edge0 + tw - 1);       /* 槽终点边 */
  }
  else
  {
    t_edge0 = (int16_t)(tcy - (th >> 1));
    t_edge1 = (int16_t)(t_edge0 + th - 1);
  }

  if (travel == UI_SLIDER_CENTER)
  {
    c_min = t_edge0;                             /* 中心走槽：从起点边走到终点边， */
    c_max = t_edge1;                             /* 两端自然突出半个滑块(拉杆图，滑块比槽大也行) */
  }
  else
  {
    c_min = (int16_t)(t_edge0 + ((horizontal != 0u) ? (hw >> 1) : (hh >> 1)));  /* 整颗缩在槽里： */
    c_max = (int16_t)(t_edge1 + 1 - ((horizontal != 0u) ? (hw >> 1) : (hh >> 1)));  /* 两端让出半滑块 */
  }
  if (c_min > c_max)                             /* 只在 INSIDE 且滑块 >= 槽时可能：塞不下就是塞不下 */
  {
    c_min = (horizontal != 0u) ? tcx : tcy;
    c_max = c_min;
  }

  /* ---- 填 para：先组件框(type/draw)，再滑块自己的字段 ---- */
  para->box.head.type = UI_TYPE_MODULE;
  para->box.head.draw = ui_slider_draw;
  para->track      = track;
  para->thumb      = thumb;
  para->segments   = segments;
  para->horizontal = (horizontal != 0u) ? 1u : 0u;
  para->travel     = (travel == UI_SLIDER_CENTER) ? UI_SLIDER_CENTER : UI_SLIDER_INSIDE;
  para->grabbing   = 0u;
  para->grab_off   = 0;
  para->c_min      = c_min;
  para->c_max      = c_max;
  para->value      = 0u;
  para->on_change  = NULL;                       /* 调用方装配后按需挂 */

  /* ---- 组件外接框 = 槽 ∪ 滑块在行程两端的极端框(滑块比槽大也罩得住) ----
   *      框架按矩形口径读它(命中 / 显隐标脏)，组件几何变了要自己维护这四个数 */
  ui_box_of(tcx, tcy, tw, th, &X0, &Y0, &X1, &Y1);
  ui_box_of((horizontal != 0u) ? c_min : hcx,
            (horizontal != 0u) ? hcy : c_min, hw, hh, &e_x0, &e_y0, &e_x1, &e_y1);
  if (e_x0 < X0) { X0 = e_x0; }
  if (e_y0 < Y0) { Y0 = e_y0; }
  if (e_x1 > X1) { X1 = e_x1; }
  if (e_y1 > Y1) { Y1 = e_y1; }
  ui_box_of((horizontal != 0u) ? c_max : hcx,
            (horizontal != 0u) ? hcy : c_max, hw, hh, &e_x0, &e_y0, &e_x1, &e_y1);
  if (e_x0 < X0) { X0 = e_x0; }
  if (e_y0 < Y0) { Y0 = e_y0; }
  if (e_x1 > X1) { X1 = e_x1; }
  if (e_y1 > Y1) { Y1 = e_y1; }

  para->box.w = para->box.base_w = (int16_t)(X1 - X0 + 1);
  para->box.h = para->box.base_h = (int16_t)(Y1 - Y0 + 1);
  para->box.head.center_x = (int16_t)(X0 + (para->box.w >> 1));
  para->box.head.center_y = (int16_t)(Y0 + (para->box.h >> 1));
  para->box.xf.scale_num = 1u;                   /* para 静态清零后 scale_den 是 0，先扶正 */
  para->box.xf.scale_den = 1u;

  /* ---- 滑块摆到初值 + 设锚点：init 阶段直接写中心、不标脏(还没进屏) ----
   *      锚点必须设：ui_anim 的平移按"锚点 + 偏移"算中心，没锚点一起跑就会闪到 (0,0) */
  if ((unsigned)value_init > (unsigned)(segments - 1u))
  {
    value_init = (uint8_t)(segments - 1u);
  }
  para->value = value_init;
  th_head = (ui_head_t *)thumb->para;
  if (horizontal != 0u)
  {
    th_head->center_x = ui_slider_center_of(para, value_init);
  }
  else
  {
    th_head->center_y = ui_slider_center_of(para, value_init);
  }
  ui_obj_set_anchor(thumb, th_head->center_x, th_head->center_y);

  /* ---- 接上组件对象：cb 非空 = 可被命中(整块控件一个交互语义) ----
   *      对象本身由调用方 UI_InitObject + 注册进链/层级(和普通对象同一套显隐) */
  obj->para = para;                              /* 首成员就是 box，head 指针 = para 指针 */
  obj->cb   = ui_slider_cb;
}

uint8_t ui_slider_get_value(const ui_object_t *obj)
{
  if (obj == NULL || obj->para == NULL)
  {
    return 0u;
  }
  return ((const ui_slider_para_t *)obj->para)->value;
}

void ui_slider_set_value(ui_object_t *obj, uint8_t value)
{
  ui_slider_para_t *s;

  if (obj == NULL || obj->para == NULL)
  {
    return;
  }
  s = (ui_slider_para_t *)obj->para;

  if ((unsigned)value > (unsigned)(s->segments - 1u))
  {
    value = (uint8_t)(s->segments - 1u);
  }
  s->value = value;
  ui_slider_settle(s, ui_slider_center_of(s, value));  /* 有动画就滑过去；不发 on_change：程序设定不算"用户改了" */
}

void ui_slider_task(ui_object_t *obj)
{
  ui_slider_para_t *s;
  ui_head_t        *th;
  int16_t cx, cy;

  if (obj == NULL || obj->para == NULL)
  {
    return;
  }
  s = (ui_slider_para_t *)obj->para;

  if (s->settling == 0u || s->thumb == NULL || s->thumb->para == NULL)
  {
    return;                                      /* 没在吸附中：每帧的空转，直接走人 */
  }
  th = (ui_head_t *)s->thumb->para;

  /* 到位(滑块中心 == 吸附目标)：收摊。判据用轴向中心，浮点都没有，不会抖 */
  if (((s->horizontal != 0u) ? th->center_x : th->center_y) == s->settle_c)
  {
    s->settling = 0u;
    return;
  }

  /* 同目标重复调 = "接着走"(ui_anim 的约定)：每帧逼近一点，到时停准 */
  cx = (s->horizontal != 0u) ? s->settle_c : th->center_x;
  cy = (s->horizontal != 0u) ? th->center_y : s->settle_c;
  ui_anim_move_obj_to(s->thumb, cx, cy, UI_SLIDER_SNAP_MS, UI_SLIDER_SNAP_EASE);
}

/* ==================================================================
 *  按钮
 * ================================================================== */

/* 整个外接框标脏：按钮的外观变化(换色 / 文字位移)都发生在这个框里 */
static void ui_button_mark(const ui_button_para_t *b)
{
  int16_t x0, y0, x1, y1;

  ui_box_of(b->box.head.center_x, b->box.head.center_y,
            b->box.w, b->box.h, &x0, &y0, &x1, &y1);
  ui_dirty_mark(x0, y0, x1, y1);
}

/* 把当前状态(按下 / 选中)刷到零件上：底色 + 文字位移，然后标脏 */
static void ui_button_apply(ui_button_para_t *b)
{
  ui_color_t c = b->c_normal;
  int16_t    dy = 0;
  ui_head_t *bh;
  ui_head_t *lh;

  if (b->pressed != 0u)
  {
    if (b->c_pressed != 0u)
    {
      c = b->c_pressed;
    }
    else if (b->checked != 0u && b->c_checked != 0u)
    {
      c = b->c_checked;                          /* 没配按下色：选中态按下仍保持选中色，别闪回常态 */
    }
    dy = b->press_dy;
  }
  else if (b->checked != 0u && b->c_checked != 0u)
  {
    c = b->c_checked;
  }

  /* 底色：只接管矩形底，图片 / 文本底不动 */
  if (b->bg != NULL && b->bg->para != NULL)
  {
    bh = (ui_head_t *)b->bg->para;
    if (bh->type == UI_TYPE_RECT && c != 0u)
    {
      ((ui_rect_para_t *)bh)->color = c;
    }
  }

  /* 文字位移：常态中心是 init 记下的 label_cy，按下加 dy，松开自然回位。
   * 另外两个状态标签也用各自记的基准一起移：同一时刻只画一个，来回切不会错位 */
  if (b->label != NULL && b->label->para != NULL)
  {
    lh = (ui_head_t *)b->label->para;
    lh->center_y = (int16_t)(b->label_cy + dy);
  }
  if (b->label_pressed != NULL && b->label_pressed->para != NULL)
  {
    lh = (ui_head_t *)b->label_pressed->para;
    lh->center_y = (int16_t)(b->label_pressed_cy + dy);
  }
  if (b->label_checked != NULL && b->label_checked->para != NULL)
  {
    lh = (ui_head_t *)b->label_checked->para;
    lh->center_y = (int16_t)(b->label_checked_cy + dy);
  }

  ui_button_mark(b);
}

/* 组件绘制：先底后文字(零件不进绘制表，z 序由这里定)。
 * 零件自己的 activation 也要认：显隐零件(比如"选中才有底")是组件的常见用法；
 * 标签按状态挑一个画：按下(label_pressed) > 选中(label_checked) > 常态(label)，
 * 对应状态没挂标签就回落到常态的那个；都没有就只画底 */
static void ui_button_draw(void *p)
{
  ui_button_para_t *b = (ui_button_para_t *)p;
  ui_object_t      *lab;

  if (b->bg != NULL && b->bg->para != NULL && b->bg->activation == activated)
  {
    ui_draw_dispatch(b->bg->para);
  }

  lab = b->label;
  if (b->checked != 0u && b->label_checked != NULL)
  {
    lab = b->label_checked;
  }
  if (b->pressed != 0u && b->label_pressed != NULL)
  {
    lab = b->label_pressed;
  }
  if (lab != NULL && lab->para != NULL && lab->activation == activated)
  {
    ui_draw_dispatch(lab->para);
  }
}

/* 组件事件：DOWN/UP 只管外观，CLICK 才发通知；自锁按钮在 CLICK 里翻 checked */
static uint8_t ui_button_cb(ui_object_t *obj, const ui_event_t *ev)
{
  ui_button_para_t *b = (ui_button_para_t *)obj->para;

  if (b == NULL)
  {
    return 0u;
  }

  switch (ev->type)
  {
    case UI_EV_DOWN:
      if (b->enabled != 0u && b->pressed == 0u)
      {
        b->pressed = 1u;
        ui_button_apply(b);
      }
      return 1u;                                 /* 不响应也吃掉：按钮的手势不往下漏 */

    case UI_EV_UP:
    case UI_EV_CANCEL:
      if (b->pressed != 0u)
      {
        b->pressed = 0u;
        ui_button_apply(b);
      }
      return 1u;

    case UI_EV_CLICK:
      if (b->enabled == 0u)
      {
        return 1u;
      }
      if (b->toggle != 0u)
      {
        b->checked = (b->checked != 0u) ? 0u : 1u;
        ui_button_apply(b);
      }
      if (b->on_click != NULL)
      {
        b->on_click(obj);                        /* 回调里读 checked 就是翻过之后的状态 */
      }
      return 1u;

    default:
      return 1u;
  }
}

void ui_button_init(ui_object_t *obj, ui_button_para_t *para,
                    ui_object_t *bg, ui_object_t *label,
                    int16_t cx, int16_t cy, int16_t w, int16_t h)
{
  ui_head_t *lh;

  if (obj == NULL || para == NULL)
  {
    return;
  }

  /* 组件外接框：框架按矩形口径读它(命中 / 显隐标脏 / 命中调试) */
  para->box.head.type     = UI_TYPE_MODULE;
  para->box.head.draw     = ui_button_draw;
  para->box.head.center_x = cx;
  para->box.head.center_y = cy;
  para->box.w             = w;
  para->box.h             = h;
  para->box.base_w        = w;
  para->box.base_h        = h;
  para->box.xf.scale_num  = 1u;                  /* para 静态清零后 scale_den 是 0，先扶正 */
  para->box.xf.scale_den  = 1u;

  para->bg    = bg;
  para->label = label;

  /* 常态色没指定就沿用矩形底当前的颜色：老代码把颜色写在底上，接过来外观不变 */
  if (para->c_normal == 0u && bg != NULL && bg->para != NULL &&
      ((ui_head_t *)bg->para)->type == UI_TYPE_RECT)
  {
    para->c_normal = ((ui_rect_para_t *)bg->para)->color;
  }

  /* 运行态清零；配置字段(颜色 / press_dy / toggle / label_pressed / on_click)留给调用方 */
  para->enabled  = 1u;
  para->pressed  = 0u;
  para->checked  = 0u;
  para->label_cy = 0;
  if (label != NULL && label->para != NULL)
  {
    lh = (ui_head_t *)label->para;
    para->label_cy = lh->center_y;
  }

  /* 按下态标签的位移基准(没挂就是 0)。指针不清：调用方要在 init 之前挂好 */
  para->label_pressed_cy = 0;
  if (para->label_pressed != NULL && para->label_pressed->para != NULL)
  {
    lh = (ui_head_t *)para->label_pressed->para;
    para->label_pressed_cy = lh->center_y;
  }

  /* 选中态标签的位移基准：同上，指针也留给调用方在 init 前挂 */
  para->label_checked_cy = 0;
  if (para->label_checked != NULL && para->label_checked->para != NULL)
  {
    lh = (ui_head_t *)para->label_checked->para;
    para->label_checked_cy = lh->center_y;
  }

  /* 接上组件对象：cb 非空 = 可被命中(整块控件一个交互语义) */
  obj->para = para;
  obj->cb   = ui_button_cb;
}

void ui_button_set_text(ui_object_t *obj, const char *txt, uint16_t len)
{
  ui_button_para_t *b;
  ui_text_para_t   *tp;

  if (obj == NULL || obj->para == NULL)
  {
    return;
  }
  b = (ui_button_para_t *)obj->para;
  if (b->label == NULL || b->label->para == NULL)
  {
    return;
  }
  if (((ui_head_t *)b->label->para)->type != UI_TYPE_TEXT)
  {
    return;                                      /* 标签不是文本：没有可换的字 */
  }

  if (txt == NULL)
  {
    txt = "";
    len = 0u;
  }
  if (len == 0u)
  {
    len = (uint16_t)strlen(txt);                 /* 只存指针不拷贝：内容要一直有效 */
  }

  tp = (ui_text_para_t *)b->label->para;
  tp->text     = (const uint8_t *)txt;
  tp->text_len = len;
  ui_button_mark(b);
}

void ui_button_set_enabled(ui_object_t *obj, uint8_t on)
{
  ui_button_para_t *b;

  if (obj == NULL || obj->para == NULL)
  {
    return;
  }
  b  = (ui_button_para_t *)obj->para;
  on = (on != 0u) ? 1u : 0u;

  if (b->enabled == on)
  {
    return;
  }
  b->enabled = on;
  if (on == 0u)
  {
    b->pressed = 0u;                             /* 变灰时手还按着：先把按下态收掉 */
  }
  ui_button_apply(b);
}

void ui_button_set_checked(ui_object_t *obj, uint8_t on)
{
  ui_button_para_t *b;

  if (obj == NULL || obj->para == NULL)
  {
    return;
  }
  b  = (ui_button_para_t *)obj->para;
  on = (on != 0u) ? 1u : 0u;

  if (b->checked == on)
  {
    return;
  }
  b->checked = on;
  ui_button_apply(b);
}

uint8_t ui_button_get_checked(const ui_object_t *obj)
{
  if (obj == NULL || obj->para == NULL)
  {
    return 0u;
  }
  return ((const ui_button_para_t *)obj->para)->checked;
}

/* ==================================================================
 *  表格
 * ================================================================== */

/* 合并块夹进网格：合法返回 1 并给结束行列(不含)；越界 / 零跨度返回 0 */
static uint8_t ui_table_merge_range(const ui_table_para_t *t, const ui_table_merge_t *m, uint8_t *r_end, uint8_t *c_end)
{
  uint16_t re, ce;

  if (m->row >= t->rows || m->col >= t->cols ||
      m->row_span == 0u || m->col_span == 0u)
  {
    return 0u;
  }

  re = (uint16_t)m->row + m->row_span;
  ce = (uint16_t)m->col + m->col_span;
  if (re > t->rows) { re = t->rows; }
  if (ce > t->cols) { ce = t->cols; }

  *r_end = (uint8_t)re;
  *c_end = (uint8_t)ce;
  return 1u;
}

/* (r,c) 是不是"要画的格子头"：返回 1 = 画(并报跨度)，0 = 被合并块盖住，跳过 */
static uint8_t ui_table_head_of(const ui_table_para_t *t, uint8_t r, uint8_t c,
                                uint8_t *rs, uint8_t *cs)
{
  uint8_t k, r_end, c_end;

  *rs = 1u;
  *cs = 1u;

  for (k = 0u; k < t->merge_num; k++)
  {
    const ui_table_merge_t *m = &t->merges[k];

    if (ui_table_merge_range(t, m, &r_end, &c_end) == 0u)
    {
      continue;
    }
    if ((uint16_t)r >= m->row && (uint16_t)r < r_end &&
        (uint16_t)c >= m->col && (uint16_t)c < c_end)
    {
      if (r != m->row || c != m->col)
      {
        return 0u;                              /* 被合并块盖住 */
      }
      *rs = (uint8_t)(r_end - m->row);
      *cs = (uint8_t)(c_end - m->col);
      return 1u;
    }
  }

  return 1u;                                    /* 不在任何合并块里：自己就是 1×1 的头 */
}

/* (row,col) 属于哪个格子头 + 跨度：合并块内部任意位置都映射回左上角 */
static void ui_table_owner_of(const ui_table_para_t *t, uint8_t row, uint8_t col,
                              uint8_t *hr, uint8_t *hc, uint8_t *rs, uint8_t *cs)
{
  uint8_t k, r_end, c_end;

  *hr = row;
  *hc = col;
  *rs = 1u;
  *cs = 1u;

  for (k = 0u; k < t->merge_num; k++)
  {
    const ui_table_merge_t *m = &t->merges[k];

    if (ui_table_merge_range(t, m, &r_end, &c_end) == 0u)
    {
      continue;
    }
    if ((uint16_t)row >= m->row && (uint16_t)row < r_end &&
        (uint16_t)col >= m->col && (uint16_t)col < c_end)
    {
      *hr = m->row;
      *hc = m->col;
      *rs = (uint8_t)(r_end - m->row);
      *cs = (uint8_t)(c_end - m->col);
      return;
    }
  }
}

/* 格子范围标脏：改文字只动这一格(含合并块整块)，网格线和别的格子不动 */
static void ui_table_cell_dirty(const ui_table_para_t *t, uint8_t r, uint8_t c,
                                uint8_t rs, uint8_t cs)
{
  int16_t x0 = (int16_t)(t->left + (int16_t)c * t->cell_w);
  int16_t y0 = (int16_t)(t->top  + (int16_t)r * t->cell_h);
  int16_t w  = (int16_t)((int16_t)cs * t->cell_w + 1);
  int16_t h  = (int16_t)((int16_t)rs * t->cell_h + 1);

  ui_dirty_mark(x0, y0, (int16_t)(x0 + w - 1), (int16_t)(y0 + h - 1));
}

/* 栈上临时矩形 para：只给 ui_draw_rect 用，不注册、不进链 */
static void ui_table_rect_at(ui_rect_para_t *p, int16_t x0, int16_t y0, int16_t w, int16_t h,
                             ui_color_t color, uint8_t border, uint8_t alpha)
{
  p->head.type     = UI_TYPE_RECT;
  p->head.draw     = ui_draw_rect;
  p->head.center_x = (int16_t)(x0 + (w >> 1));
  p->head.center_y = (int16_t)(y0 + (h >> 1));
  p->w = p->base_w = w;
  p->h = p->base_h = h;
  p->border_width  = border;
  p->color         = color;
  p->fill          = 0u;
  p->corners       = 0u;
  p->radius        = 0u;
  p->alpha         = alpha;                 /* 网格线跟随组件的 box.alpha */
  p->xf.scale_num  = 1u;
  p->xf.scale_den  = 1u;
}

/* 栈上临时文本 para：文字在给定带里横竖居中 */
static void ui_table_text_at(ui_text_para_t *p, int16_t x0, int16_t y0, int16_t w, int16_t h,
                             const char *s, uint16_t len, const ui_table_para_t *t)
{
  p->head.type     = UI_TYPE_TEXT;
  p->head.draw     = ui_draw_text;
  p->head.center_x = (int16_t)(x0 + (w >> 1));
  p->head.center_y = (int16_t)(y0 + (h >> 1));
  p->w = p->base_w = w;
  p->h = p->base_h = h;
  p->text          = (const uint8_t *)s;
  p->text_len      = len;
  p->font_size     = t->font_size;
  p->line_spacing  = 0u;                        /* 段内行距由文本自己按 \n 排 */
  p->color         = t->text_color;
  p->align         = UI_TEXT_ALIGN_CENTER;
  p->valign        = UI_TEXT_VALIGN_CENTER;
  p->hz_font_id    = t->font_id;
  p->hz_glyph_fn   = t->hz_glyph_fn;
  p->alpha         = t->box.alpha;             /* 文字跟随组件的 box.alpha */
  p->xf.scale_num  = 1u;
  p->xf.scale_den  = 1u;
}

/* 组件绘制：一趟走完每个"格子头" —— 先画它的网格框，再画格内文字。
 * 网格框按格子画(四边)，合并块就是一个跨行跨列的大格子：框按跨度撑大，
 * 被它盖住的格子整格跳过 */
static void ui_table_draw(void *p)
{
  ui_table_para_t *t = (ui_table_para_t *)p;
  ui_rect_para_t   r;
  ui_text_para_t   x;
  uint8_t rr, cc, i, n, band, rs, cs;
  int16_t x0, y0, w, h, band_h, ty;
  uint16_t base;

  if (t == NULL || t->rows == 0u || t->cols == 0u)
  {
    return;
  }

  for (rr = 0u; rr < t->rows; rr++)
  {
    for (cc = 0u; cc < t->cols; cc++)
    {
      if (ui_table_head_of(t, rr, cc, &rs, &cs) == 0u)
      {
        continue;                               /* 被合并块盖住：不画 */
      }

      x0 = (int16_t)(t->left + (int16_t)cc * t->cell_w);
      y0 = (int16_t)(t->top  + (int16_t)rr * t->cell_h);
      w  = (int16_t)((int16_t)cs * t->cell_w + 1);     /* +1：相邻格共用同一条边 */
      h  = (int16_t)((int16_t)rs * t->cell_h + 1);

      ui_table_rect_at(&r, x0, y0, w, h, t->line_color, t->line_width, t->box.alpha);
      ui_draw_rect(&r);

      if (t->text == NULL)
      {
        continue;
      }

      base = (uint16_t)(((uint16_t)rr * t->cols + cc) * t->sub_max);
      n = 0u;
      for (i = 0u; i < t->sub_max; i++)
      {
        if (t->text[base + i] != NULL && t->text[base + i][0] != '\0')
        {
          n++;
        }
      }
      if (n == 0u)
      {
        continue;
      }

      /* 和建表时的口径一致：先扣上下留白，再按段数等分、扣掉段间行距 */
      band_h = (int16_t)((h - t->pad_y * 2 - (int16_t)(n - 1u) * t->line_gap) / (int16_t)n);

      band = 0u;
      for (i = 0u; i < t->sub_max; i++)
      {
        const char *s = t->text[base + i];
        uint16_t    len;

        if (s == NULL || s[0] == '\0')
        {
          continue;
        }

        len = (uint16_t)strlen(s);
        ty  = (int16_t)(y0 + t->pad_y + (int16_t)band * (band_h + t->line_gap));
        ui_table_text_at(&x, x0, ty, w, band_h, s, len, t);
        ui_draw_text(&x);
        band++;
      }
    }
  }
}

void ui_table_init(ui_object_t *obj, ui_table_para_t *para)
{
  if (obj == NULL || para == NULL)
  {
    return;
  }

  if (para->rows == 0u) { para->rows = 1u; }
  if (para->cols == 0u) { para->cols = 1u; }
  if (para->sub_max == 0u) { para->sub_max = 1u; }
  if (para->merges == NULL) { para->merge_num = 0u; }

  /* 组件外接框 = 整表；框架按矩形口径读它(命中 / 显隐标脏 / 命中调试) */
  para->box.head.type     = UI_TYPE_MODULE;
  para->box.head.draw     = ui_table_draw;
  para->box.w             = (int16_t)((int32_t)para->cols * para->cell_w + 1);
  para->box.h             = (int16_t)((int32_t)para->rows * para->cell_h + 1);
  para->box.base_w        = para->box.w;
  para->box.base_h        = para->box.h;
  para->box.head.center_x = (int16_t)(para->left + (para->box.w >> 1));
  para->box.head.center_y = (int16_t)(para->top  + (para->box.h >> 1));
  para->box.xf.scale_num  = 1u;
  para->box.xf.scale_den  = 1u;

  obj->para = para;
  obj->cb   = NULL;                             /* 纯展示：命中测试当透明穿透 */
}

/* 把一段文字挂到格子上(只换指针)：合并块内部的行列都归到左上角那格，不标脏 */
static uint8_t ui_table_put(ui_table_para_t *t, uint8_t row, uint8_t col,
                            uint8_t sub, const char *txt)
{
  uint8_t hr, hc, rs, cs;
  uint16_t idx;

  if (t->text == NULL || row >= t->rows || col >= t->cols || sub >= t->sub_max)
  {
    return 0u;
  }

  ui_table_owner_of(t, row, col, &hr, &hc, &rs, &cs);
  idx = (uint16_t)(((uint16_t)hr * t->cols + hc) * t->sub_max + sub);

  t->text[idx] = txt;                           /* 只换指针，不拷贝 */
  return 1u;
}

uint8_t ui_table_set_text(ui_object_t *obj, uint8_t row, uint8_t col,
                          uint8_t sub, const char *txt)
{
  ui_table_para_t *t;
  uint8_t hr, hc, rs, cs;

  if (obj == NULL || obj->para == NULL || txt == NULL)
  {
    return 0u;
  }
  t = (ui_table_para_t *)obj->para;

  if (ui_table_put(t, row, col, sub, txt) == 0u)
  {
    return 0u;
  }

  ui_table_owner_of(t, row, col, &hr, &hc, &rs, &cs);
  ui_table_cell_dirty(t, hr, hc, rs, cs);
  return 1u;
}

uint8_t ui_table_set_texts(ui_object_t *obj, const ui_table_text_t *list, uint16_t n)
{
  ui_table_para_t *t;
  uint16_t i;
  uint8_t  ok = 1u;

  if (obj == NULL || obj->para == NULL || list == NULL || n == 0u)
  {
    return 0u;
  }
  t = (ui_table_para_t *)obj->para;

  for (i = 0u; i < n; i++)
  {
    if (list[i].txt == NULL || ui_table_put(t, list[i].row, list[i].col,
                                            list[i].sub, list[i].txt) == 0u)
    {
      ok = 0u;                                  /* 越界那条跳过，其余照写 */
    }
  }

  ui_table_cell_dirty(t, 0u, 0u, t->rows, t->cols);   /* 整表标一次脏就够 */
  return ok;
}

uint8_t ui_table_set_text_copy(ui_object_t *obj, uint8_t row, uint8_t col,
                               uint8_t sub, const char *txt, uint16_t len)
{
  ui_table_para_t *t;
  uint8_t  hr, hc, rs, cs;
  uint16_t idx, i;
  char    *dst;

  if (obj == NULL || obj->para == NULL || txt == NULL)
  {
    return 0u;
  }
  t = (ui_table_para_t *)obj->para;

  if (t->text == NULL || t->own_buf == NULL || t->own_size == 0u ||
      row >= t->rows || col >= t->cols || sub >= t->sub_max)
  {
    return 0u;
  }

  ui_table_owner_of(t, row, col, &hr, &hc, &rs, &cs);
  idx = (uint16_t)(((uint16_t)hr * t->cols + hc) * t->sub_max + sub);

  if (len > (uint16_t)(t->own_size - 1u))
  {
    len = (uint16_t)(t->own_size - 1u);         /* 留一个字节给结尾 '\0' */
  }

  dst = t->own_buf + (uint32_t)idx * t->own_size;
  for (i = 0u; i < len; i++)
  {
    dst[i] = txt[i];
  }
  dst[len] = '\0';

  t->text[idx] = dst;
  ui_table_cell_dirty(t, hr, hc, rs, cs);
  return 1u;
}

/* ==================================================================
 *  滚动视图（滑窗）
 *    窗口固定、内容整体平移：绘制时窗口压成裁剪区，窗口外的内容不画；
 *    拖动只改"内容偏移"，越界跟一半(橡皮筋)，松手回弹到合法区间。
 *    子对象只建不挂，绘制 / 拖动都由组件带 —— 和滑块的槽 / 滑块同一个约定
 * ================================================================== */

/* 窗口矩形（含端点），和 ui_draw_rect 同一套边界约定 */
static void ui_scroll_window(const ui_scroll_para_t *s,
                             int16_t *x0, int16_t *y0, int16_t *x1, int16_t *y1)
{
  ui_box_of(s->box.head.center_x, s->box.head.center_y, s->box.w, s->box.h,
            x0, y0, x1, y1);
}

/* "中心 + 宽高"的框和窗口是否相交（含端点）：可见裁剪就靠它判断这一项要不要动 / 要不要画 */
static uint8_t ui_scroll_box_hits(int16_t cx, int16_t cy, int16_t w, int16_t h,
                                  int16_t x0, int16_t y0, int16_t x1, int16_t y1)
{
  int16_t bx0, by0, bx1, by1;

  ui_box_of(cx, cy, w, h, &bx0, &by0, &bx1, &by1);
  return (uint8_t)(bx0 <= x1 && bx1 >= x0 && by0 <= y1 && by1 >= y0);
}

/* 硬夹到 [0, max]：程序设定 / 松手回弹目标都用它 */
static int16_t ui_scroll_clamp(int16_t v, int16_t max)
{
  if (v < 0)   { return 0; }
  if (v > max) { return max; }
  return v;
}

/* 拖动越界的"橡皮筋"：超出范围的部分只跟一半，松手再回弹 */
static int16_t ui_scroll_soft(int16_t v, int16_t max)
{
  if (v < 0)   { return (int16_t)(v / 2); }
  if (v > max) { return (int16_t)(max + (v - max) / 2); }
  return v;
}

/* 回弹 / 惯性都用二次缓出：p = 0..1000 的时长进度 -> q = 0..1000 的位移比例，
 * q = 1-(1-p)^2（起步最快、越接近终点越慢）。按时间算，不依赖帧率 */
static int32_t ui_scroll_ease_out(uint32_t p)
{
  int32_t ip = (int32_t)(1000u - p);

  return (int32_t)(1000u - (uint32_t)(ip * ip / 1000));
}

static int16_t ui_scroll_abs16(int16_t v)
{
  return (int16_t)((v < 0) ? -v : v);
}

static int32_t ui_scroll_abs32(int32_t v)
{
  return (v < 0) ? -v : v;
}

/* 32 位目标夹到 [0, max]：惯性目标可能超出 int16 的中间量，夹完再落回偏移 */
static int16_t ui_scroll_clamp32(int32_t v, int16_t max)
{
  if (v < 0)        { return 0; }
  if (v > (int32_t)max) { return max; }
  return (int16_t)v;
}

/* 用事件时间戳估瞬时速度(像素/秒)，再和旧值低通一下：
 * 单次抖动不至于把整段甩出去，快甩时又能跟上 */
static void ui_scroll_sample_vel(ui_scroll_para_t *s, int16_t d_off_x, int16_t d_off_y,
                                 uint32_t now_ms)
{
  uint32_t dt = now_ms - s->last_ms;
  int32_t  vx, vy;

  if (dt == 0u)   { dt = 1u; }
  if (dt > 100u)  { dt = 100u; }               /* 停了很久再动：限一下，别让速度估飞 */

  vx = (int32_t)d_off_x * 1000 / (int32_t)dt;
  vy = (int32_t)d_off_y * 1000 / (int32_t)dt;

  s->vel_x = (s->vel_x * 2 + vx) / 3;
  s->vel_y = (s->vel_y * 2 + vy) / 3;
  s->last_ms = now_ms;
}

/* 速度夹到上限(px/s)：防止一次快甩冲出去太远 */
static void ui_scroll_vel_clamp(ui_scroll_para_t *s)
{
  if (s->vel_x >  (int32_t)UI_SCROLL_VEL_MAX) { s->vel_x =  (int32_t)UI_SCROLL_VEL_MAX; }
  if (s->vel_x < -(int32_t)UI_SCROLL_VEL_MAX) { s->vel_x = -(int32_t)UI_SCROLL_VEL_MAX; }
  if (s->vel_y >  (int32_t)UI_SCROLL_VEL_MAX) { s->vel_y =  (int32_t)UI_SCROLL_VEL_MAX; }
  if (s->vel_y < -(int32_t)UI_SCROLL_VEL_MAX) { s->vel_y = -(int32_t)UI_SCROLL_VEL_MAX; }
}

/* 按"基准 - 偏移"把各项摆到应该在的位置：老框或新框碰到窗口的项才真的动
 * （屏外的项留着，等它要进来时一次补上 —— 差值算的是绝对目标，不会累积误差）。
 * 没给基准数组时退回"整组按 d_off 平移"；窗口变化时 d_off 传 0，整组本来就在位，不用动 */
static void ui_scroll_sync(ui_scroll_para_t *s, int16_t off_x, int16_t off_y,
                           int16_t d_off_x, int16_t d_off_y)
{
  int16_t x0, y0, x1, y1;
  uint16_t i;

  ui_scroll_window(s, &x0, &y0, &x1, &y1);

  for (i = 0u; i < s->item_num; i++)
  {
    ui_object_t *it = &s->items[i];
    int16_t cx, cy, w, h, tx, ty;

    if (it->para == NULL)
    {
      continue;
    }

    /* 没给基准数组 / 量不出框（光标之类）：退回"整组移动" */
    if (s->item_base_x == NULL || s->item_base_y == NULL ||
        ui_obj_frame(it, &cx, &cy, &w, &h) == 0u)
    {
      if (d_off_x != 0 || d_off_y != 0)
      {
        ui_anim_move_obj(it, (int16_t)(-d_off_x), (int16_t)(-d_off_y));
      }
      continue;
    }

    tx = (int16_t)(s->item_base_x[i] - off_x);   /* 目标中心 = 基准 - 偏移 */
    ty = (int16_t)(s->item_base_y[i] - off_y);

    /* 老框在窗口里（要把它挪走）或新框会进窗口（要把它挪进来）才动 */
    if (ui_scroll_box_hits(cx, cy, w, h, x0, y0, x1, y1) ||
        ui_scroll_box_hits(tx, ty, w, h, x0, y0, x1, y1))
    {
      ui_anim_move_obj(it, (int16_t)(tx - cx), (int16_t)(ty - cy));
    }
  }
}

/* 偏移增加 d_off（内容往反方向挪 d_off），脏区限制在窗口内：
 * 每个子对象的"老框 ∪ 新框"已经覆盖了变化的像素，夹到窗口后别处一个像素都不多推 */
static void ui_scroll_shift(ui_scroll_para_t *s, int16_t d_off_x, int16_t d_off_y)
{
  int16_t x0, y0, x1, y1;

  if (d_off_x == 0 && d_off_y == 0)
  {
    return;
  }

  ui_scroll_window(s, &x0, &y0, &x1, &y1);

  ui_dirty_clip_push(x0, y0, x1, y1);
  ui_scroll_sync(s, (int16_t)(s->off_x + d_off_x), (int16_t)(s->off_y + d_off_y),
                 d_off_x, d_off_y);
  ui_dirty_clip_pop();

  s->off_x = (int16_t)(s->off_x + d_off_x);
  s->off_y = (int16_t)(s->off_y + d_off_y);
}

/* 组件绘制：窗口压成裁剪区，再带裁剪把子对象逐个派发（和 ui_draw_viewport 同一套） */
static void ui_scroll_draw(void *p)
{
  ui_scroll_para_t *s = (ui_scroll_para_t *)p;
  int16_t x0, y0, x1, y1;
  uint16_t i;

  if (s == NULL || s->box.head.type != UI_TYPE_MODULE)
  {
    return;
  }

  ui_scroll_window(s, &x0, &y0, &x1, &y1);
  ui_row_clip_push_round(x0, y0, x1, y1, s->radius);   /* 圆角窗口：按行内缩，四角不漏内容 */

  for (i = 0u; i < s->item_num; i++)
  {
    ui_object_t *it = &s->items[i];
    int16_t cx, cy, w, h;

    if (it->para == NULL || it->activation != activated)
    {
      continue;                                /* 未激活的零件不画(约定同按钮/滑块) */
    }

    /* 可见裁剪：完全在窗口外的项不派发；量不出框的（光标之类）照常派发 */
    if (ui_obj_frame(it, &cx, &cy, &w, &h) != 0u &&
        ui_scroll_box_hits(cx, cy, w, h, x0, y0, x1, y1) == 0u)
    {
      continue;
    }

    ui_draw_dispatch(it->para);
  }

  ui_row_clip_pop();
}

void ui_scroll_set_offset(ui_object_t *obj, int16_t off_x, int16_t off_y)
{
  ui_scroll_para_t *s;

  if (obj == NULL || obj->para == NULL)
  {
    return;
  }
  s = (ui_scroll_para_t *)obj->para;

  s->settling = 0u;
  s->vel_x = 0;                                /* 程序设定位置：惯性 / 回弹都停 */
  s->vel_y = 0;
  off_x = ui_scroll_clamp(off_x, s->max_x);
  off_y = ui_scroll_clamp(off_y, s->max_y);
  ui_scroll_shift(s, (int16_t)(off_x - s->off_x), (int16_t)(off_y - s->off_y));
}

void ui_scroll_get_offset(const ui_object_t *obj, int16_t *off_x, int16_t *off_y)
{
  const ui_scroll_para_t *s;

  if (obj == NULL || obj->para == NULL)
  {
    if (off_x != NULL) { *off_x = 0; }
    if (off_y != NULL) { *off_y = 0; }
    return;
  }
  s = (const ui_scroll_para_t *)obj->para;
  if (off_x != NULL) { *off_x = s->off_x; }
  if (off_y != NULL) { *off_y = s->off_y; }
}

void ui_scroll_set_window(ui_object_t *obj, int16_t cx, int16_t cy, int16_t w, int16_t h)
{
  ui_scroll_para_t *s;
  int16_t ox0, oy0, ox1, oy1;
  int16_t nx0, ny0, nx1, ny1;
  int16_t wx, wy;

  if (obj == NULL || obj->para == NULL)
  {
    return;
  }
  s = (ui_scroll_para_t *)obj->para;

  if (w <= 0) { w = 1; }                       /* 收起到 0 时留 1px，避免除零 / 空裁剪 */
  if (h <= 0) { h = 1; }

  /* 老窗口先标脏：收起时露出来的那块要重画成背景 */
  ui_scroll_window(s, &ox0, &oy0, &ox1, &oy1);
  ui_dirty_mark(ox0, oy0, ox1, oy1);

  s->box.head.center_x = cx;
  s->box.head.center_y = cy;
  s->box.w = s->box.base_w = w;
  s->box.h = s->box.base_h = h;

  /* 新窗口也标脏：展开时新露出来的区域要画内容；顺手按新尺寸重算滚动上限 */
  ui_scroll_window(s, &nx0, &ny0, &nx1, &ny1);
  ui_dirty_mark(nx0, ny0, nx1, ny1);

  s->max_x = (s->horizontal != 0u && s->content_w > w)
           ? (int16_t)(s->content_w - w) : 0;
  s->max_y = (s->vertical != 0u && s->content_h > h)
           ? (int16_t)(s->content_h - h) : 0;

  /* 窗口尺寸变了，滑行 / 回弹的目标已经没意义：就地停掉 */
  s->settling = 0u;
  s->vel_x = 0;
  s->vel_y = 0;

  wx = ui_scroll_clamp(s->off_x, s->max_x);
  wy = ui_scroll_clamp(s->off_y, s->max_y);

  if (wx != s->off_x || wy != s->off_y)
  {
    ui_scroll_shift(s, (int16_t)(wx - s->off_x), (int16_t)(wy - s->off_y));
  }
  else
  {
    /* 偏移没变也要同步一次：窗口变大后，之前被裁掉的项可能要以正确位置出现 */
    ui_dirty_clip_push(nx0, ny0, nx1, ny1);
    ui_scroll_sync(s, s->off_x, s->off_y, 0, 0);
    ui_dirty_clip_pop();
  }
}

void ui_scroll_task(ui_object_t *obj)
{
  ui_scroll_para_t *s;
  uint32_t now, elapsed, prog;
  int32_t  q, nx, ny;

  if (obj == NULL || obj->para == NULL)
  {
    return;
  }
  s = (ui_scroll_para_t *)obj->para;
  if (s->settling == 0u || s->settle_time == 0u)
  {
    return;                                    /* 没在滑行 / 回弹：空转 */
  }

  /* 按时间推进：过去了多少 ms -> 进度 0..1000 -> 二次缓出比例。
   * 时间用 ui_anim_now_ms（和事件时间戳同一块 TIM3），帧率怎么抖，轨迹都一样 */
  now     = ui_anim_now_ms();
  elapsed = now - s->settle_ms;                /* 无符号减法：时钟回绕也对 */
  prog    = (elapsed >= s->settle_time) ? 1000u
                                        : (uint32_t)(elapsed * 1000u / s->settle_time);
  q       = ui_scroll_ease_out(prog);

  nx = (int32_t)s->settle_from_x
     + ((int32_t)s->settle_x - (int32_t)s->settle_from_x) * q / 1000;
  ny = (int32_t)s->settle_from_y
     + ((int32_t)s->settle_y - (int32_t)s->settle_from_y) * q / 1000;

  ui_scroll_shift(s, (int16_t)(nx - s->off_x), (int16_t)(ny - s->off_y));

  if (s->on_scroll != NULL)
  {
    s->on_scroll(obj, s->off_x, s->off_y);
  }

  if (prog >= 1000u)
  {
    s->settling = 0u;                          /* 到位：停 */
    s->vel_x = 0;
    s->vel_y = 0;
  }
}

/* 找窗口里点中的最上层"可点"子对象：从数组末尾往前找（后画的盖在上面），
 * 没挂回调 / 未激活的当透明穿透继续往下找 —— 和框架命中测试同一个口径 */
static ui_object_t *ui_scroll_item_at(ui_scroll_para_t *s, int16_t x, int16_t y)
{
  int16_t cx, cy, w, h;
  int16_t x0, y0, x1, y1;
  uint16_t i;

  for (i = s->item_num; i > 0u; i--)
  {
    ui_object_t *it = &s->items[i - 1u];

    if (it->para == NULL || it->activation != activated || it->cb == NULL)
    {
      continue;
    }
    if (ui_obj_frame(it, &cx, &cy, &w, &h) == 0u)
    {
      continue;                                /* 量不出框（光标之类）：跳过 */
    }

    ui_box_of(cx, cy, w, h, &x0, &y0, &x1, &y1);
    if (x >= x0 && x <= x1 && y >= y0 && y <= y1)
    {
      return it;
    }
  }
  return NULL;
}

/* 轻点转派：组件把 DOWN 吞了，所以这里补成 DOWN -> UP -> CLICK 三步，
 * 子对象拿到的语义和"直接点它"一致（按钮的按下反馈 / CLICK 通知都正常） */
static void ui_scroll_tap(ui_scroll_para_t *s, const ui_event_t *ev)
{
  ui_object_t *it = ui_scroll_item_at(s, ev->x, ev->y);
  ui_event_t   e;

  if (it == NULL)
  {
    return;
  }

  e = *ev;
  e.dx = 0;
  e.dy = 0;
  e.type = UI_EV_DOWN;   (void)it->cb(it, &e);
  e.type = UI_EV_UP;     (void)it->cb(it, &e);
  e.type = UI_EV_CLICK;  (void)it->cb(it, &e);
}

/* 组件事件：整块窗口一个拖动手势（轻点再转派给子对象） */
static uint8_t ui_scroll_cb(ui_object_t *obj, const ui_event_t *ev)
{
  ui_scroll_para_t *s = (ui_scroll_para_t *)obj->para;
  int16_t want_x, want_y, d_x, d_y;

  if (s == NULL || ev == NULL)
  {
    return 0;
  }

  switch (ev->type)
  {
    case UI_EV_DOWN:
      s->grabbing = 1u;
      s->settling = 0u;                        /* 手按上来：没滑完 / 没弹完的作废，跟手要紧 */
      s->moved    = 0u;
      s->vel_x    = 0;
      s->vel_y    = 0;
      s->grab_x   = ev->x;
      s->grab_y   = ev->y;
      s->base_x   = s->off_x;
      s->base_y   = s->off_y;
      s->last_ms  = ev->time_ms;
      return 1u;

    case UI_EV_MOVE:
      if (s->grabbing != 0u)
      {
        /* 手指往左 / 上拖 = 偏移变大：偏移 = 按下时偏移 + 手指相对按下点的位移 */
        want_x = (s->horizontal != 0u) ? (int16_t)(s->base_x + (s->grab_x - ev->x)) : s->off_x;
        want_y = (s->vertical   != 0u) ? (int16_t)(s->base_y + (s->grab_y - ev->y)) : s->off_y;

        want_x = ui_scroll_soft(want_x, s->max_x);
        want_y = ui_scroll_soft(want_y, s->max_y);

        d_x = (int16_t)(want_x - s->off_x);
        d_y = (int16_t)(want_y - s->off_y);
        ui_scroll_shift(s, d_x, d_y);
        ui_scroll_sample_vel(s, d_x, d_y, ev->time_ms);

        /* 位移超过阈值就算拖动：松手不再派发点击 */
        if (ui_scroll_abs16((int16_t)(ev->x - s->grab_x)) > UI_SCROLL_TAP_SLOP ||
            ui_scroll_abs16((int16_t)(ev->y - s->grab_y)) > UI_SCROLL_TAP_SLOP)
        {
          s->moved = 1u;
        }

        if (s->on_scroll != NULL)
        {
          s->on_scroll(obj, s->off_x, s->off_y);
        }
      }
      return 1u;

    case UI_EV_UP:
    case UI_EV_CANCEL:
      if (s->grabbing != 0u)
      {
        s->grabbing = 0u;

        if (s->moved == 0u && ev->type == UI_EV_UP)
        {
          /* 轻点：把阈值以内的那点微小位移先退回去（内容不该为一次点击挪位），再转派点击 */
          if (s->off_x != s->base_x || s->off_y != s->base_y)
          {
            ui_scroll_shift(s, (int16_t)(s->base_x - s->off_x),
                               (int16_t)(s->base_y - s->off_y));
          }

          s->vel_x = 0;
          s->vel_y = 0;
          ui_scroll_tap(s, ev);
          return 1u;
        }

        /* ---- 松手：算好这一段滑行 / 回弹的起点、目标与时长（全部按时间基准推进） ---- */
        s->settle_from_x = s->off_x;
        s->settle_from_y = s->off_y;
        s->settle_ms     = ui_anim_now_ms();

        {
          int32_t tx = ui_scroll_clamp32(s->off_x, s->max_x);
          int32_t ty = ui_scroll_clamp32(s->off_y, s->max_y);

          if (tx != s->off_x || ty != s->off_y)
          {
            /* 越界（橡皮筋拉出去的那段）：回弹到边界，不再惯性 */
            s->settle_x    = (int16_t)tx;
            s->settle_y    = (int16_t)ty;
            s->settle_time = UI_SCROLL_RETURN_MS;
            s->vel_x = 0;
            s->vel_y = 0;

            if (s->ticked != 0u)
            {
              s->settling = 1u;
            }
            else
            {
              ui_scroll_set_offset(obj, s->settle_x, s->settle_y);   /* 没任务推进：直接夹回 */
            }
          }
          else if (s->ticked != 0u &&
                   (ui_scroll_abs32(s->vel_x) >= (int32_t)UI_SCROLL_VEL_START ||
                    ui_scroll_abs32(s->vel_y) >= (int32_t)UI_SCROLL_VEL_START))
          {
            /* 没越界且有速度：目标 = 偏移 + 速度 × COAST_TAU，COAST_MS 内二次缓出 */
            ui_scroll_vel_clamp(s);

            tx = ui_scroll_clamp32((int32_t)s->off_x +
                                   s->vel_x * (int32_t)UI_SCROLL_COAST_TAU_MS / 1000,
                                   s->max_x);
            ty = ui_scroll_clamp32((int32_t)s->off_y +
                                   s->vel_y * (int32_t)UI_SCROLL_COAST_TAU_MS / 1000,
                                   s->max_y);
            s->settle_x    = (int16_t)tx;
            s->settle_y    = (int16_t)ty;
            s->settle_time = UI_SCROLL_COAST_MS;
            s->settling    = (uint8_t)((tx != s->off_x || ty != s->off_y) ? 1u : 0u);

            if (s->settling == 0u)
            {
              s->vel_x = 0;
              s->vel_y = 0;
            }
          }
          else
          {
            s->settling = 0u;                  /* 轻推 / 没任务表：就地停 */
            s->vel_x = 0;
            s->vel_y = 0;
          }
        }
      }
      return 1u;

    default:                                   /* CLICK / LONG_PRESS 已被 DOWN/UP 覆盖 */
      return 1u;
  }
}

void ui_scroll_init(ui_object_t *obj, ui_scroll_para_t *para,
                    ui_object_t *items, uint16_t item_num,
                    int16_t cx, int16_t cy, int16_t w, int16_t h,
                    int16_t content_w, int16_t content_h,
                    uint8_t horizontal, uint8_t vertical)
{
  uint16_t i;

  if (obj == NULL || para == NULL || w <= 0 || h <= 0)
  {
    return;
  }

  /* ---- 内容与滚动范围：内容比窗口小 -> 该方向不滚 ---- */
  para->items      = items;
  para->item_num   = (items != NULL) ? item_num : 0u;
  para->content_w  = (content_w > 0) ? content_w : 0;
  para->content_h  = (content_h > 0) ? content_h : 0;
  para->horizontal = (horizontal != 0u) ? 1u : 0u;
  para->vertical   = (vertical   != 0u) ? 1u : 0u;
  para->max_x      = (para->horizontal != 0u && para->content_w > w)
                   ? (int16_t)(para->content_w - w) : 0;
  para->max_y      = (para->vertical != 0u && para->content_h > h)
                   ? (int16_t)(para->content_h - h) : 0;

  /* 窗口圆角：不能超过半宽 / 半高（和 ui_tool 的圆角矩形同一口径） */
  {
    int16_t rmax = (int16_t)((w < h ? w : h) >> 1);

    if ((int32_t)para->radius > (int32_t)rmax)   /* rmax > 255 时不会把 8 位截断 */
    {
      para->radius = (uint8_t)rmax;
    }
  }

  /* ---- 运行态清零 ---- */
  para->off_x = 0;      para->off_y = 0;
  para->grab_x = 0;     para->grab_y = 0;
  para->base_x = 0;     para->base_y = 0;
  para->settle_x = 0;   para->settle_y = 0;
  para->settle_from_x = 0;
  para->settle_from_y = 0;
  para->settle_ms = 0u;
  para->settle_time = 0u;
  para->vel_x = 0;      para->vel_y = 0;
  para->last_ms = 0u;
  para->grabbing = 0u;  para->settling = 0u;
  para->moved = 0u;
  para->on_scroll = NULL;                      /* 调用方装配后按需挂 */

  /* ---- 子对象平移靠 xf.anchor：先给每个对象把锚点设成它当前中心 ----
   * 不设的话 anchor / anchor_off 还是 0，ui_anim_move_obj 一挪就会把中心算成"偏移"、
   * 整行跳到左上角。只对"有变换块"的类型有效（矩形 / 文字 / 图片 / 圆 / 点 / 折线 / 合并 / 视窗 / 进度条）；
   * 组件(UI_TYPE_MODULE)不在平移表里，不能当滚动内容 */
  for (i = 0u; i < para->item_num; i++)
  {
    ui_head_t *h = (ui_head_t *)para->items[i].para;

    if (h != NULL)
    {
      ui_obj_set_anchor(&para->items[i], h->center_x, h->center_y);
    }
  }

  /* ---- 可见裁剪的基准位置：调用方给了数组就填上各项当前中心 ----
   * 之后 shift 按"基准 - 偏移"反算目标，屏外的行可以跳过；
   * 有任何一项量不出框就把基准数组作废，退回整组移动 */
  if (para->item_base_x != NULL && para->item_base_y != NULL)
  {
    for (i = 0u; i < para->item_num; i++)
    {
      int16_t bx, by, bw, bh;

      if (ui_obj_frame(&para->items[i], &bx, &by, &bw, &bh) == 0u)
      {
        para->item_base_x = NULL;
        para->item_base_y = NULL;
        break;
      }
      para->item_base_x[i] = bx;
      para->item_base_y[i] = by;
    }
  }

  /* ---- 组件外接框 = 显示窗口(框架按矩形口径读它：命中 / 显隐标脏) ---- */
  para->box.head.type     = UI_TYPE_MODULE;
  para->box.head.draw     = ui_scroll_draw;
  para->box.head.center_x = cx;
  para->box.head.center_y = cy;
  para->box.w = para->box.base_w = w;
  para->box.h = para->box.base_h = h;
  para->box.xf.scale_num  = 1u;                /* para 静态清零后 scale_den 是 0，先扶正 */
  para->box.xf.scale_den  = 1u;

  /* ---- 接上组件对象：cb 非空 = 可被命中(整块窗口一个拖动手势) ---- */
  obj->para = para;
  obj->cb   = ui_scroll_cb;

  /* 回弹要每帧推一步：直接登记，调用方不用再管；表满就退化成松手直接夹回 */
  para->ticked = ui_module_task_register(obj, ui_scroll_task);
}

#if UI_DEBUG_HUD
/* ==================================================================
 *  调试 HUD：帧率 + 最近触点坐标
 *    挂到 ui_render_set_overlay 上：每片场景画完、交给硬件前画一次 ——
 *      · 永远压在所有页面之上；
 *      · 不是 UI 对象，命中测试完全看不见它：点击 / 拖动全部穿透；
 *      · 只在内容变化(帧率每秒更新、坐标变动)时给自己标脏，静态时几乎不增加推屏。
 *    首次调 ui_module_task_all 时自动挂上，应用侧不用写任何代码
 * ================================================================== */
#define UI_HUD_BG_COLOR   0x0000      /* 黑底：配 alpha 做半透明 */
#define UI_HUD_TXT_COLOR  0xFFFF      /* 白字 */
#define UI_HUD_FONT       16u         /* ASCII 走内部小字库(12/16/24/32)，不读片外字库 */

static ui_rect_para_t s_hud_bg;       /* HUD 底(半透明实心矩形) */
static ui_text_para_t s_hud_txt;      /* HUD 文字(两行：FPS / 坐标) */
static char           s_hud_buf[48];

static uint32_t s_hud_frames;         /* 本统计周期内跑了多少帧 */
static uint32_t s_hud_fps_ms;         /* 上次统计时刻(ui_anim_now_ms) */
static uint16_t s_hud_fps;            /* 显示用帧率 */
static uint8_t  s_hud_primed;         /* 1 = 已取过统计起点(跳过开机铺屏那段) */
static uint8_t  s_hud_ready;          /* 1 = 已经挂到浮层钩子上 */
static int16_t  s_hud_x = -1;         /* 最近触点(松手后保留最后位置) */
static int16_t  s_hud_y = -1;
static uint8_t  s_hud_down;
static int16_t  s_hud_x0;             /* HUD 矩形(标脏用) */
static int16_t  s_hud_y0;
static int16_t  s_hud_x1;
static int16_t  s_hud_y1;

/* 按当前显示内容重新排版文字：两行、只含 ASCII */
static void ui_hud_refresh_text(void)
{
  int n = snprintf(s_hud_buf, sizeof(s_hud_buf), "FPS:%u\nX:%d Y:%d %c",
                   (unsigned int)s_hud_fps,
                   (int)s_hud_x, (int)s_hud_y,
                   ((s_hud_down != 0u) ? 'D' : 'U'));

  if (n < 0)
  {
    n = 0;
  }
  if ((uint32_t)n >= sizeof(s_hud_buf))
  {
    n = (int)sizeof(s_hud_buf) - 1;
  }
  s_hud_txt.text_len = (uint16_t)n;
}

/* 浮层绘制：底(半透明) + 文字(半透明)，只画当前这一片脏条内的部分 */
static void ui_hud_draw(void)
{
  ui_draw_rect(&s_hud_bg);
  ui_draw_text(&s_hud_txt);
}

/* 首次使用：按实际屏幕尺寸摆到右上角、建好 para，再挂到浮层钩子上 */
static void ui_hud_setup(void)
{
  int16_t sw = ui_dirty_screen_w();

  s_hud_x1 = (int16_t)(sw - 1 - UI_DEBUG_HUD_MARGIN);
  s_hud_y0 = (int16_t)UI_DEBUG_HUD_MARGIN;
  s_hud_x0 = (int16_t)(s_hud_x1 - UI_DEBUG_HUD_W + 1);
  s_hud_y1 = (int16_t)(s_hud_y0 + UI_DEBUG_HUD_H - 1);

  s_hud_bg.head.type     = UI_TYPE_RECT;
  s_hud_bg.head.draw     = ui_draw_rect;
  s_hud_bg.head.center_x = (int16_t)(s_hud_x0 + (UI_DEBUG_HUD_W >> 1));
  s_hud_bg.head.center_y = (int16_t)(s_hud_y0 + (UI_DEBUG_HUD_H >> 1));
  s_hud_bg.w = s_hud_bg.base_w = UI_DEBUG_HUD_W;
  s_hud_bg.h = s_hud_bg.base_h = UI_DEBUG_HUD_H;
  s_hud_bg.border_width  = 0;
  s_hud_bg.color         = UI_HUD_BG_COLOR;
  s_hud_bg.fill          = 1u;
  s_hud_bg.corners       = 0u;
  s_hud_bg.radius        = 0u;
  s_hud_bg.alpha         = UI_DEBUG_HUD_BG_A;
  s_hud_bg.xf.scale_num  = 1u;
  s_hud_bg.xf.scale_den  = 1u;

  s_hud_txt.head.type     = UI_TYPE_TEXT;
  s_hud_txt.head.draw     = ui_draw_text;
  s_hud_txt.head.center_x = s_hud_bg.head.center_x;
  s_hud_txt.head.center_y = s_hud_bg.head.center_y;
  s_hud_txt.w = s_hud_txt.base_w = (int16_t)(UI_DEBUG_HUD_W - 12);
  s_hud_txt.h = s_hud_txt.base_h = (int16_t)(UI_DEBUG_HUD_H - 6);
  s_hud_txt.text          = (const uint8_t *)s_hud_buf;
  s_hud_txt.text_len      = 0u;
  s_hud_txt.font_size     = (uint16_t)UI_HUD_FONT;
  s_hud_txt.color         = UI_HUD_TXT_COLOR;
  s_hud_txt.align         = UI_TEXT_ALIGN_LEFT;
  s_hud_txt.valign        = UI_TEXT_VALIGN_CENTER;
  s_hud_txt.line_spacing  = 0u;
  s_hud_txt.hz_font_id    = 0u;
  s_hud_txt.hz_glyph_fn   = NULL;       /* ASCII 用内部字库，不读片外字库 */
  s_hud_txt.alpha         = UI_DEBUG_HUD_TXT_A;
  s_hud_txt.xf.scale_num  = 1u;
  s_hud_txt.xf.scale_den  = 1u;

  ui_hud_refresh_text();

  ui_render_set_overlay(ui_hud_draw);
  ui_dirty_mark(s_hud_x0, s_hud_y0, s_hud_x1, s_hud_y1);   /* 注册前那几帧没画：补一次 */
}

/* 每帧(ui_module_task_all 开头)调：统计帧率 / 取最近触点；内容变了才标脏 */
static void ui_hud_task(void)
{
  uint32_t now;
  int16_t  x = 0, y = 0;
  uint8_t  down;
  uint8_t  changed = 0u;

  if (s_hud_ready == 0u)
  {
    s_hud_ready = 1u;
    ui_hud_setup();
  }

  now = ui_anim_now_ms();

  if (s_hud_primed == 0u)
  {
    s_hud_primed = 1u;
    s_hud_fps_ms = now;
  }

  s_hud_frames++;

  if ((uint32_t)(now - s_hud_fps_ms) >= 1000u && now != s_hud_fps_ms)
  {
    uint16_t fps = (uint16_t)((s_hud_frames * 1000u) / (now - s_hud_fps_ms));

    s_hud_frames = 0u;
    s_hud_fps_ms = now;
    if (fps != s_hud_fps)
    {
      s_hud_fps = fps;
      changed = 1u;
    }
  }

  down = ui_event_last_touch(&x, &y, NULL);
  if (x != s_hud_x || y != s_hud_y || down != s_hud_down)
  {
    s_hud_x    = x;
    s_hud_y    = y;
    s_hud_down = down;
    changed    = 1u;
  }

  if (changed != 0u)
  {
    ui_hud_refresh_text();
    ui_dirty_mark(s_hud_x0, s_hud_y0, s_hud_x1, s_hud_y1);
  }
}
#endif /* UI_DEBUG_HUD */

/* ==================================================================
 *  每帧任务注册表
 *    一个槽只放一种任务：组件任务带 obj，页面任务无参(两种互斥)
 * ================================================================== */

typedef struct
{
  ui_object_t *obj;                          /* 组件任务的对象；页面任务 / 空槽为 NULL */
  void (*task_obj)(ui_object_t *obj);        /* 组件任务 */
  void (*task_fn)(void);                     /* 页面 / 全局任务 */
}ui_module_task_item_t;

static ui_module_task_item_t s_module_task[UI_MODULE_TASK_MAX];

uint8_t ui_module_task_register(ui_object_t *obj, void (*task)(ui_object_t *obj))
{
  uint16_t i;                                /* 16 位：上限调到 255 以上也不会回绕 */

  if (obj == NULL || task == NULL)
  {
    return 0u;
  }

  /* 已登记过：换函数就行，不重复入表 */
  for (i = 0u; i < UI_MODULE_TASK_MAX; i++)
  {
    if (s_module_task[i].obj == obj)
    {
      s_module_task[i].task_obj = task;
      s_module_task[i].task_fn  = NULL;
      return 1u;
    }
  }

  /* 找第一个空槽 */
  for (i = 0u; i < UI_MODULE_TASK_MAX; i++)
  {
    if (s_module_task[i].obj == NULL && s_module_task[i].task_fn == NULL)
    {
      s_module_task[i].obj      = obj;
      s_module_task[i].task_obj = task;
      s_module_task[i].task_fn  = NULL;
      return 1u;
    }
  }

  return 0u;                                 /* 表满：返回 0，调用方自己知道没登记上 */
}

uint8_t ui_module_task_register_fn(void (*task)(void))
{
  uint16_t i;

  if (task == NULL)
  {
    return 0u;
  }

  /* 同一个函数重复登记只入表一次(幂等) */
  for (i = 0u; i < UI_MODULE_TASK_MAX; i++)
  {
    if (s_module_task[i].task_fn == task)
    {
      return 1u;
    }
  }

  for (i = 0u; i < UI_MODULE_TASK_MAX; i++)
  {
    if (s_module_task[i].obj == NULL && s_module_task[i].task_fn == NULL)
    {
      s_module_task[i].obj      = NULL;
      s_module_task[i].task_obj = NULL;
      s_module_task[i].task_fn  = task;
      return 1u;
    }
  }

  return 0u;
}

void ui_module_task_unregister(ui_object_t *obj)
{
  uint16_t i;

  if (obj == NULL)
  {
    return;
  }

  for (i = 0u; i < UI_MODULE_TASK_MAX; i++)
  {
    if (s_module_task[i].obj == obj)
    {
      s_module_task[i].obj      = NULL;
      s_module_task[i].task_obj = NULL;
      s_module_task[i].task_fn  = NULL;
      return;
    }
  }
}

void ui_module_task_all(void)
{
  uint16_t i;

#if UI_DEBUG_HUD
  ui_hud_task();                               /* 调试 HUD：统计/刷新；首次顺手挂上浮层 */
#endif

  for (i = 0u; i < UI_MODULE_TASK_MAX; i++)
  {
    if (s_module_task[i].task_fn != NULL)
    {
      s_module_task[i].task_fn();                  /* 页面 / 全局任务：无参 */
    }
    else if (s_module_task[i].obj != NULL && s_module_task[i].task_obj != NULL)
    {
      s_module_task[i].task_obj(s_module_task[i].obj);   /* 组件任务：带上自己的对象 */
    }
  }
}
