#ifndef __LINKED_LIST_H__
#define __LINKED_LIST_H__

#include <stdint.h>
#include <stddef.h>   /* NULL、offsetof */


/*=========================================================================*/  
/*  节点类型 list_head_t 直接嵌进业务结构体：一个业务结构体可以嵌多个节点， */
/*  就能同时挂在多条链表上（比如同一个业务对象既进操作链表、又进绘制优先级链表）*/
/*  环上没有 NULL：每条链表的头是一个哨兵节点，空链时哨兵的 prev/next 都指自己 */
/*=========================================================================*/
typedef struct list_head_t
{
  struct list_head_t* prev;   /* 前一个节点；哨兵的 prev 是链上的最后一个节点 */
  struct list_head_t* next;   /* 后一个节点；哨兵的 next 是链上的第一个节点 */
}list_head_t;

/* 静态初始化哨兵：自己指自己就是空链。只写 prev/next，别的字段由编译器补 0 */
#define LIST_HEAD_INIT(name)  { &(name), &(name) }
#define LIST_HEAD(name)       list_head_t name = LIST_HEAD_INIT(name)

/* 由链表节点反查宿主业务结构体首地址（container_of）
 * ptr 是节点成员地址，type 是宿主类型，member 是节点在宿主里的成员名。
 * 用普通表达式实现，不用 typeof / 语句表达式 —— ARMCC AC5 不支持 ({...}) 和 typeof。
 * 【注意】简单 cast 不做任何校验，用错会静默出错，调用时务必保证：
 *   1) ptr 确实是某个 type 对象里 member 这个成员的地址（传错结构体 / 传错成员 / 传对象本身
 *      而不是成员地址，都会算出野指针，编译期不报错）；
 *   2) ptr 的 const / volatile 限定会被静默抹掉，返回的是非 const 的 type * */
#define ListEntry(ptr, type, member) \
  ((type *)((char *)(ptr) - offsetof(type, member)))



/* 取链上第一个宿主结构体；空链时算出来的地址不可用，所以先配 ListEmpty 判空 */
#define ListFirstEntry(head, type, member) \
  ListEntry((head)->next, type, member)

/* 由当前宿主结构体取下一个宿主结构体 */
#define ListNextEntry(pos, type, member) \
  ListEntry((pos)->member.next, type, member)

/* 当前节点是不是链上的最后一个：它的下一个就是哨兵时成立 */
#define ListIsLast(node, head)   ((node)->next == (head))

/* 按节点遍历：pos 走遍链上每个业务节点，撞到哨兵 head 停 */
#define ListForEach(pos, head) \
  for((pos) = (head)->next; (pos) != (head); (pos) = (pos)->next)

/* 按宿主结构体遍历：pos 是宿主指针，type 是宿主类型，member 是节点成员名 */
#define ListForEachEntry(pos, head, type, member) \
  for((pos) = ListFirstEntry((head), type, member); \
      &(pos)->member != (head); \
      (pos) = ListNextEntry((pos), type, member))

/* 遍历时允许删掉当前节点：多备一个同类型指针 n，提前记下下一个 */
#define ListForEachEntrySafe(pos, n, head, type, member) \
  for((pos) = ListFirstEntry((head), type, member), \
      (n) = ListNextEntry((pos), type, member); \
      &(pos)->member != (head); \
      (pos) = (n), (n) = ListNextEntry((n), type, member))

/* 取链上最后一个宿主结构体；空链时算出来的地址不可用，所以先配 ListEmpty 判空 */
#define ListLastEntry(head, type, member) \
  ListEntry((head)->prev, type, member)

/* 由当前宿主结构体取上一个宿主结构体 */
#define ListPrevEntry(pos, type, member) \
  ListEntry((pos)->member.prev, type, member)

/* 按宿主结构体【倒着】遍历：从最后一个走回第一个，撞到哨兵 head 停。
 * 挂链顺序就是绘制顺序（后画的盖住先画的）时，倒着走 = "从最上层往下层" ——
 * 命中测试要的就是这个顺序 */
#define ListForEachEntryReverse(pos, head, type, member) \
  for((pos) = ListLastEntry((head), type, member); \
      &(pos)->member != (head); \
      (pos) = ListPrevEntry((pos), type, member))

/* 链表操作接口：都是搬指针，不做入参校验（传进来的指针必须有效）；
 * 需要校验的是上层的初始化 / 注册接口 */
/* 初始化哨兵：prev / next 都指向自己，表示空链。链表当头用之前必须先初始化 */
void ListInit(list_head_t* head);
/* 把 node 插到 pos 之后 */
void ListInsertAfter(list_head_t* pos, list_head_t* node);
/* 把 node 插到 pos 之前 */
void ListInsertBefore(list_head_t* pos, list_head_t* node);
/* 头插：插到哨兵 head 之后，成为链上第一条 */
void ListAdd(list_head_t* head, list_head_t* node);
/* 尾插：插到哨兵 head 之前，成为链上最后一条 */
void ListAddTail(list_head_t* head, list_head_t* node);
/* 把 node 从它所在的链上摘下；摘完 node 自成环，可以再插到别的链上 */
void ListDel(list_head_t* node);
/* 空链判断：只有哨兵自己没有业务节点时返回 1 */
uint8_t ListEmpty(const list_head_t* head);
/* node 是否挂在某条链上：没挂过的节点自成环，返回 0 */
uint8_t ListLinked(const list_head_t* node);

#endif
