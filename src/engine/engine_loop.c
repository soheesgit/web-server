/* src/engine/engine_loop.c
 *
 * v2 이벤트 루프와 추가 fd 등록. 명세: engine.h
 */
#include "engine/engine.h"
#include "core/stub.h"

int engine_watch(engine_t *e, int fd, unsigned events, void *obj)
{
    UNUSED(e);
    UNUSED(fd);
    UNUSED(events);
    UNUSED(obj);
    /* TODO(engine): 구현 — (테스트 없음) */
    STUB_RETURN(-1);
}

int engine_unwatch(engine_t *e, int fd)
{
    UNUSED(e);
    UNUSED(fd);
    /* TODO(engine): 구현 — (테스트 없음) */
    STUB_RETURN(-1);
}
