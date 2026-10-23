#include "./ui_framework/ui_framework.h"


/*=========================================================================*/
/*  UI 层：层级 / 链 / 对象的初始化与注册                                    */
/*=========================================================================*/

/* 初始化一条链：建立"没有节点 / 没有挂在层级上 / 没有前后链"的初始状态
 * 链在当 chain 使用前必须先初始化：要么调用本函数，要么静态初始化时把
 * objs / node 两个哨兵都写成自指
 * 只建立结构、不参与挂载顺序，所以可以先初始化所有链，再逐条挂到层级上；
 * 对象要在链挂到层级之后才能注册（draws 在层级上） */
void UI_InitChain(ui_chain_t* chain)
{
  if(chain == NULL)                  // 参数不合法
  {
    return;
  }

  ListInit(&chain->objs);         /* 操作链表：空链，哨兵自指，注册节点时靠它认出"这条链还是空的" */
  chain->level      = NULL;          /* 还没往上挂到任何层级 */
  ListInit(&chain->node);         /* 本链自己的挂载节点自成环，表示"还没挂到层级上" */
  chain->registered = 0;             /* 还没挂到层级上 */
  chain->activation = activated;     /* 默认整链激活(和对象同一个语义：建好就是"可见可操作"的)，
                                      * 想藏起来用 ui_control 的 ui_chain_set_active(chain, 0)。
                                      * 不在这里置值的话，静态清零恰好等于 deactivated，
                                      * 门控一开整条链就"凭空消失"了 */
  chain->inited     = 1;             /* 标成已初始化，之后可以当链用 */
}



/* 初始化一个层级：建立"没有父级 / 没有兄弟层级 / 没有子层级 / 没有链表 / 没有可绘制对象"的初始状态
 * 层级对象在当 level 或当 parent 使用前都必须先初始化：要么调用本函数，
 * 要么静态初始化时把 node / children / chains / draws 四个哨兵都写成自指
 * 只建立结构、不参与挂载顺序，所以可以先初始化所有层级，再自底向上逐个注册 */
void UI_InitHierarchy(ui_hierarchy_t* level)
{
  if(level == NULL)                  // 参数不合法
  {
    return;
  }

  level->parent = level;             /* 没有父级：向上到根为止（父子层级不循环） */
  ListInit(&level->node);         /* 自己的挂载节点自成环，表示"还没有兄弟层级" */
  ListInit(&level->children);     /* 没有子层级：子层级链表是空链 */
  ListInit(&level->chains);       /* 还没有挂链：链链表是空链 */
  ListInit(&level->draws);        /* 还没有可绘制对象：绘制优先级链表是空链 */
  level->registered = 0;             /* 还没向上挂载 */
  level->inited     = 1;             /* 标成已初始化，之后可以当层级用 */
}



/* 初始化一个 UI 对象：建立"没有挂链 / 没有扩展 / 没有可绘制优先级"的初始状态
 * 对象在当 ui_obj 使用前必须先初始化：要么调用本函数，要么静态初始化时把
 * ops_node / draw_node / exts 三个哨兵都写成自指
 * 只建立结构、不参与挂载，所以可以先初始化对象、再设 para，最后走注册 / 扩展接口 */
void UI_InitObject(ui_object_t* obj)
{
  if(obj == NULL)                    // 参数不合法
  {
    return;
  }

  ListInit(&obj->ops_node);       /* 操作链表节点自成环：还没挂到任何链上 */
  ListInit(&obj->draw_node);      /* 绘制优先级节点自成环：还没挂到任何层级上 */
  obj->chain      = NULL;            /* 还没挂到任何链上 */
  obj->registered = 0;               /* 还没注册过 */
  ListInit(&obj->exts);           /* 还没有上层扩展：扩展链表是空链 */
  obj->activation = activated;       /* 默认激活：建好就是"可见可操作"的。
                                      * 要藏起来用 ui_obj_set_active(obj, 0)，
                                      * 藏起来之后绘制和操作两条遍历都会跳过它 */
  obj->inited     = 1;               /* 标成已初始化，之后可以当对象用 */
}


