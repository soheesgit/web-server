/* src/upstream/internal_upstream.h
 *
 * upstream 내부 전용: 레지스트리·그룹·백엔드의 정의 (backend.c, lb.c, pool.c가 공유).
 * 다른 디렉토리에서 include 금지.
 */
#ifndef HTTPD_UPSTREAM_INTERNAL_UPSTREAM_H
#define HTTPD_UPSTREAM_INTERNAL_UPSTREAM_H

#include <netinet/in.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "upstream/upstream.h"

struct backend {
    struct upstream_group *group;    /* 소속 그룹. 빌림 */
    struct sockaddr_in addr;         /* 시작 시 해석한 주소 */
    char hostport[CFG_HOST_MAX + 8]; /* 설정의 "host:port" */
    int fails;                       /* 연속 실패 횟수. group->mu */
    int64_t excluded_until_ms; /* 제외 만료 단조 시각. 0이면 제외 아님. group->mu */
    upstream_conn_t **idle;    /* 유휴 연결 (뒤가 최근). 크기 reg->pool_max_idle */
    size_t n_idle;             /* group->mu */
};

struct upstream_group {
    struct upstream_registry *reg; /* 소속 레지스트리 (설정 값). 빌림 */
    char name[CFG_NAME_MAX];
    struct backend *backends; /* 설정 순서. 소유 */
    size_t n_backends;
    size_t rr;          /* 다음 라운드로빈 시작 위치. mu */
    pthread_mutex_t mu; /* rr, 백엔드의 fails·excluded_until_ms·idle 보호 */
};

struct upstream_registry {
    struct upstream_group *groups; /* 설정 순서. 소유 */
    size_t n_groups;
    int fail_threshold; /* backend_fail_threshold */
    int cooldown_ms;    /* backend_cooldown_ms */
    size_t max_idle;    /* pool_max_idle (백엔드당) */
};

#endif
