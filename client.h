#ifndef CLIENT_H
#define CLIENT_H

#include "common.h"

#define DEFAULT_OPS 50
#define INITIAL_LIST_LEN 39

typedef enum {
    MODE_WORKLOAD,
    MODE_PRINT_ALL,
    MODE_SHUTDOWN_ALL,
    MODE_GET_ONE
} ClientMode;

#endif