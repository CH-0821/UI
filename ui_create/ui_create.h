#ifndef __UI_CREATE_H__
#define __UI_CREATE_H__

#include <stdint.h>
#include "./linked_list/linked_list.h"
#include "./ui_framework/ui_framework.h"

/* UI_GLYPH_MAX_BYTES / UI_TEXT_MAX_LINES 等可调参数在 ui_config.h */
#include "./ui_config/ui_config.h"

/* 通用脏区登记 / 取片 / 行缓冲推送 —— 都搬到 ui_dirty_areas 模块了。
 * ui_row_chunk_t / ui_dirty_span_t 和 ui_dirty_mark / ui_dirty_take / ui_row_buf_* 接口由它声明 */
#include "./ui_dirty_areas/ui_dirty_areas.h"

/* 矩形 / 光标"移动或形变"之后登记脏区：只标它真正画出来的四条描边带
 * （上 / 下两条整宽横带 + 左 / 右两条竖带），old 与 new 两个框的描边都包进去，
 * **不标矩形内部**（内部是背景，被框住的其它对象会各自把露出来的部分标脏）。
 * 于是光标一帧只推"两条 3px 竖带 + 两条 3px 横带"，而不是整个大框；
 * 那两条竖带又窄又高，各是一次 DMA 推完的一条 —— 分条错位（台阶）就是这么消掉的。
 * 形状相关，放在本模块（矩形 / 光标）里实现；参数是 old / new 两个框的四条边（含端点），
 * border_width 是描边宽度 */
void ui_dirty_mark_rect_outline(int16_t x0_old, int16_t y0_old, int16_t x1_old, int16_t y1_old,
                                int16_t x0_new, int16_t y0_new, int16_t x1_new, int16_t y1_new,
                                int16_t border_width);

/* 矩形类对象"移动 / 缩放 / 形变"后的脏区登记：old / new 两个框都算进去。
 * filled == 0（描边矩形 / 光标）：只标四条描边带，与上面同；
 * filled != 0（实心填充矩形）：整框都画了，标整框 */
void ui_dirty_mark_rect_move(int16_t x0_old, int16_t y0_old, int16_t x1_old, int16_t y1_old,
                             int16_t x0_new, int16_t y0_new, int16_t x1_new, int16_t y1_new,
                             int16_t border_width, uint8_t filled);

/* UI 类型标签：决定 para 实际指向哪一种参数结构体
 * 每个 para 结构体的第一个成员都是公共头 ui_head_t，头里存着 type 和绘制函数指针，
 * 所以拿到 void* 后可以直接当 ui_head_t* 用：先按 type 确认是哪一类，再调 draw 画出自己
 * UI_TYPE_NONE = 0 是"没赋过值"的默认值：漏填 type 时不会静默变成 UI_TYPE_CIRCLE，而是能在派发时被认出来 */
typedef enum
{
  UI_TYPE_NONE = 0,
  UI_TYPE_CIRCLE,
  UI_TYPE_RECT,
  UI_TYPE_TEXT,
  UI_TYPE_CURSOR,
  UI_TYPE_POLYLINE,
  UI_TYPE_UNION,
  UI_TYPE_POINT,
  UI_TYPE_VIEWPORT,
  UI_TYPE_PROGRESS_BAR,
  UI_TYPE_IMAGE,             /* 图片 / 图标：新类型只能往后加，各分派表才不用挪旧项 */
  UI_TYPE_MODULE             /* 组件类型槽：所有复合组件(滑块/滚动条/以后的新组件)共用这【一个】值，
                               * 以后加组件不再动这个枚举。组件约定(详见 ui_module.h)：
                               *   para 首成员 = ui_rect_para_t(组件外接框，框架按矩形口径量框/命中/标脏)；
                               *   绘制走 head.draw 自带回调；零件对象不进链，由组件带画 */
}ui_type_t;

/* 统一的绘制回调签名：只收 void*，各绘制函数在入口处再转成自己的参数类型。
 * 这样四种 para 的绘制函数就是同一个函数指针类型，能挂进同一个公共头里 */
typedef void (*ui_draw_fn_t)(void* para);



/* 每个 para 结构体的公共头，必须放在第一个成员：
 * C 保证成员按声明顺序布局，四种 para 里 head 的偏移都是 0，
 * 所以拿到 void*（ui_object_t::para 存的就是它）后可以直接当 ui_head_t* 用，
 * 读出 type 校验、再调 draw 画出自己 —— 参数结构体就自带了自己的绘制函数指针 */
typedef struct
{
  ui_type_t    type;   /* 本 para 是哪一类；UI_TYPE_NONE 表示没赋过值 */
  ui_draw_fn_t draw;   /* 本类对应的绘制函数，构造时填 ui_draw_circle / ui_draw_rect / ... */
  int16_t      center_x; /* 几何中心。四类 para 都要它，提到公共头里，
                          * 拿到 void* 就能按 head 统一读改写，不用逐类型强转 */
  int16_t      center_y;
}ui_head_t;

