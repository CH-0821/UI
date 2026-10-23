/**
 ****************************************************************************************************
 * @file        ui_module.h
 * @brief       UI 组件层：多档滑块(兼滚动条) —— 以及"以后怎么加新组件"的约定
 ****************************************************************************************************
 * @attention
 *
 *  【组件约定 —— 以后加新组件，核心文件(ui_create.h / ui_control.c...)一个字不用再改】
 *    框架在 ui_type_t 尾部一次性预留了 UI_TYPE_MODULE 这个"组件类型槽"，所有组件共用它：
 *      1. 组件 para 首成员 = ui_rect_para_t box：
 *         box.head.type 填 UI_TYPE_MODULE、box.head.draw 填组件自己的绘制回调；
 *         box 的中心/宽高 = 组件的【外接框】，框架按矩形口径量框 —— 命中测试、
 *         显隐标脏(ui_obj_set_active)、动画/复位读基准，全都拿它当矩形使。
 *         组件自己负责维护这四个数(box.head.center_x/y、box.w/h)。
 *      2. 组件的"零件对象"(槽 / 滑块 / 图标...)只建不挂：不进任何链 / 绘制表、不挂回调，
 *         由组件的绘制回调按固定先后 ui_draw_dispatch 带画(z 序天然正确)；
 *         于是命中全部落在组件对象自己的 cb 上 —— 整块控件一个交互语义(比如拖动)。
 *      3. 零件选"有 w/h 的盒子类型"(矩形 / 图片 / 文本 —— 三种 para 前缀同布局)：
 *         组件要读它们的几何来摆放 / 对齐。圆 / 进度条这类"没有 w/h 字段"的不行。
 *      4. 组件对象自己照常 UI_InitObject + UI_RegisterObject_Node 挂链 / 挂层级，
 *         显隐(对象激活 / 链激活)和普通对象同一套。
 *    绘制为什么不用登记：ui_draw_dispatch 只认 head.draw 函数指针(para 自带)，
 *    和 type 是什么值无关。量框为什么只登记一次：ui_control 的取框表里
 *    [UI_TYPE_MODULE] = ui_rect_bounds，按第 1 条的矩形前缀读框。
 *
 *  【本文件当前提供的组件】
 *    按钮 ui_button：底 + 文字(图标)，按下 / 松开 / 自锁选中三种外观，CLICK 通知。
 *    表格 ui_table：等宽网格 + 合并表，整表一个对象；格子文字可热更新。
 *    滚动视图 ui_scroll：固定窗口 + 一批只建不挂的子对象，拖动整体滚动、到边界回弹。
 *    多档滑块 ui_slider：一根槽 + 一颗滑块，档位数任意(滚动条 = 档数给大一点)。
 *      · 渲染随意：槽 / 滑块各自是独立的 ui_object_t，用矩形还是图片由使用者定；
 *      · 行程两种模型(UI_SLIDER_INSIDE / UI_SLIDER_CENTER)：
 *        INSIDE = 滑块整颗缩在槽里(小旋钮/滚动条滑块)；CENTER = 滑块几何中心从槽的
 *        起点边走到终点边，两端自然突出半个滑块(拉杆图，滑块比槽大也合法)；
 *      · 交互：按在滑块上拖(不掉手)、按在槽上直接跟手；松手吸附到最近的档；
 *        档位变化(拖动中实时)经 on_change 通知；
 *      · 滑块的移动是"直接落位 + 精确标脏"(老框 ∪ 新框)，没有动画依赖；
 *        要吸附动画的话，后续在 place 里换成 ui_anim 的定点移动即可(接口留在这一个点)。
 ****************************************************************************************************
 */

#ifndef __UI_MODULE_H__
#define __UI_MODULE_H__

#include <stdint.h>

#include "./ui_create/ui_create.h"        /* ui_rect_para_t / ui_box_of / ui_dirty_mark */
#include "./ui_framework/ui_framework.h"  /* ui_object_t / ui_event_t */

/* ==================================================================
 *  多档滑块(兼滚动条)
 * ================================================================== */

/* 行程模型：滑块中心能走到哪儿( init 按它把行程端点算好存进 c_min / c_max ) */
#define UI_SLIDER_INSIDE   0u   /* 整颗缩在槽里：槽两端各让出半个滑块(小旋钮 / 滚动条滑块)。
                                 * 滑块 >= 槽时行程退化成槽中心一个点(塞不下就是塞不下) */
