#ifndef UI_ANIM_H
#define UI_ANIM_H

#include <stdint.h>
#include <stddef.h>   /* NULL、offsetof */

#include "./linked_list/linked_list.h"

/* ============================================================================
 *  事件与回调 —— 交互层的地基
 *
 *  库不碰触摸驱动：应用在主循环里读 BSP 的触摸，然后喂给事件层 ——
 *  和字库那层"库只认一个读函数指针"是同一个思路，库保持零板级依赖。
 * ========================================================================== */

/* 事件类型。
 * 前三个是硬件给的；后三个是"合成事件"——硬件不会直接报，由事件层的状态机
 * 按时间 / 位置算出来，发不发、阈值多少都由事件层定 */
typedef enum
{
  UI_EV_NONE = 0,       /* 占位：不派发任何事件 */
  UI_EV_DOWN,           /* 手指按下 */
  UI_EV_MOVE,           /* 按住时移动(拖拽) */
  UI_EV_UP,             /* 手指抬起 */
  UI_EV_CLICK,          /* 合成：按下和抬起落在同一个对象上 */
  UI_EV_LONG_PRESS,     /* 合成：按住超过阈值 */
  UI_EV_CANCEL,         /* 合成：按下后移出对象再抬起，不算点击 ——
                         * 用来恢复"按下时改过的外观" */
}ui_ev_type_t;

/* 一次事件的全部信息。
 * 坐标一律【屏幕坐标】：和绘制、命中用的是同一个坐标系，谁都不用换算。
 * 子对象在自己的 cb 里拿到的也是屏幕坐标 —— 框架看不到 para 里的几何，
 * 换不了局部坐标；要局部坐标就自己减 head.center_x / center_y。
 * 以后加字段(滑动速度 / 压力…)直接往后面加，不影响已经挂上的回调 */
typedef struct
{
  ui_ev_type_t type;    /* 事件类型 */
  uint8_t      id;      /* 哪个触点(手指)：多指同时操作时靠它区分 */
  int16_t      x;       /* 触点位置(屏幕坐标) */
  int16_t      y;
  int16_t      dx;      /* 相对上一次事件的位移；按下的那一次是 0 */
  int16_t      dy;
  uint32_t     time_ms; /* 时间戳：长按 / 双击 / 滑动速度都要用 */
}ui_event_t;

struct ui_object_t;     /* 前向声明：下面的回调签名要引用它 */

/* 事件回调。
 * 挂上它就表示"这个对象可以被点到" —— 命中测试只认这个指针是不是 NULL，
 * 不看它指向什么，所以不用另加 hittable 标志。
 *
 * @param obj : 事件落在哪个对象上
 * @param ev  : 事件内容，只读
 * @retval 1=我处理了，事件到此为止(不再往后面的对象传)
 *         0=我没处理，继续往下传
 *
 * 回调里改对象状态是正常的，但改完要自己标脏 —— 和 ui_task_1pBLS.c 里其他改几何的
 * 地方一个规矩，不标就不会重画 */
typedef uint8_t (*ui_cb_t)(struct ui_object_t* obj, const ui_event_t* ev);

typedef enum
{
  deactivated,
  activated,
}ui_activation_t;


/* UI 对象：业务结构体。两个链表节点都嵌在这里，所以同一个对象能同时挂在
 * "操作链表"和"绘制优先级链表"上；两条链表作用范围不同、顺序互不影响：
 *   ops_node  → 所属链的 objs ：只管这一组对象的操作 / 遍历顺序（局部）
 *   draw_node → 所属层级的 draws：管整个层级(=整屏)的覆盖先后，跨链统一（全局） */
