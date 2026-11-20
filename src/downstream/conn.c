/* src/downstream/conn.c
 *
 * 연결 상태 머신. 명세: conn.h
 */
#include "downstream/conn.h"
#include "core/stub.h"

#include <stddef.h>

conn_t *conn_new(int fd, const struct sockaddr_storage *peer, const conn_env_t *env, int64_t now_ms)
{
    UNUSED(fd);
    UNUSED(peer);
    UNUSED(env);
    UNUSED(now_ms);
    /* TODO(downstream): 구현 — U-DS-40, U-DS-44 */
    STUB_RETURN(NULL);
}

void conn_free(conn_t *c)
{
    UNUSED(c);
    /* TODO(downstream): 구현 — E-SL-04, E-CG-09, E-PX-17 */
}

void conn_serve_blocking(conn_t *c)
{
    UNUSED(c);
    /* TODO(downstream): 구현 — E-ST-*, E-CG-*, E-PX-*, E-KA-*, E-ER-*, E-SL-* */
    STUB_VOID();
}

conn_status_t conn_on_event(conn_t *c, int fd, unsigned events, int64_t now_ms)
{
    UNUSED(c);
    UNUSED(fd);
    UNUSED(events);
    UNUSED(now_ms);
    /* TODO(downstream): 구현 — U-DS-40, U-DS-44 */
    STUB_RETURN(CONN_CLOSE);
}

conn_status_t conn_on_timeout(conn_t *c, int64_t now_ms)
{
    UNUSED(c);
    UNUSED(now_ms);
    /* TODO(downstream): 구현 — E-ER-10, E-ER-11, E-KA-06, E-SL-01, E-CG-07, E-PX-10 */
    STUB_RETURN(CONN_CLOSE);
}

int conn_fd(const conn_t *c)
{
    UNUSED(c);
    /* TODO(downstream): 구현 — U-DS-40 */
    STUB_RETURN(-1);
}

int64_t conn_deadline_ms(const conn_t *c)
{
    UNUSED(c);
    /* TODO(downstream): 구현 — (테스트 없음) */
    STUB_RETURN(-1);
}
