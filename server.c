#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static int server_id = -1;
static int base_port = 5000;
static int num_servers = 3;
static int listen_fd = -1;
static volatile sig_atomic_t running = 1;

static WordList list;
static pthread_mutex_t list_mutex = PTHREAD_MUTEX_INITIALIZER;

static PendingNode *pending_head = NULL;
static long next_expected = 1;
static pthread_mutex_t order_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t order_cond = PTHREAD_COND_INITIALIZER;

static long next_sequence_to_assign = 1;
static pthread_mutex_t seq_mutex = PTHREAD_MUTEX_INITIALIZER;

static const char *INITIAL_WORDS[] = {
    "Operating", "systems", "coordinate", "multiple", "processes", "and",
    "threads", "to", "share", "CPU", "time", "memory", "and", "I/O",
    "resources", "efficiently", "while", "ensuring", "correctness", "through",
    "synchronization", "mechanisms", "such", "as", "locks", "semaphores",
    "and", "monitors", "to", "avoid", "race", "conditions", "deadlocks",
    "and", "inconsistent", "states", "in", "concurrent", "programs"
};


char *
xstrdup(const char *s)
{
    const char *src = s ? s : "";
    size_t len = strlen(src);
    char *p = malloc(len + 1);
    if (!p) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    memcpy(p, src, len + 1);
    return p;
}

void 
check_capacity(WordList *list, int needed)
{
    int new_cap;
    char **new_items;

    if (needed <= list->cap) return;
    new_cap = list->cap > 0 ? list->cap : LIST_INITIAL_CAP;
    while (new_cap < needed) new_cap *= 2;

    new_items = realloc(list->items, (size_t)new_cap * sizeof(char *));
    if (!new_items){
        perror("realloc");
        exit(EXIT_FAILURE);
    }
    list->items = new_items;
    list->cap = new_cap;
}

void 
init_list(WordList *list)
{
    int count = (int)(sizeof(INITIAL_WORDS) / sizeof(INITIAL_WORDS[0]));
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
    check_capacity(list, count + 64);
    for (int i = 0; i < count; i++) {
        list->items[list->len++] = xstrdup(INITIAL_WORDS[i]);
    }
}