/* 变换块：确定态基准 + 缩放因子 + 缩放锚点。
 * 放在下面几个"单体"para（矩形 / 圆 / 文本框）的 head 正后面，偏移统一 = sizeof(ui_head_t)，
 * 于是拿到 head 就能按固定偏移取到它，统一做"设锚点 / bake"。
 *
 * 语义（head.center_x/y + w/h 仍是虚拟值，绘制 / 脏区 / 光标照读）：
 *   虚拟中心 = anchor + anchor_off × factor
 *   虚拟尺寸 = 基准尺寸 × factor
 *   确定态 = base_cx/cy（基准中心）+ 基准尺寸 + base_anchor（基准锚点）：复位读回，
 *            bake / set_anchor 时重备份
 * anchor 只随平移走；factor 只随缩放走；两者都不动 base。
 * anchor_off = 锚点 → 基准中心（派生量）：设锚点 / bake 时算，形变改了当前中心时跟着同步；
 * 缩放 / 平移都不改它。
 * 默认（set_anchor 传中心）anchor_off = 0，缩放绕中心，与旧行为一致 */
typedef struct
{
  int16_t  base_cx, base_cy;             /* 基准中心（确定态） */
  int16_t  base_anchor_x, base_anchor_y; /* 基准锚点（确定态）：复位时锚点也一起回退，
                                            再由"基准中心 - 基准锚点"把偏移重算回来 ——
                                            形变只改当前锚点 / 偏移，所以复位照样回到形变前 */
  int16_t  anchor_x, anchor_y;           /* 当前缩放锚点（绝对）：缩放不动它，平移带着它走 */
  int16_t  anchor_off_x, anchor_off_y;   /* 锚点 → 基准中心 的偏移（派生量）：设锚点 / bake 时算，
                                            形变改了当前中心时跟着同步；缩放 / 平移都不改它 */
  uint16_t scale_num, scale_den;         /* 缩放因子：虚拟尺寸 = 基准尺寸 × num/den */
  uint8_t  anchor_set;                   /* 1 = 用 ui_obj_set_anchor 设过锚点：平移 / 缩放 / 形变都绕它；
                                            0 = 没设过（默认）：变换一律按"中心不动"处理。
                                            只由 ui_obj_set_anchor 置位，bake / 复位都不清 */
}ui_xform_t;


/* 由"中心 + 宽高"反推四边（含端点）。全工程只留这一份：
 *   x_min = cx - w/2、x_max = x_min + w - 1（与 ui_draw_rect 同一套约定，逐像素对齐）。
 * 矩形 / 点 / 光标 / 折线 / 合并对象 / 视窗的对象框、脏区、绘制裁剪都走它。
 * 文字 / 圆的整块标脏用"中心 ± w/2"这套对称口径，各自保留在原处，不走这里 */
void ui_box_of(int16_t cx, int16_t cy, int16_t w, int16_t h,
               int16_t* x_min, int16_t* y_min, int16_t* x_max, int16_t* y_max);


/* 各类 UI 各自的参数，参数体大小可以完全不同。
 * 首成员固定是 ui_head_t，后面才是本类自己的字段 */
typedef struct
{
  ui_head_t  head;
  ui_xform_t xf;                 /* 变换块：紧跟 head，偏移固定 */
  int16_t    base_ext_r;         /* 基准外半径（确定态；复位读回 / bake 重备份） */
  int16_t    base_int_r;         /* 基准内半径 */
  int16_t    internal_r;
  int16_t    external_r;
  ui_color_t   color;
  uint8_t    alpha;              /* 半透明：0 = 不透明(默认/旧行为)，1~254 = 与下层混合，255 = 不透明 */
}ui_circle_para_t;

/* 点：一个实心小方块，边长 = size 像素，画在中心处。
 * size = 1 就是单个像素点，每 +1 就是再往外包一层像素。
 * 和矩形 / 圆一样是"单体"：变换块 xf 管缩放与平移，行为完全一致。
 * 边界约定跟矩形一致：x_min = 中心 - size/2、x_max = x_min + size - 1 */
typedef struct
{
  ui_head_t  head;
  ui_xform_t xf;                 /* 变换块：紧跟 head，偏移固定 */
  int16_t    base_size;          /* 基准边长（确定态；复位读回 / bake 重备份） */
  int16_t    size;               /* 当前边长（像素，>= 1） */
  ui_color_t   color;
  uint8_t    alpha;              /* 半透明：0 = 不透明(默认/旧行为)，1~254 = 与下层混合，255 = 不透明 */
}ui_point_para_t;

/* 取图片一行里一段像素的回调：ui_draw_image 只认这个接口取"片外"图片的数据 ——
 * 图标在片外 NAND / SPI Flash 时没法直接给常量指针，只能按行读到 RAM
 *（和字库的 ui_hz_glyph_fn_t 同一个思路）。
 *   参数 img_id   原样透传 ui_image_para_t::img_id，含义由回调自己定
 *        img_y    要取图片的第几行（0 = 最上一行）
 *        col0     要取的起始列（含）；col1 结束列（含）
 *        buf      调用方给的缓冲，容量 >= (col1 - col0 + 1) 个 uint16_t
 *   返回 1 = 取到（buf 里按列顺序就是 RGB565 像素）；0 = 取不到（这一行不画）
 * 图片在片内 Flash 时不用回调：直接把点阵首地址填 ui_image_para_t::pixels */
typedef uint8_t (*ui_img_row_fn_t)(uint8_t img_id, int16_t img_y,
                                   int16_t col0, int16_t col1, ui_color_t *buf);