#define UI_SLIDER_CENTER   1u   /* 中心走槽：滑块【几何中心】从槽的起点边走到终点边，
                                 * 两端自然突出半个滑块 —— 滑块比槽大也完全合法(拉杆图的画法) */

/* 吸附动画(动画版 / 无动画版自适应，不用加开关字段)：
 * 滑块对象用 ui_anim_register 注册了平移状态(动画版) -> 吸附/跳档滑过去(缓动)；
 * 没注册(无动画版) -> 一步到位。拖动跟手永远是即时的，动画只用于松手吸附和 ui_slider_set_value */
#define UI_SLIDER_SNAP_MS    180u   /* 一次吸附滑多久(ms) */
#define UI_SLIDER_SNAP_EASE  (-1)   /* 缓动：<0 = 减速缓进(和框架其他动画同一档) */

/* 滑块组件 para：首成员必须是 box(组件约定，见文件头)，后面才是滑块自己的字段 */
typedef struct
{
  ui_rect_para_t box;            /* 组件外接框：框架按矩形口径读它(约定见文件头) */

  ui_object_t   *track;          /* 槽对象(只建不挂，由组件带画) */
  ui_object_t   *thumb;          /* 滑块对象(只建不挂，画在槽上面) */

  uint8_t segments;              /* 档位数(>=2；init 会夹)。滚动条用法：档数 = 可滚的格数 */
  uint8_t horizontal;            /* 1 = 横向；0 = 纵向 */
  uint8_t travel;                /* 行程模型：UI_SLIDER_INSIDE / UI_SLIDER_CENTER(见上面的宏) */
  uint8_t value;                 /* 当前档 0..segments-1 */

  /* ---- 运行态(init 清零，外面不用碰) ---- */
  uint8_t grabbing;              /* 1 = 手指正按着拖 */
  uint8_t settling;              /* 1 = 吸附动画进行中(仅动画版)：ui_slider_task 每帧推一步 */
  int16_t settle_c;              /* 吸附目标中心(轴向) */
  int16_t grab_off;              /* 按下点相对滑块中心的轴向偏移(按在滑块哪儿就从哪儿拖) */
  int16_t c_min;                 /* 滑块中心行程下限(轴向；init 按槽/滑块几何算好) */
  int16_t c_max;                 /* 滑块中心行程上限 */

  /* 档位变了(拖动中实时)的通知；NULL = 不通知。程序设定(ui_slider_set_value)不算"用户改了"，不发 */
  void (*on_change)(ui_object_t *obj, uint8_t value);
}ui_slider_para_t;

/**
 * @brief       装配一个多档滑块：填 para + 接对象 + 按行程模型算端点 + 摆滑块到初值
 * @param       obj         : 组件对象(调用方先 UI_InitObject；之后照常注册进链/层级，
 *                            注册时 prev 给 NULL 或链尾都会让组件盖在别的对象上面 —— 它自己带画零件)
 * @param       para        : 组件参数(静态放一份；on_change 装配后按需挂)
 * @param       track       : 槽对象(只建不挂；盒子类型：矩形/图片/文本)
 * @param       thumb       : 滑块对象(同上；几何会被本函数改写——按初值摆位)
 * @param       segments    : 档位数(>=2，小了夹成 2)
 * @param       horizontal  : 1 = 横向；0 = 纵向
 * @param       travel      : 行程模型 UI_SLIDER_INSIDE / UI_SLIDER_CENTER(其他值当 INSIDE)
 * @param       value_init  : 初值 0..segments-1
 * @note        行程端点(c_min/c_max)由槽/滑块的几何 + 行程模型在本函数里算好；
 *              外接框 = 槽 ∪ 滑块在行程两端的极端框(CENTER 模式下含突出部分)，
 *              本函数算好写进 box(含 base_*)，调用方不用再管 box
 */
void ui_slider_init(ui_object_t *obj, ui_slider_para_t *para,
                    ui_object_t *track, ui_object_t *thumb,
                    uint8_t segments, uint8_t horizontal, uint8_t travel,
                    uint8_t value_init);

/**
 * @brief       读当前档位
 * @retval      0..segments-1
 */
uint8_t ui_slider_get_value(const ui_object_t *obj);

/**
 * @brief       程序设定档位：滑块直接落位并标脏(不发 on_change —— 那是"用户改了"才发的)
 * @note        开机建页时调它不脏(还没进屏，多标的那块会被整屏铺屏盖掉)，不用回避
 */