/* ① 注册操作链：把 ui_obj 挂进 chain->objs，只决定【本链内】的操作 / 遍历顺序（局部）。
 *   ops_node 插在 prev 之后；prev 传 NULL 插成本链第一条（插到哨兵之后）。
 * 链上的 objs 是环形：哨兵的 next 是第一条、prev 是最后一条，环上不存在 NULL。
 * 不碰绘制优先级 —— 那是 ② 的活 */
ui_reg_result_t UI_RegisterObject_Chain(ui_chain_t* chain, ui_object_t* ui_obj, ui_object_t* prev)
{
  if(chain == NULL || ui_obj == NULL)          // 参数不合法
  {
    return UI_REG_FAIL;
  }

  if(chain->inited != 1)                       // 链还没初始化：objs 不可信，认不出"这条链还是空的"
  {
    return UI_REG_FAIL;
  }

  if(ui_obj->inited != 1)                      // 对象还没初始化：ops_node 不可信，注册后遍历会读到垃圾
  {
    return UI_REG_FAIL;
  }

  if(ui_obj->registered == 1)                  // 已经挂过的节点不能重复注册，否则会把原链打断
  {
    return UI_REG_FAIL;
  }

  // prev 不属于这条链：没注册过的节点 registered 为 0、chain 还没填，同样被拦下
  if(prev != NULL && (prev->registered != 1 || prev->chain != chain))
  {
    return UI_REG_FAIL;
  }

  ui_obj->registered = 1;                      /* 标记已挂链：② 与重复注册校验都靠它 */
  ui_obj->chain = chain;                       /* 记下归属的链：找 level、判断 prev 都在本链上 */

  if(prev != NULL)                             // 插到 prev 之后
  {
    ListInsertAfter(&prev->ops_node, &ui_obj->ops_node);
  }
  else
  {
    /* 插成第一条：插到哨兵之后。空链和已有节点是同一个动作，不用分开处理 */
    ListAdd(&chain->objs, &ui_obj->ops_node);
  }

  return UI_REG_OK;
}

/* ② 注册绘制表：把 ui_obj 挂进 level->draws，只决定【整屏】的覆盖先后（全局 z 序）。
 * 绘制优先级不按链分组，统一收在层级上：draw_node 插在 prev 之后，越靠后越上面。
 * prev 传 NULL 就是插成第一条（最底层），插到哨兵之后。和 ① 口径一致。
 * 层级是显式传的，和 ① 完全独立：不挂操作链、只想被画出来的对象直接调本函数即可 */
ui_reg_result_t UI_RegisterObject_Draw(ui_hierarchy_t* level, ui_object_t* ui_obj, ui_object_t* prev)
{
  if(level == NULL || ui_obj == NULL)          // 参数不合法
  {
    return UI_REG_FAIL;
  }

  if(level->inited != 1)                       // 层级还没初始化：draws 不可信，挂上去会把链表搞乱
  {
    return UI_REG_FAIL;
  }

  if(ui_obj->inited != 1)                      // 对象还没初始化：draw_node 不可信
  {
    return UI_REG_FAIL;
  }

  if(ListLinked(&ui_obj->draw_node) != 0)      // 已经挂在某张绘制表上了，别重复挂
  {
    return UI_REG_FAIL;
  }

  if(prev != NULL && ListLinked(&prev->draw_node) == 0)   // prev 还没挂在绘制表上：不能当插入点
  {
    return UI_REG_FAIL;
  }

  if(prev != NULL)                             // 插到 prev 之后（z 序排在它上面）
  {
    ListInsertAfter(&prev->draw_node, &ui_obj->draw_node);
  }
  else
  {
    /* 插成第一条：插到哨兵之后，落在最底层。空链和已有节点是同一个动作 */
    ListAdd(&level->draws, &ui_obj->draw_node);
  }

  return UI_REG_OK;
}

/* 合并版：一次做完 ① + ②。
 * 先把两步的校验都过一遍（避免只挂上一半），再依次落地 —— 与分开调两步结果完全一致 */
