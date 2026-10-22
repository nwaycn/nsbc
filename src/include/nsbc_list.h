#ifndef NSBC_LIST_H
#define NSBC_LIST_H

/* 侵入式双向循环链表 + 单向哈希链（base 层，业务无关）。
 * 用法：在对象内嵌入 nsbc_node_t；链表节点不额外分配（REQ-ARCH-007 同口径）。
 */

#include <stddef.h>
#include "nsbc_common.h"

typedef struct nsbc_node {
    struct nsbc_node *next;
    struct nsbc_node *prev;
} nsbc_node_t;

static inline void nsbc_node_init(nsbc_node_t *n) { n->next = n->prev = n; }

static inline int nsbc_list_empty(const nsbc_node_t *head)
{
    return head->next == head;
}

static inline void nsbc_list_insert(nsbc_node_t *head, nsbc_node_t *n)
{
    n->next = head->next;
    n->prev = head;
    head->next->prev = n;
    head->next = n;
}

static inline void nsbc_list_append(nsbc_node_t *head, nsbc_node_t *n)
{
    n->prev = head->prev;
    n->next = head;
    head->prev->next = n;
    head->prev = n;
}

static inline void nsbc_list_del(nsbc_node_t *n)
{
    n->prev->next = n->next;
    n->next->prev = n->prev;
    n->next = n->prev = n;
}

#define nsbc_list_entry(ptr, type, member) NSBC_CONTAINER_OF(ptr, type, member)

#define nsbc_list_for_each(pos, head)                                           \
    for ((pos) = (head)->next; (pos) != (head); (pos) = (pos)->next)

#define nsbc_list_for_each_safe(pos, tmp, head)                                 \
    for ((pos) = (head)->next, (tmp) = (pos)->next; (pos) != (head);            \
         (pos) = (tmp), (tmp) = (tmp)->next)

/* 单向哈希链节点 */
typedef struct nsbc_hnode {
    struct nsbc_hnode *next;
} nsbc_hnode_t;

static inline void nsbc_hnode_init(nsbc_hnode_t *h) { h->next = NULL; }

#define nsbc_hlist_for_each(pos, head)                                          \
    for ((pos) = (head); (pos) != NULL; (pos) = (pos)->next)

#endif /* NSBC_LIST_H */