void ui_slider_set_value(ui_object_t *obj, uint8_t value);

/**
 * @brief       【动画版专属】吸附动画推进：主循环每帧(开帧后)调一次
 * @note        ui_anim 的定时移动是"每帧调一次才走一步"的轮询式(ui_anim_tick 只走时钟)，
 *              所以动画版要把本函数挂进主循环；没在吸附中(settling=0)时调用是空转；
 *              无动画版(滑块没注册平移动画)永远用不到它，调了也是空转。
 *              手指按下(UI_EV_DOWN)会自动取消吸附 —— 跟手要紧
 */
void ui_slider_task(ui_object_t *obj);


/* ==================================================================
 *  按钮
 * ================================================================== */

/* 外观：底色只在 bg 是【矩形】时由组件接管(c_normal/c_pressed/c_checked 写进 para->color)；
 * bg 是图片 / 文本时不改颜色，需要换图就在 on_click 里自己换。press_dy 是通用反馈：
 * 按下时把 label 下移几个像素，松开复位，图片底也能用。
 * 零件的 activation 会被遵守(未激活的零件不画)：像"选中才有底"就切换 bg 的激活状态 */
typedef struct
{
  ui_rect_para_t box;            /* 组件外接框：框架按矩形口径读它(约定见文件头) */

  ui_object_t   *bg;             /* 底对象(只建不挂，由组件带画) */
  ui_object_t   *label;          /* 文字 / 图标对象(只建不挂，画在底上面) */
  ui_object_t   *label_pressed;  /* 按下态标签(可选)；NULL = 按下时仍画 label(文字 / 图标不变) */
  ui_object_t   *label_checked;  /* 自锁选中态标签(可选)；NULL = 选中时仍画 label。
                                  * 想让"开始/暂停"这类两套文案跟状态绑死，就各给一个对象，
                                  * 颜色也各写在对象自己的 para 里，组件按 按下 > 选中 > 常态 选一个画 */

  ui_color_t c_normal;             /* 常态底色；0 = 不接管(保持 bg 自己的颜色) */
  ui_color_t c_pressed;            /* 按下底色；0 = 用 c_normal */
  ui_color_t c_checked;            /* 自锁选中底色；0 = 用 c_pressed */

  int16_t  press_dy;             /* 按下时 label 下移像素；0 = 不移 */
  uint8_t  enabled;              /* 1 = 可点(init 置 1) */
  uint8_t  toggle;               /* 1 = 自锁按钮：CLICK 翻转 checked(init 前设置) */

  /* ---- 运行态(init 清零，外面不用碰) ---- */
  uint8_t  pressed;              /* 1 = 手指正按着 */
  uint8_t  checked;              /* 自锁选中态(仅 toggle) */
  int16_t  label_cy;             /* label 常态中心 y(按下位移的基准) */
  int16_t  label_pressed_cy;     /* label_pressed 常态中心 y(同上；没挂就是 0) */
  int16_t  label_checked_cy;     /* label_checked 常态中心 y(同上；没挂就是 0) */

  /* CLICK 通知；NULL = 只做按下反馈 / 翻状态。自锁按钮回调里读 checked 就是新状态 */
  void (*on_click)(ui_object_t *obj);
}ui_button_para_t;

/**
 * @brief       装配一个按钮：填 para + 接对象 + 设外接框
 * @param       obj   : 组件对象(调用方先 UI_InitObject；之后照常注册进链/层级)
 * @param       para  : 组件参数(静态放一份；enabled/toggle/颜色/press_dy 在 init 前填好)
 * @param       bg    : 底对象(只建不挂；矩形 / 图片 / 文本)
 * @param       label : 文字 / 图标对象(同上；可以不传，NULL = 纯色块按钮)
 * @param       cx,cy : 外接框中心(命中 / 标脏按它算)
 * @param       w,h   : 外接框宽高
 * @note        c_normal 为 0 且 bg 是矩形时，自动取 bg 当前颜色当常态色；
 *              label_pressed / label_checked / toggle / 颜色 / press_dy 都是配置项，要在 init 之前填好
 *              (init 只记它们的常态基准，不清指针)；零件的几何由调用方摆好，本函数不改
 */
void ui_button_init(ui_object_t *obj, ui_button_para_t *para,
                    ui_object_t *bg, ui_object_t *label,
                    int16_t cx, int16_t cy, int16_t w, int16_t h);

