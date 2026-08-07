#ifndef SERVER_H
#define SERVER_H

#include "common.h"

typedef struct {
    char **items;
    int len;
    int cap;
} WordList;

typedef struct PendingNode {
    long seq;
    Operation op;
    struct PendingNode *next;
} PendingNode;

#define LIST_INITIAL_CAP 128
#define BACKLOG 64
#define SEQUENCER_ID 0

#endif 