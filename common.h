#ifndef COMMON_H
#define COMMON_H

#include <stddef.h>
#include <sys/types.h>

#define LOCALHOST "127.0.0.1"
#define MAX_LINE 2048
#define MAX_VALUE 128

typedef enum {
    OP_INSERT = 1,
    OP_DELETE = 2,
    OP_REPLACE = 3,
    OP_APPEND = 4
} OpType;

typedef struct {
    OpType type;
    int pos;               
    char value[MAX_VALUE];  
} Operation;

const char *op_type_name(OpType type);
int parse_operation(const char *text, Operation *op);
int operation_to_text(const Operation *op, char *buf, size_t buflen);

int connect_to_server(int base_port, int server_id);
ssize_t read_line(int fd, char *buf, size_t maxlen);
int write_all(int fd, const char *buf, size_t len);
int send_line(int fd, const char *fmt, ...);

#endif