/**
 * @brief       改按钮文字：换指针 + 长度，并按外接框标脏(立即生效)
 * @param       txt : GBK 字节串(只存指针不拷贝，内容要一直有效)
 * @param       len : 字节数；0 = 内部按 strlen 算
 * @note        只改常态标签 label；label_pressed / label_checked 是独立对象(按下 / 选中时的
 *              文字或图标)，不走这里 —— 它们通常随状态切换，不需要频繁改字
 */
void ui_button_set_text(ui_object_t *obj, const char *txt, uint16_t len);

/**
 * @brief       可点 / 不可点：不可点时按下与 CLICK 都不响应(外观保持常态)
 */
void ui_button_set_enabled(ui_object_t *obj, uint8_t on);

/**
 * @brief       设自锁选中态(仅 toggle 按钮)：立即刷新外观，不发 on_click
 */
void ui_button_set_checked(ui_object_t *obj, uint8_t on);

/**
 * @brief       读自锁选中态
 * @retval      1 = 选中；0 = 未选中 / 非自锁按钮
 */
uint8_t ui_button_get_checked(const ui_object_t *obj);


/* ==================================================================
 *  表格
 *    等宽等高的网格 + 一张合并表；合并块只保留左上角那格，被盖住的格子
 *    既不画也不占文字索引。整张表在绘制链里只有 1 个对象，格子/文字都在
 *    组件的绘制回调里现画(不建零件对象)。
 * ================================================================== */

/* 一个合并块：{左上角行, 左上角列, 跨几行, 跨几列}，行列都从 0 起 */
typedef struct
{
  uint8_t row;
  uint8_t col;
  uint8_t row_span;
  uint8_t col_span;
}ui_table_merge_t;

/* 一条文字：{行, 列, 第几段, 内容}——批量填表用哪格写哪格，不依赖书写顺序 */
typedef struct
{
  uint8_t row;
  uint8_t col;
  uint8_t sub;
  const char *txt;
}ui_table_text_t;

/* 表格 para：先填配置，再交 ui_table_init 算外接框。
 *
 * 文字索引固定为 ((row * cols) + col) * sub_max + sub：
 *   · 普通格 sub 只用 0；合并块也按【左上角】那格算，跨进去的其它行列会映射回来；
 *   · text 数组由调用方提供(组件只存指针，不拷贝)，字符串必须 '\0' 结尾；
 *   · 拷贝版另给 own_buf：rows * cols * sub_max * own_size 字节，NULL = 不支持拷贝版。 */
typedef struct
{
  ui_rect_para_t box;            /* 组件外接框(init 按 left/top/rows/cols/cell 算好) */

  int16_t  left, top;            /* 表格左上角(屏幕坐标) */
  int16_t  cell_w, cell_h;       /* 单元尺寸(含与右/下邻格共用的那条边) */
  uint8_t  rows, cols;

  const ui_table_merge_t *merges;/* 合并表；NULL = 没有合并 */
  uint8_t  merge_num;

  const char **text;             /* 平铺文字指针数组，索引见上 */
  uint8_t  sub_max;              /* 每格最多几段文字(1 = 只一段) */

  char    *own_buf;              /* 拷贝版落地缓冲；NULL = 不支持 */
  uint16_t own_size;             /* 每个文本框分到多少字节(含结尾 '\0') */

  int16_t  pad_y;                /* 格子内上下留白 */
  int16_t  line_gap;             /* 同一格内相邻两段文字的间隔 */

  ui_color_t line_color;           /* 网格线颜色 */
  ui_color_t text_color;
  uint16_t font_size;
  uint8_t  font_id;              /* 字模回调的字体关键码 */
  uint8_t  line_width;           /* 网格线宽(往内画) */
  ui_hz_glyph_fn_t hz_glyph_fn;  /* 取模回调；NULL = 走内部小字库 */
}ui_table_para_t;

/**
 * @brief       装配表格：算外接框、接上对象(cb = NULL，纯展示)
 * @param       obj  : 组件对象(调用方先 UI_InitObject；之后照常注册进链/层级)
 * @param       para : 组件参数(静态放一份；几何 / 合并表 / 文字表 / 颜色字体在调用前填好)
 * @note        表格自己带画网格线和文字，没有任何零件对象；要格子点击，
 *              装配完自己给 obj->cb 挂回调(命中框是整张表，回调里按坐标反算行列)
 */
