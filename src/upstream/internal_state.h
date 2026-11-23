/* src/upstream/internal_state.h
 *
 * upstream 구현 파일끼리 공유하는 상태. 다른 디렉토리에서 include하지 않는다.
 */
#ifndef HTTPD_UPSTREAM_INTERNAL_STATE_H
#define HTTPD_UPSTREAM_INTERNAL_STATE_H

#include "upstream/upstream.h"

#include <netinet/in.h>
#include <pthread.h>

typedef struct idle_node {
    upstream_conn_t *conn;
    struct idle_node *next;
} idle_node_t;

struct backend {
    struct sockaddr_in addr;
    char hostport[CFG_HOST_MAX + 8];
    unsigned failures;
    int64_t down_until_ms;
    pthread_mutex_t lock;
    bool lock_initialized;
    idle_node_t *idle;
    size_t idle_count;
    size_t idle_limit;
    int fail_threshold;
    int cooldown_ms;
};

struct upstream_group {
    char name[CFG_NAME_MAX];
    backend_t **backends;
    size_t n_backends;
    size_t next_index;
    pthread_mutex_t lock;
    bool lock_initialized;
};

struct upstream_registry {
    upstream_group_t *groups;
    size_t n_groups;
    int connect_timeout_ms;
    int response_timeout_ms;
};

#endif