ui_reg_result_t UI_RegisterObject_Node(ui_chain_t* chain, ui_object_t* ui_obj, ui_object_t* prev)
{
  if(chain == NULL || ui_obj == NULL)                  // 参数不合法
  {
    return UI_REG_FAIL;
  }

  if(chain->inited != 1 || chain->level == NULL)       // 链没初始化 / 没挂到层级：①② 都做不成
  {
    return UI_REG_FAIL;
  }

  if(ui_obj->inited != 1 || ui_obj->registered == 1)   // 对象没初始化 / 已经挂过链
  {
    return UI_REG_FAIL;
  }

  if(ListLinked(&ui_obj->draw_node) != 0)              // 已经挂在某条绘制表上
  {
    return UI_REG_FAIL;
  }

  if(prev != NULL && (prev->registered != 1 || prev->chain != chain))
  {
    return UI_REG_FAIL;
  }

  if(prev != NULL && ListLinked(&prev->draw_node) == 0)   // prev 没进绘制表：② 没处插，先拦下
  {
    return UI_REG_FAIL;
  }

  /* 校验全过，两步依次落地；此时两步内部不会再失败 */
  UI_RegisterObject_Chain(chain, ui_obj, prev);
  UI_RegisterObject_Draw(chain->level, ui_obj, prev);
  return UI_REG_OK;
}


/* 往对象上挂一个扩展节点：把上层模块的状态挂到 ui_obj 上
 * 框架不解释扩展里装什么，只认"挂在哪个对象上、身份 key 是什么"
 * 参数校验：ui_obj / ext 不能为 NULL、ext->key 不能为 NULL；ui_obj 必须是已初始化的对象（inited == 1）；
 * ext 不能已经挂着；同一对象上同 key 只许一个；校验不过直接返回 UI_REG_FAIL、不注册 */
ui_reg_result_t UI_RegisterObject_Ext(ui_object_t* ui_obj, ui_ext_node_t* ext)
{
  list_head_t* n;

  if(ui_obj == NULL || ext == NULL || ext->key == NULL)   // 参数不合法：没对象 / 没节点 / 没身份
  {
    return UI_REG_FAIL;
  }

  if(ui_obj->inited != 1)                                 // 对象还没初始化：exts 不可信，认不出"这个对象还没有扩展"
  {
    return UI_REG_FAIL;
  }

  if(ext->obj != NULL)                                    // 已经挂着了：先摘下再挂，否则会把原链打断
  {
    return UI_REG_FAIL;
  }

  /* node 是 ui_ext_node_t 的首成员，节点地址就是结构体地址：不用 container_of 也能反推 */
  for(n = ui_obj->exts.next; n != &ui_obj->exts; n = n->next)
  {
    if(((ui_ext_node_t*)n)->key == ext->key)              // 同一种扩展在这个对象上只许挂一个
    {
      return UI_REG_FAIL;
    }
  }

  ext->obj = ui_obj;                                      /* 记下归属，取回时靠它反查 */
  ListAddTail(&ui_obj->exts, &ext->node);              /* 尾插：取回顺序无关，插哪都行 */
  return UI_REG_OK;
}

/* 把扩展节点从它挂着的对象上摘下；没挂过就什么都不做 */
void UI_UnregisterObject_Ext(ui_ext_node_t* ext)
{
  if(ext == NULL || ext->obj == NULL)                     // 没传节点、或本来就没挂着：无事可做
  {
    return;
  }

  ListDel(&ext->node);                                 /* 摘完 node 自成环，可以再挂到别的对象上 */
  ext->obj = NULL;
}

/* 在 ui_obj 上找身份是 key 的扩展，返回它的 data；没找到（或对象还没初始化 / 还没挂过任何扩展）返回 NULL */
void* UI_GetObject_Ext(ui_object_t* ui_obj, const void* key)
{
  list_head_t* n;

  if(ui_obj == NULL || key == NULL || ui_obj->inited != 1)        // 对象还没初始化：exts 不可信
  {
    return NULL;
  }

  for(n = ui_obj->exts.next; n != &ui_obj->exts; n = n->next)
  {
    if(((ui_ext_node_t*)n)->key == key)
    {
      return ((ui_ext_node_t*)n)->data;
    }
  }

  return NULL;
}

