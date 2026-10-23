#ifndef __UI_ANIM_H__
#define __UI_ANIM_H__

#include <stdint.h>
#include "./ui_framework/ui_framework.h"   /* ui_object_t */
#include "./ui_create/ui_create.h"   /* ui_head_t / ui_cursor_para_t / ui_dirty_mark */
/* 1ms 心跳：应用在自己的 1ms 定时器里调它，ui_anim_ms 才会往前走。
 * 开机时把定时器接到它上面(接一次就行)：
 *     timer3_set_callback(ui_anim_tick, NULL);
 * 不接的话所有动画都停在 0 时刻，进度永远不动。
 *
 * 参数 arg 用不到：留着是为了能【直接】当定时器回调挂上去 ——
 * 定时器驱动给回调一般都会带一个 arg，这样应用就不用再包一层。
 *
 * 为什么把心跳露出来、而不是库自己去挂定时器：
 * 定时器是板级的。库以前直接 include TIMER/timer.h 去调 timer3_set_callback()，
 * 换芯片就得先凑一个同名驱动出来。现在改成"应用喂心跳"，和字库(读点阵函数)、
 * 触摸(喂事件)是同一个模式：库只管算法，板子的东西全在应用侧 */
void ui_anim_tick(void *arg);

/* 当前 UI 动画时钟(ms)：ui_anim_tick 每 1ms 加一，和事件层给的时间戳同一块 TIM3 时基。
 * 给"按时间推进"的组件用 —— 比如滚动视图的惯性和回弹：
 * 每帧只算"过去了多少 ms"，不假设固定帧率，帧率波动时滑行距离才一致。
 * 没接 ui_anim_tick 时它恒为 0，按它算的 dt 会被调用方兜成 0/1ms */
uint32_t ui_anim_now_ms(void);


/* ---- 动画状态与注册 --------------------------------------------------------
 * 每个要用"连续移动 / 连续形变"的对象，都得先注册一份属于它自己的状态，动画接口才认它。
 * 注册走框架的对象扩展机制：ui_anim 只对外要一个 ui_anim_ext_t（扩展节点 + 各动画状态指针），
 * 调一次 ui_anim_register 把它挂到对象上；框架那边只当"看板"，不认动画是什么。
 * 于是以后 ui_anim 再加多少种动画状态，都只改 ui_anim 自己，框架不用动。
 * 状态里的进度字段由 ui_anim 维护，调用者不用碰，也不必初始化（注册会清零）。
 * 没注册过的对象调动画接口：直接什么都不做 */

/* 平移动画状态：一个对象一份。给 ui_anim_move_obj_to / ui_anim_move_obj_follow 用 */
typedef struct ui_move_anim_t
{
  int16_t  start_x;      /* 本段轨迹的出发点中心 */
  int16_t  start_y;
  int16_t  target_x;     /* 本段轨迹的终点中心 */
  int16_t  target_y;
  uint32_t start_ms;     /* 出发时 ui_anim_ms 的快照，作为 0 时刻 */
  uint32_t duration;     /* 总时长 ms */
  uint16_t ease;         /* 本段缓动选择（定点/跟随共用，有符号）：0线性 / >0超调回弹 / <0减速缓进 */
  uint8_t  follow;       /* 这一段是"跟随"模式（目标会连续移动）还是"到定点"模式 */
} ui_move_anim_t;

/* 形变动画状态：一个对象一份。给 ui_anim_transform_rect_cursor_continuous 用 */
typedef struct ui_transform_anim_t
{
  int16_t  start_w;      /* 起跑时的参数快照 */
  int16_t  start_h;
  int16_t  start_border_width;
  ui_color_t start_color;
  int16_t  start_gap;
  int16_t  target_w;     /* 终点参数 */
  int16_t  target_h;
  int16_t  target_border_width;
  ui_color_t target_color;
  int16_t  target_gap;
  uint32_t start_ms;     /* 起跑时 ui_anim_ms 的快照，作为 0 时刻 */
  uint32_t duration;     /* 总时长 ms */
  int16_t  ease;         /* 本段缓动选择（跟平移动画同一套）：0线性 / >0超调回弹 / <0减速缓进 */
  uint8_t  active;       /* 1 = 进行中，0 = 已到终点 */
  uint8_t  follow;       /* 这一段是"跟随"模式（目标会连续变化）还是"到定点"模式 */
} ui_transform_anim_t;

