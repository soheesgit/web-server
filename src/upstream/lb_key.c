/* 고정 키 선택 (AI 라우팅). 명세: upstream.h upstream_pick_key, D-045.
 * 일반 프록시의 선택(lb.c)은 건드리지 않고, picker 줄이 있는 그룹만 이 경로를 쓴다. */
#include "upstream/internal_state.h"

#include <string.h>

/* splitmix64 마무리: 비트를 고르게 섞는다. bench/analyze.py의 mix64와 같다. */
static uint64_t mix64(uint64_t h)
{
    h ^= h >> 30;
    h *= UINT64_C(0xbf58476d1ce4e5b9);
    h ^= h >> 27;
    h *= UINT64_C(0x94d049bb133111eb);
    return h ^ (h >> 31);
}

/* FNV-1a 64 + mix64. bench/analyze.py의 fnv_hash와 같다. */
static uint64_t hash_bytes(const char *p, size_t n)
{
    uint64_t h = UINT64_C(1469598103934665603);

    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)p[i];
        h *= UINT64_C(1099511628211);
    }
    return mix64(h);
}

backend_t *upstream_pick_key(upstream_group_t *g, const char *key, size_t key_len,
                             const backend_t *exclude, int64_t now_ms)
{
    backend_t *order[CFG_MAX_BACKENDS];
    uint64_t score[CFG_MAX_BACKENDS];
    uint64_t kh;
    size_t n = 0;
    size_t total = 1; /* 이번 요청 포함 */
    backend_t *picked = NULL;

    if (g->picker == CFG_PICK_RR || key_len == 0) {
        return upstream_pick(g, exclude, now_ms);
    }
    kh = hash_bytes(key, key_len);
    pthread_mutex_lock(&g->lock);
    /* 후보를 rendezvous 점수 내림차순으로 줄 세운다 (백엔드 최대 16대라 삽입 정렬) */
    for (size_t i = 0; i < g->count; i++) {
        backend_t *b = &g->backends[i];
        uint64_t sc;
        size_t j = n;

        if (b == exclude || !upstream_eligible(b, now_ms)) {
            continue;
        }
        sc = mix64(kh ^ hash_bytes(b->hostport, strlen(b->hostport)));
        total += b->active;
        while (j > 0 && score[j - 1] < sc) {
            score[j] = score[j - 1];
            order[j] = order[j - 1];
            j--;
        }
        score[j] = sc;
        order[j] = b;
        n++;
    }
    if (n > 0 && g->picker == CFG_PICK_HASH) {
        picked = order[0];
    } else if (n > 0) {
        size_t denom = 100 * n;
        size_t cap = (total * (size_t)g->bound_pct + denom - 1) / denom;

        for (size_t i = 0; i < n && picked == NULL; i++) {
            if (order[i]->active < cap) {
                picked = order[i];
            }
        }
    }
    pthread_mutex_unlock(&g->lock);
    return picked;
}
