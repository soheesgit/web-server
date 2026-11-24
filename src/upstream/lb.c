/* src/upstream/lb.c
 *
 * 백엔드 선택. 명세: upstream.h
 */
#include "upstream/upstream.h"

#include "upstream/internal_upstream.h"

#include <stddef.h>

backend_t *upstream_pick(upstream_group_t *g, const backend_t *exclude, int64_t now_ms)
{
    backend_t *picked = NULL;

    pthread_mutex_lock(&g->mu);
    for (size_t k = 0; k < g->n_backends; k++) {
        size_t i = (g->rr + k) % g->n_backends;
        backend_t *b = &g->backends[i];

        if (b == exclude || b->excluded_until_ms > now_ms) {
            continue;
        }
        picked = b;
        g->rr = (i + 1) % g->n_backends;
        break;
    }
    pthread_mutex_unlock(&g->mu);
    return picked;
}

backend_t *upstream_pick_key(upstream_group_t *g, const char *key, size_t key_len,
                             const backend_t *exclude, int64_t now_ms)
{
    backend_t *order[CFG_MAX_BACKENDS];
    uint64_t score[CFG_MAX_BACKENDS];
    uint64_t kh;
    size_t n = 0;
    long total = 1; /* 이번 요청 포함 */
    backend_t *picked = NULL;

    if (g->picker == CFG_PICK_RR || key_len == 0) {
        return upstream_pick(g, exclude, now_ms);
    }
    kh = upstream_hash(key, key_len);
    pthread_mutex_lock(&g->mu);
    /* 후보를 점수 내림차순으로 줄 세운다 (백엔드 최대 16대라 삽입 정렬) */
    for (size_t i = 0; i < g->n_backends; i++) {
        backend_t *b = &g->backends[i];
        uint64_t sc = upstream_mix64(kh ^ b->name_hash); /* rendezvous 점수 */
        size_t j = n;

        if (b == exclude || b->excluded_until_ms > now_ms) {
            continue;
        }
        n++;
        total += b->inflight;
        while (j > 0 && score[j - 1] < sc) {
            score[j] = score[j - 1];
            order[j] = order[j - 1];
            j--;
        }
        score[j] = sc;
        order[j] = b;
    }
    if (n > 0 && g->picker == CFG_PICK_HASH) {
        picked = order[0];
    } else if (n > 0) {
        long denom = 100L * (long)n;
        long cap = (total * g->bound_pct + denom - 1) / denom;

        for (size_t i = 0; i < n && picked == NULL; i++) {
            if (order[i]->inflight < cap) {
                picked = order[i];
            }
        }
    }
    pthread_mutex_unlock(&g->mu);
    return picked;
}