/* 折线缩放动画状态：一个对象一份。给 ui_anim_scale_polyline_continuous 用。
 * 折线只存"基准坐标 + 当前缩放比例(scale_num / scale_den)"，缩放是绝对比例不是增量；
 * 这里按 1ms 基准在 起点比例 与 终点比例 之间线性插值出这一步的比例，
 * 再交给 ui_anim_scale_obj 绝对写入 —— 逐帧反复调也不累积取整误差 */
typedef struct ui_poly_scale_anim_t
{
  uint16_t start_num;    /* 起跑时的比例快照 */
  uint16_t start_den;
  uint16_t target_num;   /* 终点比例 */
  uint16_t target_den;
  uint32_t start_ms;     /* 起跑时 ui_anim_ms 的快照，作为 0 时刻 */
  uint32_t duration;     /* 总时长 ms */
  uint8_t  active;       /* 1 = 进行中，0 = 已到终点 */
} ui_poly_scale_anim_t;

/* 进度动画状态：一个进度条对象一份。给 ui_anim_progress_bar_continuous 用。
 * 进度是"绝对当前值"，这里按 1ms 基准在 起点进度 与 终点进度 之间线性插值，
 * 再绝对写入 para->progress —— 逐帧反复调也不累积取整误差 */
typedef struct ui_progress_anim_t
{
  uint16_t start_value;  /* 起跑时的进度快照 */
  uint16_t target_value; /* 终点进度 */
  uint32_t start_ms;     /* 起跑时 ui_anim_ms 的快照，作为 0 时刻 */
  uint32_t duration;     /* 总时长 ms */
  uint8_t  active;       /* 1 = 进行中，0 = 已到终点 */
} ui_progress_anim_t;

/* ui_anim 挂到对象上的扩展：把通用扩展节点和本对象要用的各动画状态指针捆在一起。
 * 用法：静态定义一个，把要用的状态指针填进去，再调 ui_anim_register 一次挂到对象上。
 * 某个动画不用就把对应指针填 NULL（取不到状态，那种动画对这个对象就是什么都不做） */
typedef struct ui_anim_ext_t
{
  ui_ext_node_t        ext;       /* 通用扩展节点：挂到对象 ui_object_t::exts 上 */
  ui_move_anim_t      *move;      /* 这个对象的平移状态；不用平移填 NULL */
  ui_transform_anim_t *shape;     /* 这个对象的形变状态；不用形变填 NULL */
  ui_poly_scale_anim_t *poly_scale; /* 这个对象的折线缩放状态；不用填 NULL */
  ui_progress_anim_t  *progress;  /* 这个对象的进度动画状态；不用填 NULL */
} ui_anim_ext_t;

/* 把 ext 挂到 obj 上，并把 obj 要用的动画状态一次登记好。
 * move / shape / poly_scale / progress 填要用的状态实例（调用者定义，一般静态放一个），不用就填 NULL。
 * 重复注册同一个 ext = 重置：先解绑原来挂着的对象，再清掉状态重新登记。
 * 成功后 move / shape / poly_scale / progress 会被清零（进度由 ui_anim 从零记起）。
 * obj 为 NULL 表示只解绑 ext、不绑新对象。
 * 返回框架注册结果；失败则什么都没绑定 */
ui_reg_result_t ui_anim_register(ui_anim_ext_t *ext, ui_object_t *obj,
                                 ui_move_anim_t *move, ui_transform_anim_t *shape,
                                 ui_poly_scale_anim_t *poly_scale, ui_progress_anim_t *progress);

