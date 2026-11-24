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
