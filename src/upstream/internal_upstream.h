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
    int inflight; /* 빌려 간 연결 수 = 처리 중 요청 수 (acquire~release). group->mu */
    uint64_t name_hash;     /* hostport의 해시 (rendezvous 점수용) */
    upstream_conn_t **idle; /* 유휴 연결 (뒤가 최근). 크기 reg->pool_max_idle */
    size_t n_idle;          /* group->mu */
};

struct upstream_group {
    struct upstream_registry *reg; /* 소속 레지스트리 (설정 값). 빌림 */
    char name[CFG_NAME_MAX];
    struct backend *backends; /* 설정 순서. 소유 */
    size_t n_backends;
    size_t rr;           /* 다음 라운드로빈 시작 위치. mu */
    cfg_picker_t picker; /* 설정의 picker */
    int bound_pct;       /* CFG_PICK_BOUNDED의 상한 배율 (%) */
    pthread_mutex_t mu;  /* rr, 백엔드의 fails·excluded_until_ms·idle 보호 */
};

struct upstream_registry {
    struct upstream_group *groups; /* 설정 순서. 소유 */
    size_t n_groups;
    int fail_threshold; /* backend_fail_threshold */
    int cooldown_ms;    /* backend_cooldown_ms */
    size_t max_idle;    /* pool_max_idle (백엔드당) */
};

/**
 * 64비트 값의 비트를 고르게 섞는다 (splitmix64 마무리 단계).
 *
 * @param h  입력 값
 * @return   섞은 값
 *
 * 에러:    없음
 * 소유권:  없음
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  bench (시나리오 C) · 추후 단위 테스트
 */
static inline uint64_t upstream_mix64(uint64_t h)
{
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebULL;
    return h ^ (h >> 31);
}

/**
 * 바이트열의 64비트 해시 (FNV-1a 64 + upstream_mix64). lb.c(키)와 backend.c(hostport)가 쓴다.
 *
 * @param p  바이트. n이 0이면 NULL 허용
 * @param n  길이
 * @return   해시
 *
 * 에러:    없음
 * 소유권:  p를 빌림
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  bench (시나리오 C) · 추후 단위 테스트
 */
static inline uint64_t upstream_hash(const char *p, size_t n)
{
    uint64_t h = 1469598103934665603ULL;

    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)p[i];
        h *= 1099511628211ULL;
    }
    return upstream_mix64(h);
}

#endif