/* 这个对象注册过【平移】动画状态吗(1=注册过：move 状态可用)。
 * 组件用它可以做"动画版 / 无动画版"自适应：注册了就走缓动(动画版)，
 * 没注册就直接落位(无动画版) —— 用不用动画由"挂不挂状态"决定，组件不用加开关字段 */
uint8_t ui_anim_has_move(const ui_object_t *obj);


/* 按对象类型分身平移：查内部的 anim_move_obj_tab，把活派给对应类型的 ui_move_xxx。
 * 光标不做追目标：它按自己的方框挪（跟随目标交给 ui_cursor_fit）。
 * type 没登记（NONE / 脏值）或表项为 NULL 就什么都不动。
 * dx / dy 由调用者传，函数把实际生效的位移回写回来（没动就写 0） */
void ui_anim_move_obj(ui_object_t *obj, int16_t delta_x, int16_t delta_y);

/* 把对象"正在跑的定点移动"整条轨迹平移 dx / dy：起点和终点一起挪，进度不重置。
 * 用途：对象所在容器(视窗 / 合并对象)被整体搬动时 —— 容器已经把对象搬过去了，
 * 这时如果只改终点，定点移动会按【旧起点】算绝对位置、把对象往旧轨迹上拉(表现就是
 * 滑动卡住 / 错位)；起点也挪同样多，对象就沿平移后的轨迹继续走，不卡住也不重跑。
 * 跟随档内部对"每帧都在动的目标"用的就是这个手法。
 * 没注册平移状态就直接返回；dx / dy 都为 0 时什么都不做 */
void ui_anim_move_obj_shift(ui_object_t *obj, int16_t dx, int16_t dy);

/* 定点移动：让对象在 time 毫秒内从当前中心连续走到 (target_x, target_y)。
 * 非阻塞，每帧从主循环调一次即可：内部按 1ms 基准 ui_anim_ms 算进度、挪一小步，
 * 脏区交给 ui_anim_move_obj 记；走到 time 毫秒就停在终点。
 * 前提：对象已用 ui_anim_register 注册过平移状态，这里才认；没注册什么都不做。
 * 同一目标重复调就是"接着走"（无副作用）；目标一变就从当前中心重新起跑一段
 * （本接口把目标变化当成"换了目的地"）。要跟住会动的目标请用 ui_anim_move_obj_follow。
 * ease 是缓动选择（三种，只能选一种）：
 *   0        = 线性匀速；
 *   > 0      = easeOutBack 超调回弹，走到底时先冲过终点一点再回正，数值即强度（10 约超出 10%，上限 30）；
 *   < 0      = easeOut 减速缓进，从头一路减速到终点（二次缓出），全程不超调；数值只用来选档（取负即可）。
 * time 为 0 或 para 为空直接返回。前提：初始化时调过 ui_anim_tick()，ui_anim_ms 才会走 */
void ui_anim_move_obj_to(ui_object_t *obj, int16_t target_x, int16_t target_y,
                         uint32_t time, int16_t ease);

/* 跟随移动：让对象咬住一个会连续移动的目标（比如横向往复的文本）。
 * 非阻塞，每帧从主循环调一次即可。
 * 起跑后按 time 毫秒用 ease 缓动追上目标（与其他切换用同一套缓动，观感一致），
 * 到点后 elapsed 夹在 time，want 正好落在目标上，随后每帧锁定实时目标、精确贴住不再拖尾。
 * 追的过程中目标每帧在动时，把整条轨迹平移同样的量，于是不掉队也不重置进度。
 * ease 是缓动选择（三种，只能选一种）：
 *   0        = 线性匀速；
 *   > 0      = easeOutBack 超调回弹，走到底时先冲过终点一点再回正，数值即强度（上限 30）；
 *   < 0      = easeOut 减速缓进，从头一路减速到终点（二次缓出），全程不超调；数值只用来选档（取负即可）。
 * 前提：对象已用 ui_anim_register 注册过平移状态；没注册什么都不做。
 * 前提：目标位置本身是连续的；目标会跳变时（比如换选中对象）改用 ui_anim_move_obj_to。
 * time 为 0 或 para 为空直接返回。前提：初始化时调过 ui_anim_tick()，ui_anim_ms 才会走 */