void ui_table_init(ui_object_t *obj, ui_table_para_t *para);

/**
 * @brief       改某个格子里的文字(只换指针)
 * @param       row,col : 格子坐标；落在合并块跨度里的都算命中那个大格子
 * @param       sub     : 第几段文字(0 起，从上往下)
 * @param       txt     : '\0' 结尾的 GBK 串；只存指针，内容要一直有效
 * @retval      1 = 改好并标脏；0 = 行列越界 / sub 越界 / 没有 text 数组
 */
uint8_t ui_table_set_text(ui_object_t *obj, uint8_t row, uint8_t col,
                          uint8_t sub, const char *txt);

/**
 * @brief       批量写字：list 里一条 = 一格的一段文字，按 row/col/sub 各归各位
 * @param       n : list 的条数(sizeof 算就行)
 * @retval      1 = 全部写好并整表标一次脏；0 = 有越界条目(越界那条跳过，其余照写)
 * @note        合并块写左上角坐标即可；不依赖书写顺序，加/删/改一格只动 list 里那一条
 */
uint8_t ui_table_set_texts(ui_object_t *obj, const ui_table_text_t *list, uint16_t n);

/**
 * @brief       改文字(拷贝版)：内容拷进 own_buf 里属于这个文本框的那一段
 * @param       len : 字节数；超过 own_size - 1 会截断，内部补 '\0'
 * @retval      1 = 改好并标脏；0 = 参数越界 / 没配 own_buf
 */
uint8_t ui_table_set_text_copy(ui_object_t *obj, uint8_t row, uint8_t col,
                               uint8_t sub, const char *txt, uint16_t len);


/* ==================================================================
 *  滚动视图（滑窗）
 *    窗口固定、内容整体平移：超出窗口的部分被绘制裁剪掉；
 *    拖动只改"内容偏移"（内容真平移，不改窗口），松手时超出边界的偏移回弹。
 *
 *  用法：
 *    · 子对象自己 UI_InitObject 后填好 para / head.draw，**不要**注册进链或层级
 *      —— 它们由组件带画（和滑块的槽 / 滑块同一个约定）；
 *    · 初始时把内容按"内容左上角 = 窗口左上角"摆好（坐标用屏幕绝对坐标），
 *      再调 ui_scroll_init 告诉它内容总尺寸；
 *    · 内容比窗口小时对应方向自动不滚（max = 0）；
 *    · 拖动松手带惯性（速度用事件时间戳估算），惯性与回弹都按 ui_anim_now_ms() 的时间基准
 *      在固定时长内二次缓出 —— 帧率波动时滑行距离一致。
 *    · 轻点（位移没超过阈值）会把 DOWN / UP / CLICK 转派给点中的子对象：
 *      从最上层（数组末尾）往下找，没挂回调的当透明穿透 —— 列表里的按钮 / 卡片能正常点。
 *    · 组件本身不画背景、也没有自己的 alpha：窗口底色请在它下面单独放一个普通矩形对象；
 *      要半透明就设子对象（或那块底色）的 alpha。
 *    · 窗口可以圆角：装配前把 para->radius 填成和窗口底图一致的半径，
 *      组件用圆角裁剪把内容裁在窗口内，四个圆角不会漏内容（init 会夹到 min(w,h)/2）。
 *    · 窗口描边请挂在组件【后面】（后画的在上）：这样内容滚到边沿时盖不掉边框。
 * ================================================================== */

/* 拖动判定阈值(px)：手指位移超过它就算"拖动"，松手不派发点击 */
#define UI_SCROLL_TAP_SLOP    8

/* 松手速度(px/s)：上限防止一甩飞出去；门限以下当"轻推"，直接停 */
#define UI_SCROLL_VEL_MAX     3000
#define UI_SCROLL_VEL_START   120

/* 惯性 / 回弹的时间基准(ms)：
 *   惯性的目标偏移 = 当前偏移 + 松手速度 × COAST_TAU，在 COAST_MS 内二次缓出
 *   （初始速度 ≈ 松手速度）；越界橡皮筋的回弹用 RETURN_MS */
#define UI_SCROLL_COAST_MS     400u
#define UI_SCROLL_COAST_TAU_MS 200u
#define UI_SCROLL_RETURN_MS    200u