/* 图片 / 图标：一块"规格 w×h"的 RGB565 裸像素（行优先，每行 w 个像素，如 64×64）。
 * 和矩形 / 圆 / 点一样是"单体"：变换块 xf 管平移与锚点 ——
 * 虚拟中心 = 锚点 + 偏移 × 因子（当前不做缩放，因子恒为 1；块留在这里，
 * 以后要加缩放只需往各分派表登记，结构体不用动）。
 * 规格由构造时给定：w / h 是当前值，base_w / base_h 是确定态（bake / 复位用）。
 * 像素来源二选一：
 *   pixels 非 NULL：片内点阵，直接按指针读；
 *   pixels 为 NULL：走 row_fn 按行读（片外图标用这套）
 * 透明：数据是 RGB565、没有 alpha 通道，所以"透明"用【键色】约定 —— key_on != 0 时，
 * 值等于 key 的像素不写进行缓冲，下层的卡片底/背景原样透出来（转 bin 时把透明像素
 * 写成这个键色即可，见 APP/ui_icon_port 和 Tools/png2rgb565.ps1）。
 * 边界约定跟矩形一致：x_min = 中心 - w/2、x_max = x_min + w - 1 */
typedef struct
{
  ui_head_t  head;
  ui_xform_t xf;                 /* 变换块：紧跟 head，偏移固定 */
  int16_t    base_w;             /* 基准宽高（确定态；复位读回 / bake 重备份） */
  int16_t    base_h;
  int16_t    w;                  /* 图片规格宽（像素）；当前不缩放，恒 = base_w */
  int16_t    h;                  /* 图片规格高（像素）；当前不缩放，恒 = base_h */
  const ui_color_t *pixels;        /* 片内点阵首地址（RGB565 行优先）；NULL = 走 row_fn */
  uint8_t    img_id;             /* 图片关键码：原样透传给 row_fn，含义由回调自己定 */
  ui_img_row_fn_t row_fn;        /* 片外取行回调；pixels 非 NULL 时不看它 */
  ui_color_t key;                /* 透明键色(RGB565)：值等于它的像素【跳过不写】，
                                  * 让下层(卡片底等)原样透出来 —— RGB565 没有 alpha 通道，
                                  * 图片的"透明"就是靠这个约定色实现的(key_on != 0 才生效) */
  uint8_t    key_on;             /* 1 = 启用键色透明；0 = 每个像素照画(旧行为，无透明) */
  uint8_t    alpha;              /* 半透明：0 = 不透明(默认/旧行为)，1~254 = 与下层混合，255 = 不透明。
                                  * 键色像素仍然直接跳过(不参与混合) */
}ui_image_para_t;

typedef struct
{
  ui_head_t  head;
  ui_xform_t xf;                 /* 变换块：紧跟 head，偏移固定 */
  int16_t    base_w;             /* 基准宽高（确定态；复位读回 / bake 重备份） */
  int16_t    base_h;
  int16_t    w;
  int16_t    h;
  int16_t    border_width;
  ui_color_t   color;
  uint8_t    fill;               /* 0 = 只描边（旧行为）；1 = 实心填充（忽略 border_width） */
  uint8_t    corners;            /* 要圆哪几个角(ui_tool.h 的 UI_ROUND_*)：0 = 不圆 = 整宽，旧行为 */
  uint8_t    radius;             /* 圆角半径(px)，corners != 0 时才用；内部会夹到 min(w,h)/2 */
  uint8_t    alpha;              /* 半透明：0 = 不透明(默认，旧代码行为)；1~254 = 与下层像素
                                  * 按 alpha/255 混合(透出下面的卡片 / 渐变)；255 = 不透明。
                                  * 实心 / 描边 / 圆角 / 渐变都生效 —— 见 ui_draw_rect 与
                                  * ui_tool 的圆角工具；混合走 ui_row_draw_span_blend */
}ui_rect_para_t;

/* 按 para->alpha 给 [x0, x1] 上一段色：0(未设) / 255 = 不透明(整段填色，旧行为)；
 * 1~254 = 与行缓冲里的下层像素混合(半透明)。color 由调用方给 ——
 * 普通矩形传 para->color，渐变 / 圆角工具传本行算出来的颜色。
 * ui_draw_rect / ui_tool 的圆角工具 / APP 里自写的渐变回调都走它，半透明语义才一致 */
void ui_rect_span(const ui_rect_para_t* para, int16_t x0, int16_t x1, ui_color_t color);

/* 各图元通用的"带 alpha 的一段上色"：alpha = 0(未设) / 255 = 不透明(整段填色)，
 * 1~254 = 与行缓冲里的下层像素混合。各 para 的 alpha 字段都是这套语义(0 保持旧行为)。
 * 需要在已经 ui_row_draw_select 选好行的前提下调用（和 ui_row_draw_span 同一位置） */
void ui_span_alpha(int16_t x0, int16_t x1, ui_color_t color, uint8_t alpha);


/* 文本在"可写范围"内的水平对齐；纵向始终居中。
 * CENTER = 0，保证没赋值的静态对象维持旧的"居中"行为 */
typedef enum
{
  UI_TEXT_ALIGN_CENTER = 0,
  UI_TEXT_ALIGN_LEFT,
  UI_TEXT_ALIGN_RIGHT,
}ui_text_align_t;

