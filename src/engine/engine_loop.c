/* src/engine/engine_loop.c
 *
 * v2 이벤트 루프는 선택 단계이며 현재 서버 엔진은 v1 스레드 풀만 지원한다.
 * 이벤트 루프를 선택하지 못하게 engine_new에서 ENOTSUP을 반환한다. 이 파일은
 * 공개 추가-fd API도 명시적으로 같은 상태를 반환해 미구현 동작을 숨기지 않는다.
 */
#include "engine/engine.h"

#include <errno.h>
#include <stddef.h>

int engine_watch(engine_t *e, int fd, unsigned events, void *obj)
{
    (void)fd;
    (void)events;
    (void)obj;
    if (e == NULL) {
        errno = EINVAL;
        return -1;
    }
    errno = ENOTSUP;
    return -1;
}

int engine_unwatch(engine_t *e, int fd)
{
    (void)fd;
    if (e == NULL) {
        errno = EINVAL;
        return -1;
    }
    errno = ENOTSUP;
    return -1;
}
