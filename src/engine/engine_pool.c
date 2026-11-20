/* src/engine/engine_pool.c
 *
 * v1 스레드 풀과 엔진 공통 진입점. 명세: engine.h
 */
#include "engine/engine.h"
#include "core/stub.h"

#include <stddef.h>

engine_t *engine_new(engine_kind_t kind, int workers, int queue_len, const engine_callbacks_t *cb,
                     void *ctx)
{
    UNUSED(kind);
    UNUSED(workers);
    UNUSED(queue_len);
    UNUSED(cb);
    UNUSED(ctx);
    /* TODO(engine): 구현 — U-EN-01, U-EN-02 */
    STUB_RETURN(NULL);
}

int engine_run(engine_t *e, int listen_fd, volatile sig_atomic_t *stop, int shutdown_grace_ms)
{
    UNUSED(e);
    UNUSED(listen_fd);
    UNUSED(stop);
    UNUSED(shutdown_grace_ms);
    /* TODO(engine): 구현 — U-EN-01, U-EN-02, E-* 전체 */
    STUB_RETURN(-1);
}

void engine_free(engine_t *e)
{
    UNUSED(e);
    /* TODO(engine): 구현 — U-EN-01 */
}