/* 文本整块在"可写范围"内的纵向对齐；CENTER = 0，保证没赋值的对象维持旧行为 */
typedef enum
{
  UI_TEXT_VALIGN_CENTER = 0,
  UI_TEXT_VALIGN_TOP,
  UI_TEXT_VALIGN_BOTTOM,
}ui_text_valign_t;


/* ---- 字形描述：一个字形长什么样 -------------------------------------
 * bitmap        点阵首地址；NULL = 这套字库里没这个字，调用方只按 advance_x 占位、不画
 * advance_x     画完这个字 x 往右挪多少(像素)，等于点阵宽
 * bytes_per_row 点阵里每行占几个字节 = ceil(点阵宽 / 8)
 * height        点阵高(占几个屏幕行)
 *
 * 点阵格式是【行优先、字节内低位在左】：第 0 行在最前，每行从 bit0 开始
 * 往右排(bit0 = 本字节最左那个像素)，和内部 Flash 那几套 ascii_* / tfont* 数组一致。
 * 片外字库文件大多是"高位在左"，取模回调得自己翻过来再交出来 —— 见
 * APP/ui_font_port/ui_font_port.c 的 bit_flip；不翻就是每个字节内部左右镜像，画出来全是碎块。
 *
 * bitmap 可以指向内部 Flash 的常量数组，也可以指向 RAM 缓冲 —— 片外
 * (SPI / NAND)字库没法直接给常量指针，只能先把字模读进 RAM，见 ui_hz_glyph_fn_t */
typedef struct
{
  const uint8_t *bitmap;
  uint8_t        advance_x;
  uint8_t        bytes_per_row;
  uint8_t        height;
  uint8_t        width;       /* 位图实际宽(像素)；1bpp 时 = advance_x */
  int8_t         bearing_x;   /* 位图左边相对笔位(像素)；1bpp 恒 0 */
  int8_t         bearing_y;   /* 位图顶相对"行顶"的偏移；1bpp 恒 0 */
  uint8_t        gray;        /* 1 = bitmap 是 8bit alpha(每行 width 字节)，
                               * 0 = 1bpp 行优先、字节内低位在左 */
}ui_glyph_t;

/* 一个字模最多占多少字节(1bpp 48×48 = 288 B；灰度 32×32 = 1024 B)：
 * ui_draw_text 按它准备取模缓冲，回调往缓冲里写的量不能超过这个数。数值在 ui_config.h */

/* 取模回调：ui_draw_text 只认这个接口，自己不认识任何一种字库，
 * 于是"字模在内部 Flash、片外 NAND 还是 SPI Flash"对绘制函数是透明的。
 *   参数 font_id   原样透传 ui_text_para_t::hz_font_id，含义由回调自己定
 *        code      指向 GBK 两字节(高字节在前)；ASCII 时是那一个字符码
 *        font_size 原样透传 ui_text_para_t::font_size，回调可用来校验字库
 *        buf       调用方给的 RAM 缓冲，容量 UI_GLYPH_MAX_BYTES
 *   返回 1 = 取到(填好 glyph)；0 = 取不到，调用方只按 advance_x 占位
 * 现成的实现见 ui_font.h 的 ui_font_glyph(字库库：外面挂什么读函数就取哪儿的点阵) */
typedef uint8_t (*ui_hz_glyph_fn_t)(uint8_t font_id, const uint8_t *code,
                                    uint16_t font_size, uint8_t *buf,
                                    ui_glyph_t *glyph);

typedef struct
{
  ui_head_t  head;
  ui_xform_t xf;                 /* 变换块：紧跟 head，偏移固定 */
  int16_t    base_w;             /* 基准宽高（确定态） */
  int16_t    base_h;
  int16_t    w;                  /* 文本可写范围宽（排版区域，跟文字实际长度无关，由调用方给定） */
  int16_t    h;                  /* 文本可写范围高；文字在范围内水平居中 + 按 center_y 纵向居中，
                                  * 超出范围的笔画裁掉。缩放这个对象时字号不缩（字库只有四档），只缩这个框 */
  const uint8_t *text;           /* GBK 字节串首地址；绘制侧【只读】，所以是 const ——
                                  * 改文字 = 换个指针(见 ui_task.h 的 ui_table_set_* )。
                                  * 只存指针不拷贝：它指向的内容要一直有效 */
  uint16_t font_size;
  uint16_t text_len;
  ui_color_t color;
  ui_text_align_t align;         /* 水平对齐：居中(默认) / 左 / 右 */
  uint8_t line_spacing;          /* 行距：在 font_size 之上额外加的像素；0 = 紧贴 */
  ui_text_valign_t valign;       /* 纵向对齐：居中(默认) / 顶 / 底 */
  uint8_t          hz_font_id;   /* 字体关键码：原样透传给 hz_glyph_fn，含义由回调自己定
                                  * (见 APP/ui_font_port/ui_font_port.h 的 ui_font_id_t；
                                  *  ASCII 不看它) */
  ui_hz_glyph_fn_t hz_glyph_fn;  /* 取模回调；NULL = 走内部 Flash 的 tfontXX / ascii_*
                                  * 小字库(只有 12/16/24/32 四档、每档几个字，仅够兜底)。
                                  * 填 ui_font_glyph = 走外部字库(外面挂的读点阵函数) */
  uint8_t          alpha;        /* 半透明：0 = 不透明(默认/旧行为)，1~254 = 与下层混合，255 = 不透明。
                                  * 灰度字模自带的 alpha 会和它相乘(字形边缘的过渡保留) */
}ui_text_para_t;

