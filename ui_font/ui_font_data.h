#ifndef __UI_FONT_DATA_H__
#define __UI_FONT_DATA_H__

/* 内置字库(片内 Flash 里那几套小点阵)的【声明】。
 *
 * 点阵数据一个字节都不在这里 —— 它们全在 ui_font_data.c 里落地。头文件只给
 * 类型、长度宏和 extern 声明，这样谁 include 它都只拿到声明，不会凭空
 * 多生成 80KB 数据，也不会撞上重复定义。
 *
 * 用到它的地方：ui_create.c —— 没挂取模回调时，拿这几套小数组兜底。
 *
 * 要增删汉字：改 ui_font_data.c 里对应数组的内容，并把下面的 TFONTxx_NUM 一起改。 */
typedef struct
{
	unsigned char Index[2];
	unsigned char Msk[24];
}typFNT_GB12;

typedef struct
{
	unsigned char Index[2];
	unsigned char Msk[32];
}typFNT_GB16;

typedef struct
{
	unsigned char Index[2];
	unsigned char Msk[72];
}typFNT_GB24;

typedef struct
{
	unsigned char Index[2];
	unsigned char Msk[128];
}typFNT_GB32;

/* 汉字字库各数组里收了多少个字。
 * 数组在没落地的地方是不完整类型（extern const typFNT_GB12 tfont12[]），
 * sizeof 取不出长度，所以长度单独用宏给出来 —— ui_font_data.c 里增删汉字时这里跟着改。 */
#define TFONT12_NUM  5
#define TFONT16_NUM  5
#define TFONT24_NUM  5
#define TFONT32_NUM  5


/* 四套 ASCII + 四套汉字的声明；实体都在 ui_font_data.c */
extern const unsigned char ascii_1206[][12];
extern const unsigned char ascii_1608[][16];
extern const unsigned char ascii_2412[][48];
extern const unsigned char ascii_3216[][64];
extern const typFNT_GB12 tfont12[];
extern const typFNT_GB16 tfont16[];
extern const typFNT_GB24 tfont24[];
extern const typFNT_GB32 tfont32[];

#endif


