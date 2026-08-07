#include "common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

const char *
op_type_name(OpType type) 
{
    switch (type) {
        case OP_INSERT:  
            return "INSERT";
        case OP_DELETE:  
            return "DELETE";
        case OP_REPLACE: 
            return "REPLACE";
        case OP_APPEND:
            return "APPEND";
        default:
            return "UNKNOWN";
    }
}

int 
parse_operation(const char *text, Operation *op) 
{
    char cmd[32];
    char val[MAX_VALUE];
    int pos;

    if (!text || !op) return -1;
    memset(op, 0, sizeof(*op));
    memset(cmd, 0, sizeof(cmd));
    memset(val, 0, sizeof(val));

    if (sscanf(text, "%31s", cmd) != 1) return -1;

    if (strcmp(cmd, "INSERT") == 0) {
        if (sscanf(text, "%31s %d %127s", cmd, &pos, val) != 3) return -1;
        op->type = OP_INSERT;
        op->pos = pos;
        memcpy(op->value, val, strlen(val) + 1);
        return 0;
    }
    if (strcmp(cmd, "DELETE") == 0) {
        if (sscanf(text, "%31s %d", cmd, &pos) != 2) return -1;
        op->type = OP_DELETE;
        op->pos = pos;
        return 0;
    }
    if (strcmp(cmd, "REPLACE") == 0) {
        if (sscanf(text, "%31s %d %127s", cmd, &pos, val) != 3) return -1;
        op->type = OP_REPLACE;
        op->pos = pos;
        memcpy(op->value, val, strlen(val) + 1);
        return 0;
    }
    if (strcmp(cmd, "APPEND") == 0) {
        if (sscanf(text, "%31s %127s", cmd, val) != 2) return -1;
        op->type = OP_APPEND;
        op->pos = -1;
        memcpy(op->value, val, strlen(val) + 1);
        return 0;
    }
    return -1;
}

int 
operation_to_text(const Operation *op, char *buf, size_t buflen) 
{
    if (!op || !buf || buflen == 0) return -1;

    switch (op->type) {
        case OP_INSERT:
            snprintf(buf, buflen, "INSERT %d %s", op->pos, op->value);
            return 0;
        case OP_DELETE:
            snprintf(buf, buflen, "DELETE %d", op->pos);
            return 0;
        case OP_REPLACE:
            snprintf(buf, buflen, "REPLACE %d %s", op->pos, op->value);
            return 0;
        case OP_APPEND:
            snprintf(buf, buflen, "APPEND %s", op->value);
            return 0;
        default:
            return -1;
    }
}

int 
connect_to_server(int base_port, int server_id) 
{
    int fd;
    struct sockaddr_in addr;
    int port = base_port + server_id;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, LOCALHOST, &addr.sin_addr) <= 0) {
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

ssize_t 
read_line(int fd, char *buf, size_t maxlen) 
{
    size_t n = 0;
    char c;

    if (!buf || maxlen == 0) return -1;

    while (n + 1 < maxlen) {
        ssize_t r = recv(fd, &c, 1, 0);
        if (r == 0) break;        
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (c == '\r') continue;
        if (c == '\n') break;
        buf[n++] = c;
    }
    buf[n] = '\0';
    return (ssize_t)n;
}

int 
write_all(int fd, const char *buf, size_t len) 
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

int 
send_line(int fd, const char *fmt, ...)
{
    char buf[MAX_LINE];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t)n >= sizeof(buf)) return -1;
    return write_all(fd, buf, (size_t)n);
}