void ui_anim_move_obj_follow(ui_object_t *obj, int16_t target_x, int16_t target_y,
                             uint32_t time, int16_t ease);

/* 按对象类型分身缩放：查内部的 anim_scale_obj_tab，把活派给对应类型的 ui_scale_xxx。
 * 光标不做追目标：它按自己的方框缩（跟随目标交给 ui_cursor_fit）。
 * 新尺寸 = 原尺寸 * num / den：num > den 放大、num < den 缩小。
 * 文字参与缩放，但只缩"文本可写范围"这个框，字号不跟着缩（见 ui_scale_text）。
 * type 没登记（NONE / 脏值）或表项为 NULL 就什么都不动。
 *
 * 【最重要的注意事项 —— 用之前先看这段】
 * 本接口是【绝对比例】：一切都拿【确定态】(base_*) 重算，隐含前提是
 *     "当前几何 == 基准几何"（即形变层是空的，虚拟几何只是基准乘了上一轮缩放因子）。
 * 所以任何"改了几何却没 bake"的改动（形变，或绕过接口手写 w / h / center）都会在下一次缩放时被丢掉。
 * 三类对象丢的东西不一样，别一概而论：
 *
 *   ① 单体（矩形 / 文字 / 圆 / 点 / 进度条）
 *      尺寸 = base_w / base_h × 因子            —— 尺寸回基准；
 *      中心 = 锚点 + xf.anchor_off × 因子，而 anchor_off 被形变刷成过"虚拟中心 - 锚点"，
 *      于是【尺寸取确定态、偏移取虚拟态】，两边基准不同 → 算出的中心配不上新的 w / h
 *      → 锚点跑到盒子外面，那条本该不动的边会飘。
 *      例：底边锚点、base 90×50 形变到 220×160 之后调 scale(1,1)（本该什么都不动），
 *          尺寸缩回 90×50，底边从 470 跳到 415。注意中心本身并没有回基准，是"脱钩"造成的假象。
 *
 *   ② 视窗自己
 *      尺寸和中心都从 base_w / base_h、base_cx / base_cy 重算 —— 尺寸 + 中心一起回确定态。
 *      但 base_cx / base_cy 会跟着 ui_move_viewport 的平移一起走，视窗又没有形变接口，
 *      所以只有"手写 para 的 w / h / center 却没同步 base"这种写法才会被拉回。
 *      （合并对象自己没有中心基准，它的框是缩放后从子对象并集重新量的，见 ui_scale_union）
 *
 *   ③ 合并对象 / 视窗里的子对象（两者共用 ui_union_scale_child）
 *      尺寸 = 子对象自己的 base_w / base_h × 因子；
 *      位置 = 容器锚点 + node->base_rel_x / base_rel_y × 因子 —— 完全不看子对象的当前中心。
 *      base_rel 是"成组那一刻"的快照（ui_union_append / ui_viewport_append 里对每个子对象 bake 的），
 *      所以子对象被单独平移 / 形变过没 bake，一次缩放就把它摆回"成组时的位置 + 基准尺寸"。
 *      （容器整体平移是安全的：ui_move_union / ui_move_viewport 把容器锚点和子对象一起挪，
 *        base_rel 是相对锚点量，相对关系不变）
 *
 * 结论：要"形变 + 缩放"叠加，或子对象单独动过之后还要缩放容器，先 ui_obj_bake() 提交确定态。
 * 逐帧反复调同一个比例不累积（幂等）：因为每次都从 base 现算，不是拿当前几何乘增量 */
