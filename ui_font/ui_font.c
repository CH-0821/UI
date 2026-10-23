#include "./ui_font/ui_font.h"

/* ==================================================================
 *  字库库：把外部的读点阵函数包上一层逐字缓存
 *
 *  为什么要有这一层：文字是【反复重画】的。一条正在动画的文字每帧都要重新
 *  标脏、重新排版，同一个字一遍又一遍地来取模 —— 而取一次就是一次介质读
 *  (NAND 是一个字节一次 AHB 访问 + 软件轮询 R/B)。有了这层缓存，稳态下
 *  重复的字一个字节的介质都不用读。
 *
 *  库这里不认识任何一个字库，只有 s_reader 一个函数指针。
 * ================================================================== */

/* 当前挂着的读点阵函数；NULL = 还没挂 */
static ui_hz_glyph_fn_t s_reader = NULL;

/* ------------------------------------------------------------------
 *  字模缓存
 *
 *  直接映射(不做 LRU)：命中就命中，不命中就顶掉本槽那一条。界面上真正用到
 *  的字通常就几十个，给 32 个槽够用(槽数 UI_FONT_CACHE_NUM 在 ui_config.h)；
 *  真撞了同槽也只是退化成"每次都去读介质"，不会出错 —— 每条都带着自己的 key，
 *  顶掉的一定是另一个字。
 *
 *  槽里的点阵【已经是读函数交出来的样子】(低位在左)，命中时省掉翻转那一步。
 *  key 要带全 (font_id, code, font_size)：ASCII 和汉字的 code 字节数不同，
 *  同一档字号还有可能配不同的表，少一项就会串字。
 * ------------------------------------------------------------------ */
#define UI_FONT_CACHE_EMPTY    0u    /* used 字段的"这条空着"标记 */

typedef struct
{
    uint8_t used;                        /* 0=空 / 1=有效 */
    uint8_t font_id;
    uint8_t c0;                          /* code[0] */
    uint8_t c1;                          /* code[1](ASCII 时恒为 0) */
    uint8_t size;                        /* 取进来时的字号 */
    uint8_t w;                           /* 位图宽(像素) */
    uint8_t adv;                         /* 笔位步进(灰度汉字 = 字号，其余 = 位图宽) */
    uint8_t bpr;                         /* 每行字节数 */
    uint8_t h;                           /* 字高 */
    int8_t  bx;                          /* 位图左偏移(灰度字用) */
    int8_t  by;                          /* 位图顶偏移(灰度字用) */
    uint8_t gray;                        /* 1 = 8bit alpha */
    uint8_t data[UI_GLYPH_MAX_BYTES];    /* 点阵(行优先；1bpp 低位在左 / 灰度 8bit) */
}ui_font_cache_t;

static ui_font_cache_t s_cache[UI_FONT_CACHE_NUM];

/* 缓存槽下标：把四个 key 混成一个 5 位的数。只是个散列，怎么混都行 ——
 * 只要同一个 key 每次都落到同一个槽、不同的字尽量别挤在一起 */
static uint8_t cache_slot(uint8_t font_id, uint8_t c0, uint8_t c1, uint8_t size)
{
    return (uint8_t)(((uint16_t)font_id * 97u + (uint16_t)c0 * 59u
                      + (uint16_t)c1 * 13u + (uint16_t)size)
                     & (uint16_t)(UI_FONT_CACHE_NUM - 1u));
}

/* 把"没取到"时的占位口径摆好：调用方仍要按字宽把笔往右挪 */
static void glyph_placeholder(ui_glyph_t *glyph, uint16_t font_size)
{
    glyph->bitmap        = NULL;
    glyph->advance_x     = (uint8_t)font_size;
    glyph->bytes_per_row = (uint8_t)((font_size + 7u) >> 3);
    glyph->height        = (uint8_t)font_size;
    glyph->width         = (uint8_t)font_size;
    glyph->bearing_x     = 0;
    glyph->bearing_y     = 0;
    glyph->gray          = 0u;
}


void ui_font_set_reader(ui_hz_glyph_fn_t fn)
{
    uint8_t i;

    s_reader = fn;

    /* 来源换了，缓存里存着的旧点阵就过期了 —— 全部清掉 */
    for (i = 0u; i < (uint8_t)UI_FONT_CACHE_NUM; i++)
    {
        s_cache[i].used = UI_FONT_CACHE_EMPTY;
    }
}

uint8_t ui_font_glyph(uint8_t font_id, const uint8_t *code,
                      uint16_t font_size, uint8_t *buf,
                      ui_glyph_t *glyph)
{
    ui_font_cache_t *e;
    ui_glyph_t       g;
    uint16_t         bytes;
    uint16_t         i;
    uint8_t          c1;

    if (s_reader == NULL || code == NULL || buf == NULL || glyph == NULL)
    {
        return 0u;
    }

    /* ASCII 只占一个字节，code[1] 是后面那个字符 —— 不能拿它当 key 的一部分 */
    c1 = (code[0] < 0x80u) ? 0u : code[1];

    glyph_placeholder(glyph, font_size);

    e = &s_cache[cache_slot(font_id, code[0], c1, (uint8_t)font_size)];

    if (e->used == UI_FONT_CACHE_EMPTY ||
        e->font_id != font_id || e->c0 != code[0] || e->c1 != c1 ||
        e->size != (uint8_t)font_size)
    {
        /* 没命中：让外面的读函数把点阵直接读进缓存槽(省一次搬运)。
         * 先把这一槽失效掉 —— 读失败时槽里可能是半截数据，留着会被后面
         * 同 key 的查询当成好数据 */
        e->used = UI_FONT_CACHE_EMPTY;

        g.bitmap        = NULL;
        g.advance_x     = (uint8_t)font_size;
        g.bytes_per_row = (uint8_t)((font_size + 7u) >> 3);
        g.height        = (uint8_t)font_size;
        g.width         = (uint8_t)font_size;
        g.bearing_x     = 0;
        g.bearing_y     = 0;
        g.gray          = 0u;

        if (s_reader(font_id, code, font_size, e->data, &g) == 0u || g.bitmap == NULL)
        {
            return 0u;                   /* 取不到：上层只按 advance_x 占位 */
        }

        e->font_id = font_id;
        e->c0      = code[0];
        e->c1      = c1;
        e->size    = (uint8_t)font_size;
        e->w       = g.width;
        e->adv     = g.advance_x;
        e->bpr     = g.bytes_per_row;
        e->h       = g.height;
        e->bx      = g.bearing_x;
        e->by      = g.bearing_y;
        e->gray    = g.gray;
        e->used    = 1u;
    }

    /* 交给调用方：命中时这一步就是 RAM->RAM 的一次拷贝，
     * 比再读一次介质便宜几个数量级 */
    bytes = (uint16_t)e->bpr * (uint16_t)e->h;
    if (bytes > (uint16_t)UI_GLYPH_MAX_BYTES)
    {
        e->used = UI_FONT_CACHE_EMPTY;   /* 读函数报的尺寸不对，这一条不要了 */
        return 0u;
    }

    for (i = 0u; i < bytes; i++)
    {
        buf[i] = e->data[i];
    }

    glyph->bitmap        = buf;
    glyph->advance_x     = e->adv;
    glyph->bytes_per_row = e->bpr;
    glyph->height        = e->h;
    glyph->width         = e->w;
    glyph->bearing_x     = e->bx;
    glyph->bearing_y     = e->by;
    glyph->gray          = e->gray;
    return 1u;
}
