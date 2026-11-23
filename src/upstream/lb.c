/* src/upstream/lb.c
 *
 * 백엔드 선택. 명세: upstream.h
 */
#include "upstream/upstream.h"

#include "upstream/internal_state.h"

backend_t *upstream_pick(upstream_group_t *g, const backend_t *exclude, int64_t now_ms)
{
    size_t i;
    backend_t *selected = NULL;
    if (g == NULL || g->n_backends == 0) return NULL;
    pthread_mutex_lock(&g->lock);
    for (i = 0; i < g->n_backends; i++) {
        size_t idx = (g->next_index + i) % g->n_backends;
        backend_t *b = g->backends[idx];
        int available;
        if (b == exclude) continue;
        pthread_mutex_lock(&b->lock);
        available = b->down_until_ms <= now_ms;
        pthread_mutex_unlock(&b->lock);
        if (available) {
            selected = b;
            g->next_index = (idx + 1) % g->n_backends;
            break;
        }
    }
    pthread_mutex_unlock(&g->lock);
    return selected;
}