void ui_anim_scale_obj(ui_object_t *obj, uint16_t num, uint16_t den);

/* 折线连续缩放：让折线在 time 毫秒内从当前缩放比例连续变到 num / den（锚点仍是起始点）。
 * 折线缩放是"绝对比例"，这里按 1ms 基准在 起点比例 与 终点比例 之间线性插值，
 * 再交给 ui_anim_scale_obj 绝对写入 —— 逐帧反复调也不累积取整误差。
 * 非阻塞，每帧从主循环调一次即可；到 time 毫秒就停在终点。
 * 同一目标/时长重复调就是"接着走"；换了目标 / 时长则用当前比例重新起跑。
 * 前提：对象已用 ui_anim_register 注册过 poly_scale 状态；没注册什么都不做。
 * time 为 0、den 为 0 或 para 为空直接返回。前提：初始化时调过 ui_anim_tick() */
void ui_anim_scale_polyline_continuous(ui_object_t *obj, uint16_t num, uint16_t den, uint32_t time);

/* 进度条连续动画：让进度条在 time 毫秒内从当前进度连续变到目标进度（圆环 / 矩形通用）。
 * 进度是"绝对当前值"，这里按 1ms 基准在 起点进度 与 终点进度 之间线性插值，
 * 再绝对写入 para->progress —— 逐帧反复调也不累积取整误差；每步按进度条外框整块标脏。
 * 非阻塞，每帧从主循环调一次即可；到 time 毫秒就停在终点。
 * 同一目标/时长重复调就是"接着走"；换了目标 / 时长则用当前进度重新起跑。
 * 目标超过 max_value 时按 max_value 夹住。
 * 前提：对象已用 ui_anim_register 注册过 progress 状态；没注册什么都不做。
 * time 为 0 或 para 为空直接返回。前提：初始化时调过 ui_anim_tick() */
void ui_anim_progress_bar_continuous(ui_object_t *obj, uint16_t target_value, uint32_t time);




/* ---- 对外接口声明 ---------------------------------------------------------- */

/* 立即形变（一步到位）：把矩形 / 光标直接改成目标尺寸 / 线宽 / 颜色 / 间距。
 * 缩放锚点用的是【对象自带的锚点】(ui_obj_set_anchor 设过的那个)：
 *   设过 → 锚点相对盒子的位置不变：把它当成盒子上一点，到四边的点距随尺寸等比缩放。
 *          锚点恰好落在某条边上（比如下边中点）→ 那条边严格不动，跟尺寸奇偶无关；
 *   没设过 → 中心不动（旧行为）。
 * 所以想控制"从哪边长"，就在确定态调 ui_obj_set_anchor()，且锚点要落在那条边上
 * （用那条边的行列坐标，不是"中心 ± 一半"）。
 * 形变本身用不到基准尺寸，ui_obj_bake() 不是必须的（基准只给缩放 / 复位用）。
 * 形变是"直接改几何"，属于【未确定态】的改动；但它改完会把 xf.anchor_off 重刷成
 * "当前中心 - 锚点"（anchor_set != 0 时），让"中心 = 锚点 + 偏移 × 因子"这个不变量继续成立 ——
 * 所以【形变完再平移不会被拉回】：平移只挪锚点，再按这个式子反推中心。
 * 会被"按基准拉回"的是【缩放】和【复位】，改完还要走这两条就先 ui_obj_bake() 提交确定态：
 *   缩放：尺寸一律按 base_* 重算，不 bake 就等于把形变丢掉 —— 分对象的差异和后果
 *         详见上面 ui_anim_scale_obj 那段"最重要的注意事项"；
 *   复位（ui_obj_restore）直接读回 base_* —— 不 bake 就还原成形变前的几何。
 * 另注：既没 ui_obj_set_anchor() 也没 ui_obj_bake() 过的对象，anchor / anchor_off 还都是 0，
 * 平移时中心会被算成"锚点 + 0"而闪走 —— 平移前这两个接口至少要走一个。
 * 光标没有锚点（它没有变换块），一律中心不动。
 * 只认 UI_TYPE_RECT / UI_TYPE_CURSOR，且要和对象实际类型一致，否则直接返回。
 * 旧框 + 新框并集记一次脏区；时间类形变接口内部也靠它落地。
 * 无状态，不需要 ui_anim_register 也能用 */
