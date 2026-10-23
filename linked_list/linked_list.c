#include "./linked_list/linked_list.h"



/*=========================================================================*/
/*  list_head 基础操作                                                      */
/*  只搬指针，不做入参校验：传进来的指针必须有效，判空/判归属交给上层的注册接口 */
/*=========================================================================*/

/* 初始化哨兵：prev / next 都指向自己，就是一条空链 */
void ListInit(list_head_t* head)
{
  head->prev = head;
  head->next = head;
}

/* 把 node 插到 pos 之后 */
void ListInsertAfter(list_head_t* pos, list_head_t* node)
{
  node->prev       = pos;
  node->next       = pos->next;
  pos->next->prev  = node;
  pos->next        = node;
}

/* 把 node 插到 pos 之前 */
void ListInsertBefore(list_head_t* pos, list_head_t* node)
{
  node->next       = pos;
  node->prev       = pos->prev;
  pos->prev->next  = node;
  pos->prev        = node;
}

/* 头插：插到哨兵 head 之后，成为链上第一条 */
void ListAdd(list_head_t* head, list_head_t* node)
{
  ListInsertAfter(head, node);
}

/* 尾插：插到哨兵 head 之前，成为链上最后一条 */
void ListAddTail(list_head_t* head, list_head_t* node)
{
  ListInsertBefore(head, node);
}

/* 把 node 从它所在的链上摘下；摘完 node 自成环，可以再插到别的链上 */
void ListDel(list_head_t* node)
{
  node->prev->next = node->next;
  node->next->prev = node->prev;
  node->prev       = node;
  node->next       = node;
}

/* 空链判断：只有哨兵自己没有业务节点时返回 1 */
uint8_t ListEmpty(const list_head_t* head)
{
  return (head->next == head) ? 1 : 0;
}

/* node 是否挂在某条链上：没挂过的节点自成环，返回 0 */
uint8_t ListLinked(const list_head_t* node)
{
  return (node->next == node) ? 0 : 1;
}