/* 光标方框套在被框图案外面，每边相对图案外扩的量 = gap + border_width：
 * gap 让边框内缘离图案一段距离，线宽让外框再整体涨出线宽，
 * 所以线宽加粗时方框是往外长，不会盖住被框的图案。
 * 这个外扩量在 ui_cursor_target_frame / ui_cursor_fit 里各算一次：
 * 前者算完只报出来（给跟随动画当目标框），后者算完写进光标自己的 center / w / h，
 * 之后绘制和脏区记账都直接读那几个字段，不用再回头找被框对象 */
typedef struct
{
  ui_head_t head;
  ui_object_t* obj;              /* 被框住的那个对象；不需要跟随目标就填 NULL */
                                 /* 方框自身的中心在 head.center_x / head.center_y，宽高在下面的 w / h。
                                  * 跟着目标走的用法是 ui_cursor_fit 按 obj 算好写进来；
                                  * 想自己指定位置宽高就直接填 head.center_x / head.center_y / w / h，
                                  * 四者必须成套 */
  int16_t w;
  int16_t h;
  int16_t gap;                   /* 边框内缘离被框图案的间距，常规填 4。
                                  * 每个光标各存一份，所以能按实例调 —— 做"选中时框往外涨一圈"
                                  * 这类波纹动画，逐帧改它再 ui_cursor_fit 一次就行 */
  uint16_t border_width;         /* 光标框的线宽 */
  ui_color_t color;
  uint8_t  alpha;                /* 半透明：0 = 不透明(默认/旧行为)，1~254 = 与下层混合，255 = 不透明 */
}ui_cursor_para_t;

/* 一个平面坐标点 */
typedef struct
{
  int16_t x;
  int16_t y;
}ui_point_t;

/* 折线的顶点节点：存一个顶点坐标，并用通用链表的 list_head_t 挂进折线的顶点链。
 * 节点由调用方自己定义（静态数组 / 静态变量都行），ui_create 只负责挂链、不分配内存，
 * 所以顶点数不受限 —— 调用方有多少节点就能挂多少段。
 * 首个顶点是"起始点"，单独存在 ui_polyline_para_t::start 里，不进这条链；
 * 链上每个节点都是一条线段的终点、同时也是下一条线段的起点。
 *
 * point 存的是**相对锚点的偏移**（不是屏幕绝对坐标）：
 *   虚拟顶点 = 锚点 + 偏移 × 缩放因子。
 * 调用方 append 时按绝对坐标填 point，append 内部减去锚点转成偏移再存储 */
typedef struct
{
  ui_point_t  point;  /* 这个顶点相对锚点的偏移 */
  list_head_t node;   /* 挂到 ui_polyline_para_t::list 上的节点 */
}ui_poly_node_t;

/* 折线：起始点 + 顶点链表，相邻两点连一条线，整条当一个对象。
 *
 * 几何一律存成"相对锚点的偏移"：start 和链上每个顶点的偏移都不随缩放 / 平移改动，
 *   虚拟顶点 = 锚点 + 偏移 × 因子（scale_num / scale_den）。
 * 缩放只写因子、锚点一个字都不动（反复写同一比例幂等、不累积取整误差）；
 * 平移只挪锚点、偏移不动；于是"先平移再缩放"结果也一致。
 * anchor 是当前锚点（绝对坐标），只随平移走；base_anchor 是上一次确定态的锚点，
 * 供复位读回。对象框 / 裁剪 / 脏区都只看 head.center_x / head.center_y + w / h：
 * 中心 + 宽高按当前因子算好存进去（ui_polyline_update_frame 负责刷新） */
typedef struct
{
  ui_head_t  head;               /* head.center_x / head.center_y = 对象框中心（虚拟、当前） */
  ui_point_t start;              /* 起始点：相对锚点的偏移（第 0 个顶点） */
  list_head_t list;              /* 顶点链表头（哨兵）：上面挂 ui_poly_node_t 的 node；
                                  * 用前必须先 ListInit（或静态初始化时写成自指），空链自指 */
  uint16_t   count;              /* 链上顶点数（不含起始点），也就是线段数 */
  uint16_t   line_width;         /* 线宽（像素） */
  ui_color_t   color;
  int16_t    anchor_x;           /* 当前缩放锚点（绝对）：缩放不动它，平移带着它走 */
  int16_t    anchor_y;
  int16_t    base_anchor_x;      /* 基准锚点（上一次确定态；复位读回 / bake 重备份） */
  int16_t    base_anchor_y;
  uint16_t   scale_num;          /* 缩放因子分子：虚拟顶点 = 锚点 + 偏移 * scale_num / scale_den */
  uint16_t   scale_den;          /* 缩放因子分母，初值 1，不能为 0 */
  int16_t    w;                  /* 对象框宽（按当前因子算，已含线宽外扩的一半） */
  int16_t    h;                  /* 对象框高 */
  uint8_t    alpha;              /* 半透明：0 = 不透明(默认/旧行为)，1~254 = 与下层混合，255 = 不透明 */
}ui_polyline_para_t;