void ui_anim_transform_rect_cursor(ui_object_t *obj, int16_t w, int16_t h,
                                   int16_t border_width, ui_color_t color,
                                   int16_t gap, ui_type_t type);



/* 连续形变：让矩形 / 光标在 time 毫秒内连续变形到目标尺寸、线宽、颜色、间距。
 * 中心动不动看锚点：设过锚点就绕锚点走（锚点所在那条边严格不动、中心随之挪）；
 * 没设过锚点才中心不动。确定态（base_*）一律不动，要提交就自己调 ui_obj_bake()。
 * 非阻塞，每帧从主循环调一次即可：内部按 1ms 基准 ui_anim_ms 算进度、插值出一小步，
 * 再交给 ui_anim_transform_rect_cursor 落地，旧框/新框与脏区由它记；到 time 毫秒就停在终点。
 * 同一目标/时长/缓动重复调就是"接着走"；换了目标 / 时长 / 缓动则用当前参数重新起跑。
 * ease 是缓动选择（跟平移动画同一套，只能选一种）：
 *   0 = 线性匀速；> 0 = easeOutBack 超调回弹；< 0 = easeOut 减速缓进（数值只用来选档）。
 * 给 0 就是原来的线性形变；要让光标"中心动"和"尺寸动"两条动画同进度（比如框一条
 * 中心 + 尺寸同时在变的折线），就给和它的平移动画一样的 ease，两条曲线才不错位。
 * 前提：对象已用 ui_anim_register 注册过形变状态；没注册什么都不做。
 * 只认 UI_TYPE_RECT / UI_TYPE_CURSOR，且要和对象实际类型一致。
 * time 为 0 或 para 为空直接返回。前提：初始化时调过 ui_anim_tick()，ui_anim_ms 才会走 */
void ui_anim_transform_rect_cursor_continuous(ui_object_t *obj, int16_t w, int16_t h,
                                              int16_t border_width, ui_color_t color,
                                              int16_t gap, ui_type_t type, uint32_t time,
                                              int16_t ease);

/* 跟随形变：让矩形 / 光标咬住一个尺寸会连续变化的目标（比如正在缩放的矩形外框）。
 * 非阻塞，每帧从主循环调一次即可。
 * 起跑后按 time 毫秒从当前尺寸缓动追上目标，到点后 elapsed 夹在 time，
 * 插值结果正好落在实时目标上，随后每帧锁定目标、精确贴住不再拖尾。
 * 追的过程中目标每帧在变时，把整条轨迹（起点 + 终点）平移同样的量，
 * 于是缓动进度不重置、掉不了队 —— 这正是定点档 ui_anim_transform_rect_cursor_continuous
 * 在目标每帧都变时会每帧重跑、追不上的原因所在。
 * 前提：对象已用 ui_anim_register 注册过形变状态；没注册什么都不做。
 * 只认 UI_TYPE_RECT / UI_TYPE_CURSOR，且要和对象实际类型一致。
 * 目标尺寸会跳变时（比如换选中对象）改用 ui_anim_transform_rect_cursor_continuous 起一段定点动画。
 * time 为 0 或 para 为空直接返回。前提：初始化时调过 ui_anim_tick()，ui_anim_ms 才会走 */
void ui_anim_transform_rect_cursor_follow(ui_object_t *obj, int16_t w, int16_t h,
                                          int16_t border_width, ui_color_t color,
                                          int16_t gap, ui_type_t type, uint32_t time,
                                          int16_t ease);

#endif