typedef struct ui_object_t
{
  void* para;                     /* 指向 ui_circle_para_t / ui_rect_para_t ... */

  ui_cb_t cb;                     /* 事件回调。非 NULL = 这个对象可被点到(命中测试的判据) */

  list_head_t ops_node;        /* 操作链表节点：挂进所属链的 objs 链表 */
  list_head_t draw_node;       /* 绘制优先级节点：挂进所属层级的 draws 链表 */

  struct ui_chain_t*  chain;      /* 本对象挂在哪条链上；没注册时为 NULL，注册后用于 O(1) 反查归属 */
  uint8_t registered;             /* 成功注册过的标志：走 UI_RegisterObject_Node 注册后自动置 1 */
  uint8_t inited;                 /* 结构已初始化的标志：走 UI_InitObject 后自动置 1
                                     和 registered 分开：注册 / 挂扩展只要求对象"已初始化"，不要求它"已挂载" */

  /* 上层扩展链表头（哨兵）：上面挂的是 ui_ext_node_t 的 node。
   * 框架只管"把上层的状态挂上去 / 按身份取回来"，不解释扩展里装的是什么，
   * 所以上层以后加多少种注册都只改上层，这里不用动。
   * exts 必须先由 UI_InitObject 建成空链（哨兵自指），挂扩展时才认得出"这个对象还没有扩展" */
  list_head_t exts;

  ui_activation_t activation;       /* 对象是否激活*/
}ui_object_t;


/* 对象扩展节点：上层模块要把自己的状态挂到一个 UI 对象上时，把本节点嵌进自己的结构体，
 * 再调 UI_RegisterObject_Ext 挂上去。框架只认"挂在哪个对象上、身份 key 是什么"，
 * data 指什么全由 key 所属的模块自己解释 —— 所以新加一种扩展只改上层，框架不动。
 * key 是扩展身份：同一个对象上同 key 只允许挂一个，也靠它把扩展取回来（UI_GetObject_Ext），
 * 习惯上取模块内一个静态对象的地址当 key */
typedef struct ui_ext_node_t
{
  list_head_t  node;   /* 挂到 ui_object_t::exts 上的节点（放首成员，便于按节点反推本结构体） */
  ui_object_t    *obj;    /* 挂在哪个对象上；没挂时为 NULL */
  const void     *key;    /* 扩展身份：区分同一对象上的不同扩展 */
  void           *data;   /* 扩展数据：由 key 所属模块自己解释 */
}ui_ext_node_t;


/* 一条 UI 链：一组对象的操作 / 遍历顺序。
 * 绘制优先级不在这里 —— 层级独占屏幕，覆盖关系是整屏的，所以 z 序收在所属层级上 */
typedef struct ui_chain_t
{
  list_head_t objs;            /* 操作链表头（哨兵）：上面挂的是 ui_object_t 的 ops_node */

  struct ui_hierarchy_t* level;   /* 本链挂在哪一层级上；没注册时为 NULL，注册后用于 O(1) 反查归属 */
  list_head_t node;            /* 本链挂到层级上的节点：挂在 ui_hierarchy_t::chains 上 */

  uint8_t registered;             /* 成功注册过的标志：走 UI_RegisterChain 注册后自动置 1 */
  uint8_t inited;                 /* 结构已初始化的标志：走 UI_InitChain 后自动置 1
                                     objs 必须是空链（哨兵自指），注册节点时才认得出"这条链还是空的" */
  /* 链级"组开关"：同一层级内的一组对象整体显隐 —— 一个"视图 / 浮层"就是一条链。
   * 门控收在三条遍历里（ui_level_draw / ui_chain_visit / ui_hit_test）：
   * 链去激活 = 它的对象既不画、也不被点到，链上顺序和 z 序一个字节都不动；
   * 对象自己的 activation 照旧独立生效（两层开关相与）。UI_InitChain 默认置 activated。
   * 写入走 ui_control 的 ui_chain_set_active()；标脏由调用方负责（链没有自己的几何）。
   * 典型用法见 APP/ui_task_home：考核视图一条链，切模式 = 换激活的链 */
  ui_activation_t activation;
}ui_chain_t;