/* 合并对象的一个子对象节点：把 obj 挂进合并对象的子对象链。
 * 节点由调用方提供（静态变量 / 静态数组都行），ui_create 只挂链、不分配内存。
 *
 * 尺寸基准不在这里 —— 用子对象自己自带的基准（单体是 ui_xform_t 的 base_*，
 * 折线是它自己的偏移）：append 时会先把每个子对象 bake 一次，让自带基准 = 成组那一刻的几何；
 * 之后整体缩放从子对象自带基准现算，比例是绝对值，逐帧反复设也不累积取整误差。
 * 这里只留 union 专属的"位置基准"：子对象中心相对 union 锚点的偏移。
 * 光标不做 union 子对象（它没有自带基准）。 */
typedef struct
{
  ui_object_t* obj;      /* 子对象；不单独进绘制链表，由所属合并对象统一绘制 / 驱动 */
  list_head_t  node;     /* 挂到 ui_union_para_t::list 上的节点 */

  /* 位置基准：子对象基准中心相对合并对象锚点（= 对象框中心）的偏移。
   * 平移只挪锚点、不动这个偏移，所以"先平移再缩放"结果也一致 */
  int16_t  base_rel_x;
  int16_t  base_rel_y;
}ui_merge_obj_node_t;

/* 合并对象（union）：把若干子对象当成一个对象使。
 * 子对象不单独进绘制链表，由 ui_draw_union 统一画；平移 / 缩放整体作用于所有子对象。
 * 框中心 head.center 是"所有子对象外接框并集的中心"，每次都从子对象重新量（真实框），
 * 脏区和光标都读它 —— 所以脏区和画出来的像素严格对齐。
 * 缩放锚点单独存 anchor_x / anchor_y：它只随整体平移走、缩放对它零影响。
 * 子对象的基准偏移就是相对它记的，于是缩放不会因为框中心重新量出来的 ±1px 取整误差而漂。
 * 线宽 / 边框宽 / 字号不跟着缩（各类型自己的缩放只管几何尺寸）。
 * 用法：先 ui_union_append 把所有子对象挂上，之后用 ui_anim_move_obj / ui_anim_scale_obj 整体驱动
 * （合并对象的平移 / 缩放分支实现都在 ui_anim.c 里） */
typedef struct
{
  ui_head_t  head;               /* head.center_x / head.center_y = 对象框中心（子对象框并集的中心） */
  list_head_t list;              /* 子对象链头（哨兵）：上面挂 ui_merge_obj_node_t 的 node；
                                  * 用前必须先 ListInit（或静态初始化时写成自指），空链自指 */
  int16_t    w;                  /* 对象框宽：所有子对象外接框并集的宽（当前几何下现算） */
  int16_t    h;                  /* 对象框高 */
  int16_t    anchor_x;           /* 缩放锚点：只随整体平移走，缩放一个字都不动它（见上） */
  int16_t    anchor_y;
  int16_t    base_anchor_x;      /* 基准锚点（确定态；复位读回 / bake 重备份） */
  int16_t    base_anchor_y;
}ui_union_para_t;



/* 视窗（viewport）：一个"只裁剪、不绘制"的容器对象。
 * 几何（中心 + 宽高）就是它露出的那块窗口；子对象画在窗口内，超出窗口的部分被裁掉。
 * 它自己不产生任何像素，但和别的对象一样支持设锚点 / 缩放 / 平移 / 复位：
 * 平移、缩放会把窗口连同里面的子对象一起带走（容器语义，和合并对象一致）。
 * 子对象必须先 ui_viewport_append 挂进来才算"属于这个视窗"：
 * 挂进来的子对象不单独进绘制链表，由 ui_draw_viewport 统一带裁剪地派发 */
typedef struct
{
  ui_head_t   head;               /* head.center_x / head.center_y = 窗口中心 */
  list_head_t list;               /* 子对象链头（哨兵）：上面挂 ui_merge_obj_node_t 的 node；
                                   * 用前必须先 ListInit（或静态初始化时写成自指），空链自指 */
  int16_t     w;                  /* 窗口宽（当前几何） */
  int16_t     h;                  /* 窗口高 */
  int16_t     base_w;             /* 基准窗口宽高（确定态；复位读回 / bake 重备份） */
  int16_t     base_h;
  int16_t     base_cx;            /* 基准窗口中心（确定态） */
  int16_t     base_cy;
  int16_t     anchor_x;           /* 缩放锚点：只随平移走，缩放一个字都不动它 */
  int16_t     anchor_y;
  int16_t     base_anchor_x;      /* 基准锚点（确定态；复位读回 / bake 重备份） */
  int16_t     base_anchor_y;
}ui_viewport_para_t;


/* 进度条形状：两个进度条共用 UI_TYPE_PROGRESS_BAR，分派处理(平移 / 缩放 / 复位 / 进度动画)
 * 靠 para 里的 shape 判别位区分是按圆环几何还是矩形几何走 */
typedef enum
{
  UI_PROGRESS_SHAPE_CIRCLE = 0,   /* 圆环进度条 */
  UI_PROGRESS_SHAPE_RECT,         /* 矩形进度条 */
} ui_progress_shape_t;

/* 两个进度条 para 的公共前缀：都是 head + xf + shape 打头、偏移一致，
 * 于是分派处理只看这一段就能拿到公共字段，再按 shape 分开读各自的几何量 */
typedef struct
{
  ui_head_t  head;
  ui_xform_t xf;
  uint8_t    shape;
} ui_progress_common_t;

