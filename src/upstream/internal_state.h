#ifndef HTTPD_UPSTREAM_INTERNAL_STATE_H
#define HTTPD_UPSTREAM_INTERNAL_STATE_H

#include <netinet/in.h>
#include <pthread.h>

#include "upstream/upstream.h"

#define UPSTREAM_IDLE_MS 60000

typedef enum { UP_RR, UP_LEAST, UP_P2C } up_policy_t;
typedef enum { UP_HEALTHY, UP_COOLDOWN, UP_RECOVERING } up_health_t;

typedef struct pool_entry {
    upstream_conn_t conn;
    struct pool_entry *next;
    int64_t idle_since;
} pool_entry_t;

struct backend {
    upstream_group_t *group;
    struct sockaddr_in address;
    char hostport[CFG_HOST_MAX + 6];
    up_health_t health;
    int64_t until;
    unsigned failures, recovery_limit, recovery_successes;
    size_t active, idle_count, peak_active;
    uint64_t assigned, sockets, reused, cooldowns, recoveries, expired;
    uint64_t detect_to_healthy_ms;
    int64_t detected_at;
    pool_entry_t *idle;
};

struct upstream_group {
    char name[CFG_NAME_MAX];
    pthread_mutex_t lock;
    backend_t backends[CFG_MAX_BACKENDS];
    size_t count, cursor, preview_cursor, max_idle;
    unsigned fail_threshold;
    int cooldown_ms;
    up_policy_t policy;
    uint64_t rng;
    bool ramp;
};

/** 그룹 lock을 잡은 호출자가 시도 가능 여부를 확인한다. */
bool upstream_eligible(const backend_t *b, int64_t now);
/** 그룹 lock 아래에서 제외 대상을 건너뛰고 다음 대상을 고른다. */
backend_t *upstream_select_locked(upstream_group_t *g, const backend_t *exclude, int64_t now);
/** 선택·예약을 함께 수행하며, 이후 호출자가 연결을 반납한다. */
int upstream_checkout(upstream_group_t *g, backend_t *pinned, const backend_t *exclude,
                      backend_t **out_selected, upstream_conn_t **out);
/** 현재 시도의 예약을 유지하고 소켓만 새로 연다. */
int upstream_reconnect(upstream_conn_t *c);
/** 목록에서 분리된 유휴 연결을 모두 닫는다. */
void upstream_discard_idle(pool_entry_t *entries);

#endif
