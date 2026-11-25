/* 선택과 시도 수 예약은 같은 그룹 mutex에서 수행한다. */
#include "upstream/internal_state.h"
#include "upstream/internal_demo_trace.h"

bool upstream_eligible(const backend_t *b, int64_t now)
{
    switch (b->health) {
    case UP_HEALTHY:
        return true;
    case UP_COOLDOWN:
        return now >= b->until && b->active == 0;
    case UP_RECOVERING:
        return b->active < b->recovery_limit;
    }
    return false;
}

static uint64_t random_next(upstream_group_t *g)
{
    uint64_t x = g->rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g->rng = x;
    return x * UINT64_C(2685821657736338717);
}

static size_t random_index(upstream_group_t *g, size_t n)
{
    const uint64_t threshold = (UINT64_C(0) - n) % n;
    uint64_t value;
    do {
        value = random_next(g);
    } while (value < threshold);
    return (size_t)(value % n);
}

backend_t *upstream_select_locked(upstream_group_t *g, const backend_t *exclude, int64_t now)
{
    uint64_t started = demo_trace_clock_us();
    size_t candidates[CFG_MAX_BACKENDS];
    size_t count = 0;
    size_t best = 0;
    const backend_t *sample_a = NULL, *sample_b = NULL;
    for (size_t i = 0; i < g->count; i++) {
        size_t index = (g->cursor + i) % g->count;
        backend_t *b = &g->backends[index];

        if (b == exclude || !upstream_eligible(b, now)) {
            continue;
        }
        if (g->policy == UP_RR) {
            best = index;
            count = 1;
            break;
        }
        if (g->policy == UP_LEAST && (count == 0 || b->active < g->backends[best].active)) {
            best = index;
        }
        candidates[count++] = index;
    }
    if (count != 0 && g->policy == UP_P2C) {
        best = candidates[0];
        if (count > 1) {
            size_t a = random_index(g, count);
            size_t b = random_index(g, count - 1);
            if (b >= a) {
                b++;
            }
            size_t first = a < b ? a : b;
            size_t second = a < b ? b : a;
            best = candidates[first];
            sample_a = &g->backends[candidates[first]];
            sample_b = &g->backends[candidates[second]];
            if (g->backends[candidates[second]].active < g->backends[best].active) {
                best = candidates[second];
            }
        }
    }
    if (started != 0) {
        static const char *const policies[] = {"rr", "least_requests", "p2c"};
        demo_trace_choice(policies[g->policy], 0, started, count != 0 ? &g->backends[best] : NULL);
        for (size_t i = 0; i < g->count; i++) {
            const backend_t *b = &g->backends[(g->cursor + i) % g->count];
            const char *reason = "eligible";
            if (b == exclude) {
                reason = "excluded";
            } else if (!upstream_eligible(b, now)) {
                reason = b->health == UP_COOLDOWN ? "cooldown" : "recovery-limit";
            } else if (sample_a != NULL && b != sample_a && b != sample_b) {
                reason = "not-sampled";
            }
            demo_trace_candidate(b, reason, i + 1);
        }
    }
    if (count == 0) {
        return NULL;
    }
    g->cursor = (best + 1) % g->count;
    return &g->backends[best];
}

backend_t *upstream_pick(upstream_group_t *g, const backend_t *exclude, int64_t now_ms)
{
    backend_t *pick = NULL;
    pthread_mutex_lock(&g->lock);
    for (size_t i = 0; i < g->count; i++) {
        size_t index = (g->preview_cursor + i) % g->count;
        backend_t *b = &g->backends[index];
        if (b != exclude && upstream_eligible(b, now_ms)) {
            g->preview_cursor = (index + 1) % g->count;
            pick = b;
            break;
        }
    }
    pthread_mutex_unlock(&g->lock);
    return pick;
}