void 
free_list(WordList *list)
{
    for (int i = 0; i < list->len; i++) {
        free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
}

void 
insert(WordList *list, int pos, const char *value) 
{
    if (pos < 0 || pos > list->len) {
        printf("server %d: invalid insert position %d ignored\n", server_id, pos);
        return;
    }
    check_capacity(list, list->len + 1);
    for (int i = list->len; i > pos; i--) list->items[i] = list->items[i - 1];
    list->items[pos] = xstrdup(value);
    list->len++;
}

void 
delete(WordList *list, int pos)
{
    if (pos < 0 || pos >= list->len) {
        printf("server %d: invalid delete position %d ignored\n", server_id, pos);
        return;
    }
    free(list->items[pos]);
    for (int i = pos; i + 1 < list->len; i++) list->items[i] = list->items[i + 1];
    list->len--;
}

void
replace(WordList *list, int pos, const char *value)
{
    if (pos < 0 || pos >= list->len) {
        printf("server %d: invalid replace position %d\n", server_id, pos);        
        return;
    }
    free(list->items[pos]);
    list->items[pos] = xstrdup(value);
}

void 
append(WordList *list, const char *value)
{
    check_capacity(list, list->len + 1);
    list->items[list->len++] = xstrdup(value);
}

void 
apply_operation_to_list(const Operation *op)
{
    pthread_mutex_lock(&list_mutex);
    switch (op->type) {
        case OP_INSERT:
            insert(&list, op->pos, op->value);
            break;
        case OP_DELETE:
            delete(&list, op->pos);
            break;
        case OP_REPLACE:
            replace(&list, op->pos, op->value);
            break;
        case OP_APPEND:
            append(&list, op->value);
            break;
        default:
            break;
    }
    pthread_mutex_unlock(&list_mutex);
}

void 
append_str(char **buf, size_t *cap, size_t *len, const char *s)
{
    size_t need = strlen(s);
    if (*len + need + 1 > *cap) {
        while (*len + need + 1 > *cap) *cap *= 2;
        char *new_buf = realloc(*buf, *cap);
        if (!new_buf){
            perror("realloc");
            exit(EXIT_FAILURE);
        }
        *buf = new_buf;
    }
    memcpy(*buf + *len, s, need);
    *len += need;
    (*buf)[*len] = '\0';
}

void 
append_fmt(char **buf, size_t *cap, size_t *len, const char *fmt, ...) 
{
    char tmp[512];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    if (n < 0) return;
    if ((size_t)n < sizeof(tmp)) {
        append_str(buf, cap, len, tmp);
        return;
    }

    char *big = malloc((size_t)n + 1);
    if (!big){
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    append_str(buf, cap, len, big);
    free(big);
}

char *
list_to_indexed_string_locked(void) 
{
    size_t cap = 4096, len = 0;
    char *buf = calloc(cap, 1);
    if (!buf){
        perror("calloc");
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < list.len; i++) {
        append_fmt(&buf, &cap, &len, "%d:%s", i, list.items[i]);
        if (i + 1 < list.len) append_str(&buf, &cap, &len, " ");
    }
    return buf;
}

char *
list_to_sentence_string_locked(void) 
{
    size_t cap = 4096, len = 0;
    char *buf = calloc(cap, 1);
    if (!buf){
        perror("calloc");
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < list.len; i++) {
        append_str(&buf, &cap, &len, list.items[i]);
        if (i + 1 < list.len) append_str(&buf, &cap, &len, " ");
    }
    return buf;
}

char *
snapshot_sentence(void)
{
    char *sentence;
    pthread_mutex_lock(&list_mutex);
    sentence = list_to_sentence_string_locked();
    pthread_mutex_unlock(&list_mutex);
    return sentence;
}

void 
print_final_list(long upto_seq) 
{
    char *indexed;
    char *sentence;

    pthread_mutex_lock(&list_mutex);
    indexed = list_to_indexed_string_locked();
    sentence = list_to_sentence_string_locked();
    pthread_mutex_unlock(&list_mutex);

    printf("\n----- SERVER %d FINAL LIST (applied through sequence %ld) -----\n", server_id, upto_seq);
    printf("Indexed format:\n%s\n", indexed);
    printf("Sentence format:\n%s\n", sentence);
    printf("----- END SERVER %d FINAL LIST -----\n\n", server_id);
    fflush(stdout);

    free(indexed);
    free(sentence);
}

void 
free_pending_node(PendingNode *node)
{
    free(node);
}

void
apply_ready_operations_locked(void)
{
    while (pending_head && pending_head->seq == next_expected) {
        PendingNode *node = pending_head;
        pending_head = node->next;

        apply_operation_to_list(&node->op);
        next_expected++;
        free_pending_node(node);
        pthread_cond_broadcast(&order_cond);
    }
}

void
deliver_ordered_operation(long seq, const Operation *op)
{
    PendingNode *node;
    PendingNode **cur;

    pthread_mutex_lock(&order_mutex);

    if (seq < next_expected) {
        pthread_mutex_unlock(&order_mutex);
        return;
    }

    cur = &pending_head;
    while (*cur && (*cur)->seq < seq) cur = &(*cur)->next;

    if (*cur && (*cur)->seq == seq) {
        pthread_mutex_unlock(&order_mutex);
        return; 
    }

    node = malloc(sizeof(*node));
    if (!node){
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    node->seq = seq;
    node->op = *op;
    node->next = *cur;
    *cur = node;

    apply_ready_operations_locked();
    pthread_mutex_unlock(&order_mutex);
}

void
wait_until_applied(long seq)
{
    pthread_mutex_lock(&order_mutex);
    while (next_expected <= seq) {
        pthread_cond_wait(&order_cond, &order_mutex);
    }
    pthread_mutex_unlock(&order_mutex);
}

long
sequencer_last_assigned(void)
{
    long last;
    pthread_mutex_lock(&seq_mutex);
    last = next_sequence_to_assign - 1;
    pthread_mutex_unlock(&seq_mutex);
    return last;
}

int 
broadcast_order(long seq, const Operation *op) 
{
    char op_text[MAX_LINE];
    char response[MAX_LINE];
    int failures = 0;

    if (operation_to_text(op, op_text, sizeof(op_text)) != 0) return -1;

    for (int sid = 0; sid < num_servers; sid++) {
        if (sid == server_id) {
            deliver_ordered_operation(seq, op);
            continue;
        }

        int fd = connect_to_server(base_port, sid);
        if (fd < 0) {
            fprintf(stderr, "sequencer: failed to connect to server %d for ORDER %ld\n", sid, seq);
            failures++;
            continue;
        }

        if (send_line(fd, "ORDER %ld %s\n", seq, op_text) != 0 ||
            read_line(fd, response, sizeof(response)) <= 0 ||
            strncmp(response, "ACK", 3) != 0) {
            fprintf(stderr, "sequencer: failed to deliver ORDER %ld to server %d\n", seq, sid);
            failures++;
        }
        close(fd);
    }

    return failures == 0 ? 0 : -1;
}

long 
sequencer_assign_and_broadcast(const Operation *op) 
{
    long seq;

    pthread_mutex_lock(&seq_mutex);
    seq = next_sequence_to_assign++;
    pthread_mutex_unlock(&seq_mutex);

    broadcast_order(seq, op);
    return seq;
}

long 
forward_to_sequencer(const Operation *op) 
{
    char op_text[MAX_LINE];
    char response[MAX_LINE];
    long seq = -1;

    if (operation_to_text(op, op_text, sizeof(op_text)) != 0) return -1;

    int fd = connect_to_server(base_port, SEQUENCER_ID);
    if (fd < 0) return -1;

    if (send_line(fd, "SEQREQ %s\n", op_text) == 0 &&
        read_line(fd, response, sizeof(response)) > 0) {
        if (sscanf(response, "OK %ld", &seq) != 1) seq = -1;
    }
    close(fd);
    return seq;
}

long 
order_update_operation(const Operation *op) 
{
    if (server_id == SEQUENCER_ID) {
        return sequencer_assign_and_broadcast(op);
    }
    return forward_to_sequencer(op);
}

int 
parse_order_message(const char *line, long *seq, Operation *op) 
{
    const char *p = line;
    char *end = NULL;

    if (strncmp(p, "ORDER ", 6) != 0) return -1;
    p += 6;
    errno = 0;
    *seq = strtol(p, &end, 10);
    if (errno != 0 || end == p || *seq <= 0) return -1;
    while (*end == ' ') end++;
    return parse_operation(end, op);
}

void 
handle_client_update(int fd, const char *operation_text) 
{
    Operation op;
    long seq;

    if (parse_operation(operation_text, &op) != 0) {
        send_line(fd, "ERR bad operation\n");
        return;
    }

    seq = order_update_operation(&op);
    if (seq < 0) {
        send_line(fd, "ERR could not order operation\n");
        return;
    }

    wait_until_applied(seq);
    send_line(fd, "OK %ld\n", seq);
}

void 
handle_seqreq(int fd, const char *operation_text) 
{
    Operation op;
    long seq;

    if (server_id != SEQUENCER_ID) {
        send_line(fd, "ERR not sequencer\n");
        return;
    }
    if (parse_operation(operation_text, &op) != 0) {
        send_line(fd, "ERR bad operation\n");
        return;
    }

    seq = sequencer_assign_and_broadcast(&op);
    wait_until_applied(seq);
    send_line(fd, "OK %ld\n", seq);
}

void 
handle_admin_print(int fd, const char *line, int shutdown_after)
{
    long upto = 0;
    if (sscanf(line, shutdown_after ? "ADMIN SHUTDOWN %ld" : "ADMIN PRINT %ld", &upto) != 1) {
        upto = sequencer_last_assigned();
    }

    wait_until_applied(upto);
    print_final_list(upto);
    send_line(fd, "OK printed %ld\n", upto);

    if (shutdown_after) {
        running = 0;
    }
}

void *
connection_thread(void *arg) 
{
    int fd = *(int *)arg;
    char line[MAX_LINE];
    free(arg);

    ssize_t n = read_line(fd, line, sizeof(line));
    if (n <= 0) {
        close(fd);
        return NULL;
    }

    if (strncmp(line, "CLIENT GET", 10) == 0) {
        char *sentence = snapshot_sentence();
        send_line(fd, "LIST %s\n", sentence);
        free(sentence);
    } else if (strncmp(line, "CLIENT ", 7) == 0) {
        handle_client_update(fd, line + 7);
    } else if (strncmp(line, "SEQREQ ", 7) == 0) {
        handle_seqreq(fd, line + 7);
    } else if (strncmp(line, "ORDER ", 6) == 0) {
        long seq;
        Operation op;
        if (parse_order_message(line, &seq, &op) == 0) {
            deliver_ordered_operation(seq, &op);
            send_line(fd, "ACK %ld\n", seq);
        } else {
            send_line(fd, "ERR bad ORDER\n");
        }
    } else if (strncmp(line, "ADMIN LASTSEQ", 13) == 0) {
        send_line(fd, "LASTSEQ %ld\n", sequencer_last_assigned());
    } else if (strncmp(line, "ADMIN PRINT", 11) == 0) {
        handle_admin_print(fd, line, 0);
    } else if (strncmp(line, "ADMIN SHUTDOWN", 14) == 0) {
        handle_admin_print(fd, line, 1);
    } else {
        send_line(fd, "ERR unknown request\n");
    }

    close(fd);
    return NULL;
}

int 
create_listener(int port) 
{
    int fd;
    int opt = 1;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0){
        perror("socket");
        exit(EXIT_FAILURE);
    }

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(fd);
        exit(EXIT_FAILURE);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0){
        perror("bind");
        close(fd);
        exit(EXIT_FAILURE);
    }
    if (listen(fd, BACKLOG) < 0){
        perror("listen");
        close(fd);
        exit(EXIT_FAILURE);
    }
    return fd;
}

int 
main(int argc, char **argv) 
{
    int port;

    if (argc != 4) {
        printf("Usage: ./server <server_id> <base_port> <num_servers>\n");
        printf("Example: ./server 0 5000 3\n");
        return EXIT_FAILURE;
    }

    server_id = atoi(argv[1]);
    base_port = atoi(argv[2]);
    num_servers = atoi(argv[3]);

    if (server_id < 0 || server_id >= num_servers || num_servers < 3) {
        printf("server_id must be in [0, num_servers), and num_servers must be at least 3\n");
        return EXIT_FAILURE;
    }

    signal(SIGPIPE, SIG_IGN);

    init_list(&list);
    port = base_port + server_id;
    listen_fd = create_listener(port);

    printf("server %d listening on %s:%d%s\n",
           server_id, LOCALHOST, port,
           server_id == SEQUENCER_ID ? " [sequencer]" : "");
    fflush(stdout);

    while (running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int *conn_fd;
        pthread_t tid;
        fd_set read_fds;
        struct timeval timeout;
        int ready;

        FD_ZERO(&read_fds);
        FD_SET(listen_fd, &read_fds);
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;

        ready = select(listen_fd + 1, &read_fds, NULL, NULL, &timeout);
        if (ready < 0) {
            if (errno == EINTR) continue;
            if (!running) break;
            perror("select");
            continue;
        }
        if (ready == 0) continue;

        conn_fd = malloc(sizeof(int));
        if (!conn_fd) {
            perror("malloc");
            exit(EXIT_FAILURE);
        }
        *conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (*conn_fd < 0) {
            free(conn_fd);
            if (!running) break;
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        if (pthread_create(&tid, NULL, connection_thread, conn_fd) != 0) {
            perror("pthread_create");
            close(*conn_fd);
            free(conn_fd);
            continue;
        }
        pthread_detach(tid);
    }

    if (listen_fd >= 0) close(listen_fd);
    free_list(&list);
    return EXIT_SUCCESS;
}
