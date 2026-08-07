#include "client.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

int 
send_request_to_server(int base_port, int server_id, const char *request, char *response, size_t response_len) 
{
    int fd = connect_to_server(base_port, server_id);
    if (fd < 0) return -1;

    if (send_line(fd, "%s\n", request) != 0) {
        close(fd);
        return -1;
    }

    if (read_line(fd, response, response_len) <= 0) {
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

long 
get_last_sequence_from_sequencer(int base_port) 
{
    char response[MAX_LINE];
    long last = -1;

    if (send_request_to_server(base_port, 0, "ADMIN LASTSEQ", response, sizeof(response)) != 0) {
        return -1;
    }
    if (sscanf(response, "LASTSEQ %ld", &last) != 1) return -1;
    return last;
}

int 
admin_all(int base_port, int num_servers, int shutdown_after) 
{
    long last = get_last_sequence_from_sequencer(base_port);
    char request[MAX_LINE];
    char response[MAX_LINE];
    int failures = 0;

    if (last < 0) {
        printf("couldn't contact sequencer to obtain last sequence number\n");
        return 1;
    }

    snprintf(request, sizeof(request), "%s %ld",
             shutdown_after ? "ADMIN SHUTDOWN" : "ADMIN PRINT", last);

    for (int sid = 0; sid < num_servers; sid++) {
        if (send_request_to_server(base_port, sid, request, response, sizeof(response)) != 0) {
            fprintf(stderr, "admin request failed for server %d\n", sid);
            failures++;
        } else {
            printf("server %d: %s\n", sid, response);
        }
    }
    return failures == 0 ? 0 : 1;
}

int 
get_one(int base_port, int server_id) 
{
    char response[MAX_LINE];
    if (send_request_to_server(base_port, server_id, "CLIENT GET", response, sizeof(response)) != 0) {
        fprintf(stderr, "failed to get list from server %d\n", server_id);
        return 1;
    }
    printf("%s\n", response);
    return 0;
}

void 
build_random_operation(int client_id, int op_index, int estimated_len, Operation *op) 
{
    int r = rand() % 100;
    char value[MAX_VALUE];

    snprintf(value, sizeof(value), "C%d_W%d", client_id, op_index);
    memset(op, 0, sizeof(*op));

    if (r < 40) {                         
        op->type = OP_INSERT;
        op->pos = estimated_len > 0 ? rand() % (estimated_len + 1) : 0;
        memcpy(op->value, value, strlen(value) + 1);
    } else if (r < 70) {                 
        op->type = OP_DELETE;
        op->pos = estimated_len > 0 ? rand() % estimated_len : 0;
    } else if (r < 90) {                  
        op->type = OP_REPLACE;
        op->pos = estimated_len > 0 ? rand() % estimated_len : 0;
        memcpy(op->value, value, strlen(value) + 1);
    } else {                           
        op->type = OP_APPEND;
        op->pos = -1;
        memcpy(op->value, value, strlen(value) + 1);
    }
}

void 
update_estimated_len(int *estimated_len, const Operation *op, const char *response) 
{
    if (strncmp(response, "OK", 2) != 0) return;
    if (op->type == OP_INSERT || op->type == OP_APPEND) {
        (*estimated_len)++;
    } else if (op->type == OP_DELETE && *estimated_len > 0) {
        (*estimated_len)--;
    }
}

int 
run_workload(int client_id, int base_port, int num_servers, int operations) 
{
    int estimated_len = INITIAL_LIST_LEN;
    int failures = 0;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    srand((unsigned int)(tv.tv_sec ^ tv.tv_usec ^ (client_id * 7919)));

    for (int i = 1; i <= operations; i++) {
        Operation op;
        char op_text[256];
        char request[512];
        char response[MAX_LINE];
        int server_id;

        build_random_operation(client_id, i, estimated_len, &op);
        if (operation_to_text(&op, op_text, sizeof(op_text)) != 0) {
            fprintf(stderr, "client %d: failed to format operation %d\n", client_id, i);
            failures++;
            continue;
        }

        snprintf(request, sizeof(request), "CLIENT %s", op_text);
        server_id = rand() % num_servers;

        if (send_request_to_server(base_port, server_id, request, response, sizeof(response)) != 0) {
            fprintf(stderr, "client %d: request %d to server %d failed: %s\n",
                    client_id, i, server_id, op_text);
            failures++;
            usleep(20000);
            continue;
        }

        printf("client %d op %02d -> server %d: %-24s => %s\n",
               client_id, i, server_id, op_text, response);
        fflush(stdout);
        update_estimated_len(&estimated_len, &op, response);

        usleep(1000 + rand() % 25000);
    }

    printf("client %d finished with %d failed requests\n", client_id, failures);
    return failures == 0 ? 0 : 1;
}

void 
usage() 
{
    printf("Usage:\n");
    printf("  Run workload: ./client <client_id> <base_port> <num_servers> [operations]\n");
    printf("  Print all:   ./client --print-all <base_port> <num_servers>\n");
    printf("  Shutdown all: ./client --shutdown-all <base_port> <num_servers>\n");
    printf("  Get one:     ./client --get <base_port> <server_id>\n");
}

int 
main(int argc, char **argv) 
{
    signal(SIGPIPE, SIG_IGN);

    if (argc >= 2 && strcmp(argv[1], "--print-all") == 0) {
        if (argc != 4) {
            usage();
            return EXIT_FAILURE;
        }
        return admin_all(atoi(argv[2]), atoi(argv[3]), 0);
    }

    if (argc >= 2 && strcmp(argv[1], "--shutdown-all") == 0) {
        if (argc != 4) {
            usage();
            return EXIT_FAILURE;
        }
        return admin_all(atoi(argv[2]), atoi(argv[3]), 1);
    }

    if (argc >= 2 && strcmp(argv[1], "--get") == 0) {
        if (argc != 4) {
            usage();
            return EXIT_FAILURE;
        }
        return get_one(atoi(argv[2]), atoi(argv[3]));
    }

    if (argc == 4 || argc == 5) {
        int client_id = atoi(argv[1]);
        int base_port = atoi(argv[2]);
        int num_servers = atoi(argv[3]);
        int operations = argc == 5 ? atoi(argv[4]) : DEFAULT_OPS;

        if (client_id <= 0 || num_servers < 3 || operations <= 0) {
            usage();
            return EXIT_FAILURE;
        }
        return run_workload(client_id, base_port, num_servers, operations);
    }

    usage();
    return EXIT_FAILURE;
}
