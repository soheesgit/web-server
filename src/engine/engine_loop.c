/* v2 추가 fd 등록은 이벤트 엔진이 구현되는 단계에서 연결한다. */
#include "engine/engine.h"

#include <errno.h>

int engine_watch(engine_t *e, int fd, unsigned events, void *obj)
{
    (void)e;
    (void)fd;
    (void)events;
    (void)obj;
    errno = ENOTSUP;
    return -1;
}

int engine_unwatch(engine_t *e, int fd)
{
    (void)e;
    (void)fd;
    errno = ENOTSUP;
    return -1;
}