/* 层级：一个父层级下挂不定量子层级，每个层级再挂不定量条 UI 链 */
typedef struct ui_hierarchy_t
{
  struct ui_hierarchy_t* parent;  /* 父层级；根层级没有父级，指向自己，回父级切换靠它（父子层级不循环） */

  list_head_t node;            /* 本层级挂到父层级下的节点：挂在 parent->children 上；
                                     多个根层级互为兄弟时直接挂在各自的 node 环上，根层级没有父级哨兵 */

  list_head_t children;        /* 子层级链表头（哨兵）：上面挂的是 ui_hierarchy_t 的 node；空时自指 */
  list_head_t chains;          /* 本层级挂载的链链表头（哨兵）：上面挂的是 ui_chain_t 的 node；空时自指 */
  list_head_t draws;           /* 本层级全部对象的绘制优先级（哨兵）：上面挂的是 ui_object_t 的 draw_node。
                                     层级独占屏幕，覆盖关系是整屏的，所以 z 序跨链统一收在这里，不分散在各链上：
                                     脏区检索只需一趟遍历本链，越靠后(靠近哨兵 prev)越上、后画的盖住先画的 */

  uint8_t registered;             /* 成功注册过的标志：走 UI_RegisterHierarchy 注册后自动置 1 */
  uint8_t inited;                 /* 结构已初始化的标志：走 UI_InitHierarchy 后自动置 1
                                     和 registered 分开：注册函数只要求父级"已初始化"，不要求父级"已挂载" */
  
  ui_activation_t activation;     /* 层级是否激活*/
}ui_hierarchy_t;

/* 注册结果 */
typedef enum
{
  UI_REG_FAIL,   /* 注册失败 */
  UI_REG_OK,     /* 注册成功 */
}ui_reg_result_t;


/* 初始化接口：层级 / 链在当 level、parent、chain 使用前必须先初始化 */
/* 初始化一个层级：建立"没有父级 / 没有兄弟层级 / 没有子层级 / 没有链表 / 没有可绘制对象"的初始状态
 * 层级对象在当 level 或当 parent 使用前都必须先初始化：
 * 要么调用本函数，要么静态初始化时把 node / children / chains / draws 四个哨兵都写成自指（LIST_HEAD_INIT）
 * 只建立结构、不参与挂载顺序，所以可以先初始化所有层级，再自底向上逐个注册 */
void UI_InitHierarchy(ui_hierarchy_t* level);
/* 初始化一条链：建立"没有节点 / 没有挂在层级上 / 没有前后链"的初始状态
 * 链在当 chain 使用前必须先初始化：
 * 要么调用本函数，要么静态初始化时把 objs / node 两个哨兵都写成自指（LIST_HEAD_INIT）
 * 只建立结构、不参与挂载顺序，所以可以先初始化所有链，再逐条挂到层级上；
 * 对象要在链挂到层级之后才能注册（draws 在层级上） */
void UI_InitChain(ui_chain_t* chain);
/* 初始化一个 UI 对象：建立"没有挂链 / 没有扩展 / 没有可绘制优先级"的初始状态
 * 对象在当 ui_obj 使用前必须先初始化：要么调用本函数，要么静态初始化时把
 * ops_node / draw_node / exts 三个哨兵都写成自指（LIST_HEAD_INIT）
 * 只建立结构、不参与挂载，所以可以先初始化对象、再设 para，最后走注册 / 扩展接口；
 * 注册 / 挂扩展都要求对象 inited == 1，否则 ops_node / exts 不可信 */
void UI_InitObject(ui_object_t* obj);


/* 注册接口；也可以在定义时直接静态初始化结构体，不走这些函数。
 * 对象的注册拆成两步，各自只管一条链、互相独立 —— 调用处一眼能看出它进了哪两条链：
 *   ① UI_RegisterObject_Chain(chain, obj, prev)：挂进 chain->objs —— 本链内的操作 / 遍历顺序（局部）
 *   ② UI_RegisterObject_Draw (level, obj, prev)：挂进 level->draws —— 整屏的绘制优先级 z 序（全局）
 * ①② 的插入口径一致：prev 都是"插到谁之后"，prev 传 NULL 都表示"插成第一条"
 * 于是三种状态都能表达：
 *   只 ①   → 能遍历 / 操作，但不在 draws 里，不会被画出来
 *   只 ②   → 只被画，不参与任何链的遍历 / 操作（静态装饰用得上）
 *   ① + ②  → 又画又能操作（常规对象） */

