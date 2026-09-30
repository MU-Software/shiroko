#include <utlist.h>

#include "shr_lru.h"

void shr__lru_push(shr__lru_node **head, shr__lru_node *n) { DL_PREPEND(*head, n); }

void shr__lru_remove(shr__lru_node **head, shr__lru_node *n) {
    DL_DELETE(*head, n);
    n->prev = n->next = NULL;
}

shr__lru_node *shr__lru_oldest(shr__lru_node *head) { return head ? head->prev : NULL; }