/* 注册一条 UI 链：把 chain 挂到 level 里、插在同一层级的上一条链 prev 之后
 *   prev 传 NULL：挂成本层级的第一条链，插到 level->chains 哨兵之后
 *   prev 非 NULL：插到 prev 之后，prev 必须已注册过且挂在本层级下（靠 ui_chain_t->level 反查）
 * 层级里的链是环形：哨兵的 next 是第一条、prev 是最后一条，环上不存在 NULL；
 * 只有一条链时哨兵的 prev / next 都指向它
 * 参数校验：level / chain 不能为 NULL，level 必须是已初始化的层级（inited == 1）、chain 必须是已初始化的链（inited == 1）；
 * chain 不能是已注册过的链；prev 非 NULL 时必须是已注册过且挂在本层级下的链；
 * 校验不过直接返回 UI_REG_FAIL、不注册 */
ui_reg_result_t UI_RegisterChain(ui_hierarchy_t* level, ui_chain_t* chain, ui_chain_t* prev)
{
  if(level == NULL || chain == NULL)           // 参数不合法
  {
    return UI_REG_FAIL;
  }

  if(level->inited != 1)                       // 层级还没初始化：chains 不可信，认不出"本层级还没有链"
  {
    return UI_REG_FAIL;
  }

  if(chain->inited != 1)                       // 链还没初始化：objs 不可信，挂上去后遍历会读到垃圾
  {
    return UI_REG_FAIL;
  }

  if(chain->registered == 1)                   // 已经挂过的链不能重复注册，否则会把原链打断
  {
    return UI_REG_FAIL;
  }

  // prev 不属于这个层级：没注册过的链 registered 为 0、level 还没填，同样被拦下
  if(prev != NULL && (prev->registered != 1 || prev->level != level))
  {
    return UI_REG_FAIL;
  }

  chain->registered = 1;                       /* 校验通过，标记为已注册 */
  chain->level = level;                        /* 记下归属的层级，之后靠它判断 prev 是否在本层级下 */

  if(prev != NULL)                             // 插到 prev 之后
  {
    ListInsertAfter(&prev->node, &chain->node);
    return UI_REG_OK;
  }

  ListAdd(&level->chains, &chain->node);    /* 挂成第一条：插到哨兵之后 */
  return UI_REG_OK;
}



/* 注册一个层级：把 level 挂到 parent 下、插在同一父级的上一兄弟层级 prev 之后
 *   parent 传 NULL：注册根层级，它没有父级，parent 指向自己（父子层级不循环，向上到根为止）；
 * 此时 prev 也传 NULL 就表示这个根层级自己成一个环、没有兄弟层级
 *   prev 传 NULL：挂成父层级的第一条子层级（插到 parent->children 哨兵之后）
 * 兄弟层级都是同一父级下的一条环形链表：父层级哨兵的 next 是第一条子层级、prev 是最后一条，
 * 环上不存在 NULL；只有一条时哨兵的 prev / next 都指向它。
 * 根层级没有父级哨兵，兄弟关系直接挂在各自的 node 环上（从任一个根层级出发沿 node 走回自己就是一圈）
 * 支持自底向上注册：可以先把子层级挂到 level 下，再调用本函数把 level 向上挂到 parent 下，
 * 本函数不碰 level->children / level->chains，所以已经挂上的子层级和链不会因为这次注册而丢失
 * 参数校验：level 不能为 NULL 且必须是已初始化的层级（inited == 1）；parent 非 NULL 时也必须是已初始化的层级；
 * level 不能是已注册过的层级、不能自己当自己的父级；prev 非 NULL 时必须已注册过且确实挂在同一个父级下；
 * 校验不过直接返回 UI_REG_FAIL、不注册 */