typedef struct
{
  ui_rect_para_t box;            /* 组件外接框 = 显示窗口(框架按矩形口径量框 / 标脏) */

  ui_object_t   *items;          /* 子对象数组(只建不挂；由组件带画) */
  uint16_t       item_num;       /* 子对象个数 */

  /* 可见裁剪用的基准位置(可选)：各给一个长度 = item_num 的 int16 数组。
   * 给了就启用"只动 / 只画窗口里的行"：每帧按"基准 - 偏移"算目标位置，
   * 只有旧框或新框碰到窗口的行才真的移动并标脏；屏外的行留着，等它进来时一次补上。
   * 不给(NULL) = 每帧整组移动，逻辑最简单，但屏外对象也走一遍移动计算 */
  int16_t       *item_base_x;
  int16_t       *item_base_y;

  int16_t        content_w;      /* 内容总宽(像素，>= 0)；<= 窗口宽时横向不滚 */
  int16_t        content_h;      /* 内容总高 */
  uint8_t        horizontal;     /* 1 = 允许横向滚动 */
  uint8_t        vertical;       /* 1 = 允许纵向滚动 */
  uint8_t        radius;         /* 窗口圆角半径(px)：0 = 直角；装配前填，
                                  * init 会夹到 min(w,h)/2，绘制时按圆角裁剪内容 */

  /* ---- 运行态(init 填，外面不用碰) ---- */
  int16_t        off_x;          /* 当前偏移：内容相对初始位置已经移动了多少(正 = 往左/往上) */
  int16_t        off_y;
  int16_t        max_x;          /* 偏移上限 = max(0, 内容宽 - 窗口宽) */
  int16_t        max_y;
  int16_t        grab_x;         /* 按下点(只用来算相对位移) */
  int16_t        grab_y;
  int16_t        base_x;         /* 按下那一刻的偏移 */
  int16_t        base_y;
  int16_t        settle_x;       /* 本次惯性 / 回弹的目标偏移 */
  int16_t        settle_y;
  int16_t        settle_from_x;  /* 本次惯性 / 回弹的起点偏移 */
  int16_t        settle_from_y;
  uint32_t       settle_ms;      /* 起点时刻(ui_anim_now_ms) */
  uint32_t       settle_time;    /* 本段总时长(ms) */
  int32_t        vel_x;          /* 松手速度(px/s)：惯性目标 = 偏移 + 速度 × COAST_TAU */
  int32_t        vel_y;
  uint32_t       last_ms;        /* 上一次拖动采样时间(ev->time_ms)，算瞬时速度用 */
  uint8_t        grabbing;       /* 1 = 手指正按着拖 */
  uint8_t        settling;       /* 1 = 惯性 / 回弹动画进行中(ui_scroll_task 每帧推一步) */
  uint8_t        moved;          /* 1 = 这次手势已超过点击阈值(松手不派发点击) */
  uint8_t        ticked;         /* 1 = init 已把 ui_scroll_task 登记进每帧任务表 */

  /* 偏移变了就通知(拖动 / 惯性 / 回弹中实时)；NULL = 不通知。
   * 程序设定(ui_scroll_set_offset)不算"拖动"，不发 */
  void (*on_scroll)(ui_object_t *obj, int16_t off_x, int16_t off_y);
}ui_scroll_para_t;

/**
 * @brief       装配一个滚动视图：填 para + 接对象 + 算滚动范围
 * @param       obj       : 组件对象(调用方先 UI_InitObject；之后照常注册进链/层级)
 * @param       para      : 组件参数(静态放一份)
 * @param       items     : 子对象数组(首地址；元素是 ui_object_t，组件只存指针)
 * @param       item_num  : 子对象个数(0 = 空窗口)
 * @param       cx, cy    : 窗口中心(屏幕坐标)
 * @param       w, h      : 窗口宽高
 * @param       content_w : 内容总宽；<= w 时横向不滚
 * @param       content_h : 内容总高；<= h 时纵向不滚
 * @param       horizontal: 1 = 允许横向滚动
 * @param       vertical  : 1 = 允许纵向滚动
 * @note        子对象必须"只建不挂"：本函数只记指针，绘制、裁剪、拖动都由组件负责；
 *              init 会自动把 ui_scroll_task 登记进每帧任务表(登记失败则退化成松手直接夹回)。
 *              可选：init 之前把 para->item_base_x / item_base_y 指向两个 int16 数组(长度 = item_num)，
 *              init 会填入各项当前中心，之后启用"只动 / 只画可见行"的裁剪；不填就整组移动 */