/* ① 把对象挂进操作链（只动 chain->objs）：
 *   ops_node 插在 prev 之后；prev 传 NULL 就是插成本链第一条。
 *  参数校验：chain / ui_obj 不能为 NULL；chain 必须是已初始化的链（inited == 1）；
 * ui_obj 必须是已初始化的对象（inited == 1）且还没挂过链（registered != 1）；
 * prev 非 NULL 时必须已注册过且确实挂在本链上（靠 ui_object_t->chain 反查）；
 * 校验不过直接返回 UI_REG_FAIL、什么都不改。
 * 成功后记下 ui_obj->registered = 1、ui_obj->chain = chain，② 和重复注册校验都靠这两个字段 */
ui_reg_result_t UI_RegisterObject_Chain(ui_chain_t* chain, ui_object_t* ui_obj, ui_object_t* prev);

/* ② 把对象挂进某个层级的绘制表（只动 level->draws）：
 *   draw_node 插在 prev 之后；prev 传 NULL 就是插成本层级绘制链的第一条（最底层）。
 *   绘制链越靠后的越上面、后画的盖住先画的 —— 想摆在最顶层就插到"当前最后一条"之后。
 *  层级是显式传的，所以 ② 与 ① 互相独立：不想挂操作链、只想被画出来的对象直接调 ② 即可
 *（"只画不操作"）。
 *  参数校验：level / ui_obj 不能为 NULL；level 必须是已初始化的层级（inited == 1，否则 draws 不可信）；
 * ui_obj 必须是已初始化的对象（inited == 1）；draw_node 不能已经挂在别的绘制表上；
 * prev 非 NULL 时必须已经挂在绘制表上（draw_node 已挂链）才能当插入点；
 * 校验不过直接返回 UI_REG_FAIL、什么都不改。
 * 【注意】常规对象传它所属链的那一层级（即 ui_obj->chain->level），z 序才和它的操作组对得上；
 * 传别的层级也能挂上，但那就成了"归在 A 页操作、画在 B 页"，除特殊用途外一般不是想要的 */
ui_reg_result_t UI_RegisterObject_Draw(ui_hierarchy_t* level, ui_object_t* ui_obj, ui_object_t* prev);

/* 合并版：一次做完 ① + ②，等价于先 UI_RegisterObject_Chain 再 UI_RegisterObject_Draw。
 * prev 同时当两条链的插入点：操作链插到 prev 之后、绘制链也插到 prev 之后
 *（两张表都是"插到 prev 之后"，prev 传 NULL 就是两条都插成第一条）。
 * 内部先把两步的校验都过一遍再动手，不会只挂上一半；校验规则和分开调两步完全一致，
 * 另外要求 prev（非 NULL 时）也必须已经挂在绘制表上，否则绘制链那步没处插。
 * 想写得更直观就分开调上面两个；只想一行搞定就用这个。
 * 另外：注册完想单独调 z 序，直接对 ui_obj->draw_node 调 ListDel + ListAddTail 即可，
 * 操作链（objs）的顺序不受影响 */
ui_reg_result_t UI_RegisterObject_Node(ui_chain_t* chain, ui_object_t* ui_obj, ui_object_t* prev);

/* 对象扩展接口：上层模块要把自己的状态挂到一个 UI 对象上时用这三支。
 * 框架只认"挂在哪个对象上、身份 key 是什么"，扩展里装什么全由 key 所属模块自己解释，
 * 所以上层以后加多少种注册都只改上层，框架不用动 */
/* 往对象上挂一个扩展节点：把上层模块的状态挂到 ui_obj 上
 *  参数校验：ui_obj / ext 不能为 NULL，ext->key 不能为 NULL（没身份就取不回来）；
 * ui_obj 必须是已初始化的对象（inited == 1，否则 exts 不可信）；
 * ext 不能已经挂在某个对象上；同一个对象上不允许挂第二个同 key 的扩展；校验不过直接返回 UI_REG_FAIL、不注册
 * 挂上后 ext->obj 记下归属，取回用 UI_GetObject_Ext(ui_obj, ext->key) */
ui_reg_result_t UI_RegisterObject_Ext(ui_object_t* ui_obj, ui_ext_node_t* ext);
/* 把扩展节点从它挂着的对象上摘下（摘完 ext->obj 清空、可以再挂到别的对象上）；没挂过就什么都不做 */
void UI_UnregisterObject_Ext(ui_ext_node_t* ext);
/* 在 ui_obj 上找身份是 key 的扩展，返回它的 data；没找到（或对象还没初始化 / 还没挂过任何扩展）返回 NULL */
void* UI_GetObject_Ext(ui_object_t* ui_obj, const void* key);

