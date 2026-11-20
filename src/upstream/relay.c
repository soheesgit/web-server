/* src/upstream/relay.c
 *
 * 프록시 I/O 실행. 명세: relay.h
 */
#include "upstream/relay.h"
#include "core/stub.h"

#include <stddef.h>

relay_status_t relay_exchange_blocking(proxy_job_t *job, const http_limits_t *lim,
                                       int connect_timeout_ms, int response_timeout_ms,
                                       upstream_conn_t **out_conn, http_response_t *out_resp,
                                       proxy_fail_t *out_fail)
{
    UNUSED(job);
    UNUSED(lim);
    UNUSED(connect_timeout_ms);
    UNUSED(response_timeout_ms);
    UNUSED(out_conn);
    UNUSED(out_resp);
    UNUSED(out_fail);
    /* TODO(upstream): 구현 — E-PX-01~17 */
    if (out_fail != NULL) {
        *out_fail = PFAIL_CONNECT;
    }
    STUB_RETURN(RELAY_FAIL);
}

relay_t *relay_start(proxy_job_t *job, const http_limits_t *lim, int64_t now_ms,
                     int connect_timeout_ms, int response_timeout_ms)
{
    UNUSED(job);
    UNUSED(lim);
    UNUSED(now_ms);
    UNUSED(connect_timeout_ms);
    UNUSED(response_timeout_ms);
    /* TODO(upstream): 구현 — (테스트 없음) */
    STUB_RETURN(NULL);
}

relay_status_t relay_step(relay_t *r, unsigned events, int64_t now_ms)
{
    UNUSED(r);
    UNUSED(events);
    UNUSED(now_ms);
    /* TODO(upstream): 구현 — (테스트 없음) */
    STUB_RETURN(RELAY_FAIL);
}

int relay_fd(const relay_t *r)
{
    UNUSED(r);
    /* TODO(upstream): 구현 — (테스트 없음) */
    STUB_RETURN(-1);
}

int64_t relay_deadline_ms(const relay_t *r)
{
    UNUSED(r);
    /* TODO(upstream): 구현 — (테스트 없음) */
    STUB_RETURN(-1);
}

void relay_take(relay_t *r, upstream_conn_t **out_conn, http_response_t *out_resp,
                proxy_fail_t *out_fail)
{
    UNUSED(r);
    UNUSED(out_conn);
    UNUSED(out_resp);
    UNUSED(out_fail);
    /* TODO(upstream): 구현 — (테스트 없음) */
    STUB_VOID();
}

void relay_free(relay_t *r)
{
    UNUSED(r);
    /* TODO(upstream): 구현 — (테스트 없음) */
}