void ui_scroll_init(ui_object_t *obj, ui_scroll_para_t *para,
                    ui_object_t *items, uint16_t item_num,
                    int16_t cx, int16_t cy, int16_t w, int16_t h,
                    int16_t content_w, int16_t content_h,
                    uint8_t horizontal, uint8_t vertical);

/**
 * @brief       程序设定偏移(直接落位并标脏，夹到合法范围，不发 on_scroll)
 */
void ui_scroll_set_offset(ui_object_t *obj, int16_t off_x, int16_t off_y);

/**
 * @brief       读当前偏移(正 = 内容往左/往上移动了多少)
 * @param       off_x / off_y : 输出；不用给的方向传 NULL
 */
void ui_scroll_get_offset(const ui_object_t *obj, int16_t *off_x, int16_t *off_y);

/**
 * @brief       运行时改窗口几何（收起 / 展开动画用）
 * @param       cx, cy : 新的窗口中心(屏幕坐标)
 * @param       w, h   : 新的窗口宽高（<= 0 会被夹成 1）
 * @note        只改"裁剪窗"，内容偏移不动（内容锚在屏幕上，不跟着窗口走）。
 *                 · 按新尺寸重算滚动上限，并把当前偏移夹回合法范围；
 *                 · 正在跑的惯性 / 回弹就地停掉（窗口都在变，目标已无意义）；
 *                 · 老窗口 ∪ 新窗口各标一次脏，收起 / 展开露出来的那块都会重画；
 *                 · 给了 item_base_x / item_base_y 时会顺带同步可见项，展开不会露出过期位置。
 *              典型收起：每帧把 h 减小、cy 保持"固定顶边 + h/2"，再调本接口
 */
void ui_scroll_set_window(ui_object_t *obj, int16_t cx, int16_t cy, int16_t w, int16_t h);

/**
 * @brief       惯性 / 回弹推进：主循环每帧(开帧后)调一次
 * @note        按 ui_anim_now_ms() 的时间基准推进（每帧只算"过去了多少 ms"，
 *              帧率波动不影响滑行距离与时长）；没有在滑行 / 回弹时调用是空转。
 *              init 已自动登记，一般不用手动调；手工注销过任务表 / 想自己驱动时再用
 */
void ui_scroll_task(ui_object_t *obj);


/* ==================================================================
 *  每帧任务注册表
 *    所有"每帧要推进一次"的活都登记到这里：组件任务(滑块的吸附动画)
 *    和页面任务(柱状图左移、深度柱、顶部走秒、码表)都算。
 *    主循环开帧后只调 ui_module_task_all() 一次 —— 新增任务/新组件
 *    不再改 main 的调用列表，谁需要谁在装配时登记。
 * ================================================================== */

/* 容量上限 UI_MODULE_TASK_MAX 在 ui_config.h 第九节（属"可调参数"，由它统一管） */

/**
 * @brief       登记【组件任务】：带自己的对象，每帧回调一次
 * @param       obj  : 任务要操作的对象(原样回传给任务函数)
 * @param       task : 每帧调一次的函数(签名和 ui_slider_task 一致)
 * @retval      1 = 成功；0 = 参数为空 / 表满
 * @note        同一个对象重复登记 = 更新它的任务函数(不会重复入表)
 */
uint8_t ui_module_task_register(ui_object_t *obj, void (*task)(ui_object_t *obj));

/**
 * @brief       登记【页面 / 全局任务】：无参，每帧调一次
 * @param       task : 每帧调一次的函数(签名和 ui_bar_task / ui_info_task 一致)
 * @retval      1 = 成功；0 = 参数为空 / 表满
 * @note        同一个函数重复登记只入表一次(幂等)
 */
uint8_t ui_module_task_register_fn(void (*task)(void));

/**
 * @brief      注销一个组件任务(对象销毁 / 换页不用了时调)
 * @param       obj : 登记时给的对象；没登记过就什么都不做
 */
void ui_module_task_unregister(ui_object_t *obj);

/**
 * @brief       主循环每帧(开帧后、绘制前)调一次：按登记顺序跑所有任务
 * @note        执行顺序 = 登记顺序；任务函数自己负责"没事可做就立刻返回"。
 *              不要在任务里注销自己(遍历用固定下标，本帧后面还可能继续跑)
 */
void ui_module_task_all(void);


#endif /* __UI_MODULE_H__ */
