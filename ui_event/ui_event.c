/**
 ****************************************************************************************************
 * @file        ui_event.c
 * @brief       UI 事件层
 * @note        说明见 ui_event.h
 ****************************************************************************************************
 */

#include "./ui_event/ui_event.h"

#include <stdio.h>                       /* UI_EVENT_DEBUG 打开时要用 printf */

#include "./ui_config/ui_config.h"       /* UI_EVENT_TOUCH_MAX / UI_EVENT_DEBUG */
#include "./ui_control/ui_control.h"     /* ui_hit_test */


/* seen 位图只有 16 位：触点 id 上限再大就得换成数组 */
#if UI_EVENT_TOUCH_MAX > 16u
#error "UI_EVENT_TOUCH_MAX 超过 16：ui_event_feed 里的 seen 位图放不下，要改成数组"
#endif


/* 每个触点一份跟踪状态，按 id 下标取。
 * s_down_obj / s_cur_obj 的区别就是"点击"的判据：
 *   松手时 s_cur_obj 还等于 s_down_obj -> 有效点击；否则手指早移出去了 -> 取消 */
static ui_hierarchy_t* s_level = NULL;                          /* 事件派发到哪个层级 */

static ui_object_t* s_down_obj[UI_EVENT_TOUCH_MAX];             /* 按下那一刻命中的对象 */
static ui_object_t* s_cur_obj [UI_EVENT_TOUCH_MAX];             /* 最后有坐标时命中的对象 */
static int16_t      s_last_x  [UI_EVENT_TOUCH_MAX];             /* 最后已知的坐标(给位移和 UP 用) */
static int16_t      s_last_y  [UI_EVENT_TOUCH_MAX];
static uint8_t      s_track   [UI_EVENT_TOUCH_MAX];             /* 这个 id 正按着(1) */

/* 最近触点快照：给调试 HUD 之类的"只看不改"的模块用 */
static int16_t      s_touch_x    = -1;
static int16_t      s_touch_y    = -1;
static uint8_t      s_touch_down = 0u;


void ui_event_set_level(ui_hierarchy_t* level)
{
  s_level = level;
}

uint8_t ui_event_last_touch(int16_t *x, int16_t *y, uint8_t *down)
{
  if (x != NULL)    { *x = s_touch_x; }
  if (y != NULL)    { *y = s_touch_y; }
  if (down != NULL) { *down = s_touch_down; }
  return s_touch_down;
}

/* 给一个对象发一个事件。没有对象 / 对象没挂回调就什么都不做 */
static void ui_event_send(ui_object_t* obj, ui_ev_type_t type, const ui_touch_point_t* pt,
                          int16_t dx, int16_t dy, uint32_t time_ms)
{
  ui_event_t ev;

  if (obj == NULL || obj->cb == NULL)
  {
    return;
  }

  ev.type    = type;
  ev.id      = pt->id;
  ev.x       = pt->x;
  ev.y       = pt->y;
  ev.dx      = dx;
  ev.dy      = dy;
  ev.time_ms = time_ms;

#if UI_EVENT_DEBUG
  printf("[EV] type=%u id=%u x=%d y=%d dx=%d dy=%d\r\n",
         (unsigned int)type, (unsigned int)pt->id,
         (int)ev.x, (int)ev.y, (int)ev.dx, (int)ev.dy);
#endif

  obj->cb(obj, &ev);                     /* 返回值(1 = 已处理)留给以后做事件传递 */
}

/* 一个触点抬起来了：按"松手时是不是还在按下那个对象上"决定发 CLICK 还是 CANCEL */
static void ui_event_release(uint8_t id, uint32_t time_ms)
{
  ui_touch_point_t pt;

  pt.id = id;
  pt.x  = s_last_x[id];
  pt.y  = s_last_y[id];

  if (s_cur_obj[id] == s_down_obj[id])
  {
    /* 松手时还在同一个对象上 = 有效点击：先发 UP(手指松了)，再补一个 CLICK */
    ui_event_send(s_down_obj[id], UI_EV_UP,    &pt, 0, 0, time_ms);
    ui_event_send(s_down_obj[id], UI_EV_CLICK, &pt, 0, 0, time_ms);
  }
  else
  {
    /* 移出去之后才松手 = 不算点击 */
    ui_event_send(s_down_obj[id], UI_EV_CANCEL, &pt, 0, 0, time_ms);
  }

  s_track[id]    = 0u;
  s_down_obj[id] = NULL;
  s_cur_obj[id]  = NULL;
}

void ui_event_feed(const ui_touch_point_t* pts, uint8_t count, uint32_t time_ms)
{
  uint16_t seen = 0u;                    /* 这一帧出现过的 id(位图) */
  uint8_t  i;
  uint8_t  id;

  /* 最近触点快照(调试 HUD 用)：有手指就取第一个点；全抬起只清标志，坐标留着 */
  if (pts != NULL && count != 0u)
  {
    s_touch_x    = pts[0].x;
    s_touch_y    = pts[0].y;
    s_touch_down = 1u;
  }
  else
  {
    s_touch_down = 0u;
  }

  if (s_level == NULL || pts == NULL)    /* 没注册层级 / 没有数据 */
  {
    return;
  }

  /* ---- 一、这一帧有坐标的触点：按下 或者 移动 ---- */
  for (i = 0u; i < count; i++)
  {
    ui_object_t* hit;

    id = pts[i].id;
    if (id >= (uint8_t)UI_EVENT_TOUCH_MAX)   /* id 越界：丢掉这个点，别写坏数组 */
    {
      continue;
    }

    seen |= (uint16_t)(1u << id);

    hit = ui_hit_test(s_level, pts[i].x, pts[i].y);

    if (s_track[id] == 0u)
    {
      /* 这个触点刚出现 = 按下 */
      s_track[id]    = 1u;
      s_down_obj[id] = hit;
      s_cur_obj[id]  = hit;
      s_last_x[id]   = pts[i].x;
      s_last_y[id]   = pts[i].y;

      ui_event_send(hit, UI_EV_DOWN, &pts[i], 0, 0, time_ms);
    }
    else
    {
      int16_t dx = (int16_t)(pts[i].x - s_last_x[id]);
      int16_t dy = (int16_t)(pts[i].y - s_last_y[id]);

      s_cur_obj[id] = hit;               /* 记下最后落在谁身上，抬起来判点击用 */
      s_last_x[id]  = pts[i].x;
      s_last_y[id]  = pts[i].y;

      /* MOVE 发给【按下时那个对象】：手指移出它的框也继续发，拖拽要靠它持续跟手。
       * 坐标没变就不发 —— 手指按住不动时没必要每帧叫一次回调 */
      if (dx != 0 || dy != 0)
      {
        ui_event_send(s_down_obj[id], UI_EV_MOVE, &pts[i], dx, dy, time_ms);
      }
    }
  }

  /* ---- 二、这一帧没出现、但之前在按的触点：抬起了 ----
   * count == 0 时这一轮会把所有按着的触点全部收掉 */
  for (id = 0u; id < (uint8_t)UI_EVENT_TOUCH_MAX; id++)
  {
    if (s_track[id] != 0u && (seen & (uint16_t)(1u << id)) == 0u)
    {
      ui_event_release(id, time_ms);
    }
  }
}