/* 圆环进度条：背景环 + 前景进度环共心叠画，前景按进度铺开。
 * 和圆一样是"单体"：变换块 xf 管缩放与平移 —— 虚拟中心 = 锚点 + 偏移 × 因子、
 * 虚拟半径 = 基准半径 × 因子；缩放绕锚点进行、平移只挪锚点。
 * 布局与 ui_circle_para_t 对齐：中心在 head，几何量拆"当前值 + base_ 基准值"，
 * 基准值供复位读回 / bake 重备份。线宽不随缩放改变 */
typedef struct
{
    ui_head_t   head;            /* head.center_x / head.center_y = 圆心（虚拟、当前） */
    ui_xform_t  xf;              /* 变换块：紧跟 head，偏移固定（缩放 / 平移都走它） */
    uint8_t     shape;           /* 本对象是圆环还是矩形进度条(ui_progress_shape_t)，分派用 */

    /* 基础几何参数（半径） */
    int16_t base_ext_r;          /* 基准外半径（确定态；复位读回 / bake 重备份） */
    int16_t base_int_r;          /* 基准内半径；外-内=环宽度 为0时，环为实心圆 */
    int16_t external_r;          /* 当前外半径 = 基准外半径 × 缩放因子 */
    int16_t internal_r;          /* 当前内半径 = 基准内半径 × 缩放因子 */

    /* 角度与进度 */
    int16_t angle_start;     // 起始角度(默认0在右侧，270=顶部起始)
    int16_t angle_end_bg;    // 背景环总结束角度，一般360整圈
    uint16_t progress;       // 当前进度值(0~max_value)
    uint16_t max_value;      // 进度最大值，如100

    /* 样式颜色 */
    ui_color_t color_bg;       // 背景圆环颜色(RGB565)
    ui_color_t color_fore;     // 前景进度圆弧颜色(RGB565)
    uint16_t line_width;     // 圆弧线条宽度(环粗细)，不随缩放改变

    /* 端点样式 */
    uint8_t rounded_end;        // true=两端为圆弧，false=平头

    uint8_t alpha;              /* 半透明：0 = 不透明(默认/旧行为)，1~254 = 与下层混合，255 = 不透明 */

}ui_circle_progress_bar_para_t;


/* 矩形进度条的前景增长方向：RIGHT = 0，保证没赋值的静态对象维持"从左往右"的旧行为 */
typedef enum
{
    UI_PROGRESS_DIR_RIGHT = 0,
    UI_PROGRESS_DIR_LEFT,
    UI_PROGRESS_DIR_UP,
    UI_PROGRESS_DIR_DOWN,
}ui_progress_dir_t;

/* 矩形进度条：底条 + 前景进度条，前景按方向从一端铺开。
 * 和矩形一样是"单体"：变换块 xf 管缩放与平移 —— 虚拟中心 = 锚点 + 偏移 × 因子、
 * 虚拟宽高 = 基准宽高 × 因子；缩放绕锚点进行、平移只挪锚点。
 * 布局与 ui_rect_para_t 对齐：中心在 head，宽高拆"当前值 + base_ 基准值"，
 * 基准值供复位读回 / bake 重备份。边框线宽不随缩放改变 */
typedef struct
{
    ui_head_t   head;            /* head.center_x / head.center_y = 条中心（虚拟、当前） */
    ui_xform_t  xf;              /* 变换块：紧跟 head，偏移固定（缩放 / 平移都走它） */
    uint8_t     shape;           /* 本对象是圆环还是矩形进度条(ui_progress_shape_t)，分派用 */

    int16_t base_w;              /* 基准宽高（确定态；复位读回 / bake 重备份） */
    int16_t base_h;
    int16_t w;                   /* 当前宽高 = 基准宽高 × 缩放因子 */
    int16_t h;

    /* 进度 */
    uint16_t progress;       // 当前进度值(0~max_value)
    uint16_t max_value;      // 进度最大值，如100

    /* 样式 */
    ui_color_t color_bg;       // 底条颜色
    ui_color_t color_fore;     // 前景进度条颜色
    int16_t border_width;    // 外框线宽，0=无边框；不随缩放改变
    uint8_t dir;             // 前景增长方向(ui_progress_dir_t)

    uint8_t rounded_end;        // true=两端为圆弧，false=平头

    uint8_t alpha;              /* 半透明：0 = 不透明(默认/旧行为)，1~254 = 与下层混合，255 = 不透明 */

}ui_rect_progress_bar_para_t;



/* 三个绘制函数的入参都是 void*，进来第一件事转成自己的 para 类型。
 * 调用前要保证 para 的首成员 head 已经挂好：head.type 填本类、head.draw 填本函数 */
/* 把圆环一次画满当前这一整段缓冲，
 * 笔画范围是内半径 internal_r 到外半径 external_r 之间 */
void ui_draw_circle(void* para);

/* 把点（实心小方块，边长 size）一次画满当前这一整段缓冲 */
void ui_draw_point(void* para);

/* 把图片/图标（规格 w×h 的 RGB565 裸像素）一次画满当前这一整段缓冲。
 * 只支持平移：虚拟中心 = 锚点 + 偏移 × 因子（和矩形同一套，因子恒为 1）；
 * 出屏 / 出视窗 / 出脏条的部分由 ui_tool 的按行搬运裁掉。
 * 像素来源见 ui_image_para_t：pixels 直读，或 row_fn 按行读（片外图标） */