ui_reg_result_t UI_RegisterHierarchy(ui_hierarchy_t* level, ui_hierarchy_t* parent, ui_hierarchy_t* prev)
{
  if(level == NULL || level->inited != 1)      // 参数不合法，或 level 还没初始化
  {
    return UI_REG_FAIL;
  }

  if(level->registered == 1)                   // 已经挂过的层级不能重复注册，否则会重复挂到父级上
  {
    return UI_REG_FAIL;
  }

  if(parent != NULL && parent->inited != 1)    // 父级不是已初始化的层级，不能当层级用
  {
    return UI_REG_FAIL;
  }

  if(parent == level)                          // 自己不能当自己的父级，否则向上遍历会死循环
  {
    return UI_REG_FAIL;
  }

  if(prev != NULL)
  {
    if(prev->registered != 1)                  // prev 没注册过，不能当兄弟层级
    {
      return UI_REG_FAIL;
    }

    if(prev->parent != ((parent == NULL) ? prev : parent)) // prev 不是这个父级下的子层级
    {
      return UI_REG_FAIL;
    }
  }

  level->registered = 1;                       /* 校验通过，标记为已注册 */

  if(parent == NULL)                           // 无父级，自立根级
  {
    level->parent = level;                     /* 没有父级：向上到根为止 */
    if(prev != NULL)                           // 根层级之间也排兄弟：插到 prev 之后
    {
      ListInsertAfter(&prev->node, &level->node);
    }
    return UI_REG_OK;                          /* prev 为 NULL 时 node 保持自环，表示"没有兄弟层级" */
  }

  level->parent = parent;                      /* 有父级：挂载在父级下 */
  if(prev != NULL)                             // 插到 prev 之后
  {
    ListInsertAfter(&prev->node, &level->node);
    return UI_REG_OK;
  }

  ListAdd(&parent->children, &level->node); /* 挂成父层级的第一条子层级：插到哨兵之后 */
  return UI_REG_OK;
}


/*=========================================================================*/
/*  激活门控：绘制 / 操作两条遍历都只走"已激活"的对象                        */
/*  设计说明见 ui_framework.h 的"激活门控"一节                              */
/*=========================================================================*/

/* 这个对象现在激活着吗(1=激活)。传 NULL 返回 0 */
uint8_t ui_obj_is_active(const ui_object_t* obj)
{
  if(obj == NULL)
  {
    return 0;
  }

  return (obj->activation == activated) ? 1u : 0u;
}


/* 按 z 序把 level 上【已激活且 para 非空】的对象逐个交给 visit。
 * 顺着 draws 从哨兵的 next 走到回到哨兵：这就是 z 序（越靠后越上，后画的盖住先画的） */
void ui_level_draw(ui_hierarchy_t* level, void (*visit)(ui_object_t* obj))
{
  ui_object_t* obj;

  if(level == NULL || visit == NULL)          // 参数不合法
  {
    return;
  }

  ListForEachEntry(obj, &level->draws, ui_object_t, draw_node)
  {
    /* 藏起来的对象不画；para 还没配好的也跳过（draw 函数要靠 para 才知道画什么）；
     * 对象所属的链被整条去激活时也不画（链 = 同一层级内的"组开关/视图/浮层"，
     * 见 ui_framework.h 的 ui_chain_t::activation 说明） */
    if(ui_obj_is_active(obj) != 0u && obj->para != NULL &&
       (obj->chain == NULL || obj->chain->activation == activated))
    {
      visit(obj);
    }
  }
}


/* 按操作链顺序遍历 chain 上【已激活】的对象，逐个交给 visit。
 * visit 返回 1 表示"找到了，停下"。
 * 链自己被整条去激活(ui_chain_set_active(chain, 0))时这条链跳过、一个都不走 */
uint8_t ui_chain_visit(ui_chain_t* chain, uint8_t (*visit)(ui_object_t* obj))
{
  ui_object_t* obj;

  if(chain == NULL || visit == NULL)          // 参数不合法
  {
    return 0;
  }

  if(chain->activation == deactivated)        // 链整条藏起来了：一个都不遍历
  {
    return 0;
  }

  ListForEachEntry(obj, &chain->objs, ui_object_t, ops_node)
  {
    if(ui_obj_is_active(obj) != 0u)
    {
      if(visit(obj) != 0u)
      {
        return 1;                             // 被 visit 主动停下
      }
    }
  }

  return 0;                                   // 整条链走完
}
