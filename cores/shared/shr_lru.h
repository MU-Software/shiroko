#ifndef SHIROKO_SHR_LRU_H
#define SHIROKO_SHR_LRU_H

#include <stddef.h>

/* Intrusive LRU list, most recent first; unlinked nodes have prev == NULL. */
typedef struct shr__lru_node {
    struct shr__lru_node *prev, *next;
} shr__lru_node;

void shr__lru_push(shr__lru_node **head, shr__lru_node *n);
void shr__lru_remove(shr__lru_node **head, shr__lru_node *n);
shr__lru_node *shr__lru_oldest(shr__lru_node *head);
#define SHR_CONTAINER(p, T, member) ((T *)(void *)((char *)(p) - offsetof(T, member)))

#endif