void ui_draw_image(void* para);

/* 把矩形一次画满当前这一整段缓冲 */
void ui_draw_rect(void* para);

/* 把文字一次画满当前这一整段缓冲
 * text 是 GBK 字节串，text_len 是字节数：ASCII 一个字节，汉字两个字节
 * font_size 是字号，也是行高基准；汉字走 hz_glyph_fn 取模：填了回调就按回调去取
 * (代码里不含任何一种字库，见 ui_hz_glyph_fn_t)，没填则退回内部 Flash 的 tfontXX
 * 小字库(只认 12/16/24/32 四档)；字库里没收这个字就不画、只占位
 * w/h 是"文本可写范围"（排版区域），由调用方给定、跟文字实际长度无关：
 * 文字在范围内水平居中、按 center_y 纵向居中，超出范围的笔画裁掉；
 * 缩放这个对象时字号不缩（点阵字号是固定的），只缩这个范围框。
 * 底色由 ui_row_buf_clear 铺，这里只写前景色 */
void ui_draw_text(void* para);

/* 量整串文字排出来的实际宽度（像素）：ASCII 按 font_size/2 估算，汉字前进 font_size，
 * 和 ui_draw_text 的排版估算同一套规则（'\n' 只断行不占宽，也不算）。供调用者做布局 /
 * 左对齐 / 右对齐、算可写范围。注意它只量【一行】的宽度，串里有 '\n' 时要自己按段切。
 * 灰度 ASCII 字库(如美好体)是比例字体，绘制时笔位真正按字库给的 advance 走，
 * 这里只是没有字库上下文时的估算值（用于居中/折行，误差几个像素） */
int16_t ui_text_advance(const uint8_t *str, uint16_t len, uint16_t font_size);

/* 折线绘制回调：按段（起始点→各顶点依次相连）画。
 * 每段按"点到线段的距离落在半线宽以内"判定，于是拐角自动是圆的、接缝不缺口；
 * 线宽 1 只画中轴那一像素，加粗往外长 */
void ui_draw_polyline(void* para);

/* ---- 折线几何工具（绘制与 ui_control 共用）----
 * 由"相对锚点的偏移"算虚拟屏幕坐标 / 由对象框 中心+宽高 反推四边。
 * 实现留在本模块（ui_draw_polyline 也要用）。
 * 折线的"操作类"接口（update_frame / append / delete_last / set_anchor / bake / restore）
 * 都在 ui_control.h 里 */
int16_t ui_poly_virtual(int16_t off, int16_t anchor, uint16_t num, uint16_t den);
void    ui_poly_rect_of(const ui_polyline_para_t* polyline,
                        int16_t* x_min, int16_t* y_min, int16_t* x_max, int16_t* y_max);

/* 合并对象绘制回调：遍历子对象链，逐个 ui_draw_dispatch 画出来（子对象自带绘制函数）。
 * 子对象不单独注册进绘制链表，整组就靠这一个回调画 */
void ui_draw_union(void* para);

/* 视窗绘制回调：先把自己的窗口矩形压成裁剪区，再遍历子对象链逐个派发，最后还原裁剪。
 * 视窗自己不画任何像素 —— 它存在的意义就是"把子对象裁在窗口里" */
void ui_draw_viewport(void* para);

/* 合并对象的"操作类"接口（update_frame / append）在 ui_control.h；
 * 光标的"操作类"接口（target_frame / fit）也在 ui_control.h */

/* 光标绘制回调：只照光标自己的 center_x / center_y / w / h 画一个矩形框，
 * 跟 obj 无关 —— 框是谁定的由调用者决定（ui_cursor_fit，或者自己填字段） */
void ui_draw_cursor(void* para);

/* 圆环进度条绘制回调：背景环 + 前景进度环，都从 angle_start 起顺时针铺开。
 * 环粗细取 internal_r / external_r（line_width 不参与几何）；rounded_end 非 0 时
 * 前景弧两端补半圆帽。圆环 / 矩形进度条共用 UI_TYPE_PROGRESS_BAR，靠 head.draw 区分，
 * 构造时圆环对象把 head.draw 填本函数 */
void ui_draw_circle_progress_bar(void* para);

/* 矩形进度条绘制回调：底条(color_bg) + 前景进度(color_fore)，按 dir 从一端铺开；
 * border_width 是进度区离条边的留边（即外框厚度）。构造时矩形对象把 head.draw 填本函数 */
void ui_draw_rect_progress_bar(void* para);

/* 进度值变化(动画)对应的脏区：只标"新旧进度之间真正变了的那一段"，而不是整个外框 ——
 * 圆环 = 旧角度到新角度之间那段环扇区的外接框；矩形 = 前景末端两端之间的内区条带。
 * 值没变就直接返回(一个像素都没动)；几何退化时退回整框。只多推、绝不漏。
 * 给 ui_anim 的进度动画用：每一小步只推变了的那一小段，省掉整条/整环的重画 */
void ui_progress_mark_value_change(const ui_progress_common_t* p,
                                   uint16_t old_value, uint16_t new_value);

/* 统一派发：para 指向任意一种 para 结构体（首成员都是 ui_head_t）。
 * 按 head->type 确认赋过值、再调 head->draw；
 * 漏填 type 或 draw 就什么都不画，不会误当成某一种形状静默画出来 */
void ui_draw_dispatch(void* para);

#endif