/* 注册一个层级：把 level 挂到 parent 下、插在同一父级的上一兄弟层级 prev 之后
 *  parent 填 NULL 表示注册根层级，此时 level->parent 指向自己；prev 填 NULL 表示挂成父层级的第一条子层级
 *  支持自底向上注册：先调用 UI_RegisterHierarchy 把子层级挂到 level 下，再调用本函数把 level 向上挂到 parent 下，
 * parent 不要求已注册过，只要求已初始化，level 已有的子层级也不会因为这次注册而丢失
 *  参数校验：level 不能为 NULL、必须是已初始化的层级（inited == 1）、不能是已注册过的层级、不能自己当自己的父级；
 * parent 非 NULL 时也必须是已初始化的层级（inited == 1）；
 * prev 非 NULL 时必须已注册过且确实挂在同一个父级下
 * （根层级互为兄弟时，prev->parent 指向 prev 自己），校验不过就直接返回 UI_REG_FAIL、不注册 */
ui_reg_result_t UI_RegisterHierarchy(ui_hierarchy_t* level, ui_hierarchy_t* parent, ui_hierarchy_t* prev);
/* 注册一条 UI 链：把 chain 挂到 level 里、插在同一层级的上一条链 prev 之后
 *  prev 填 NULL 表示挂成本层级的第一条链（插到 level->chains 哨兵之后）
 *  参数校验：level / chain 不能为 NULL，level 必须是已初始化的层级（inited == 1）、chain 必须是已初始化的链（inited == 1）；
 * chain 不能是已注册过的链；prev 非 NULL 时必须已注册过且确实挂在本层级下
 * 层级内的链是环形：哨兵的 next 是第一条、prev 是最后一条，环上不存在 NULL；
 * 只有一条链时哨兵的 prev / next 都指向它；校验不过直接返回 UI_REG_FAIL、不注册 */
ui_reg_result_t UI_RegisterChain(ui_hierarchy_t* level, ui_chain_t* chain, ui_chain_t* prev);


/* ============================================================================
 *  激活门控：只有【已激活】的对象才参与绘制和操作
 *
 *  状态存在 ui_object_t::activation。UI_InitObject 把它默认置成 activated ——
 *  对象建好就是"可见可操作"的，想藏起来用 ui_control 的 ui_obj_set_active(obj, 0)。
 *
 *  门控只收在下面这两个遍历接口里（不改链表结构，只是在走链时跳过）：
 *  激活 / 去激活只改一个字段，O(1)，链上的顺序和位置一个字节都不动。
 *
 *  【应用侧请走这两个接口】不要自己写 ListForEachEntry(&level->draws, ...) ——
 *  绕过去的话，对象藏了还会被画出来 / 还会被点到。
 * ========================================================================== */

/* 这个对象现在激活着吗(1=激活)。传 NULL 返回 0 */
uint8_t ui_obj_is_active(const ui_object_t* obj);

/* 按 z 序（越靠后越上）把 level 上【已激活且 para 非空】的对象逐个交给 visit。
 * 场景绘制函数里用它替掉手写的 ListForEachEntry(&level->draws, ...)：
 *     static void app_draw_object(ui_object_t* obj) { ui_draw_dispatch(obj->para); }
 *     ui_level_draw(&page_level, app_draw_object); */
void ui_level_draw(ui_hierarchy_t* level, void (*visit)(ui_object_t* obj));

/* 按操作链顺序遍历 chain 上【已激活】的对象，逐个交给 visit。
 * 给"光标找目标 / 触摸命中查询 / 整组操作"这类用途。
 * visit 返回 1 表示"找到了，停下"，返回 0 就继续走。
 * @retval 1=被 visit 中途停下; 0=整条链都走完了 */
uint8_t ui_chain_visit(ui_chain_t* chain, uint8_t (*visit)(ui_object_t* obj));


#endif /* UI_ANIM_H */
