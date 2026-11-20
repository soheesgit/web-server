/* src/engine/poller_epoll.c
 *
 * Linux epoll poller (D-022: 빌드 대상에 맞는 파일 하나만 심볼을 정의). 명세: poller.h
 */
#if defined(__linux__) && !defined(HTTPD_FORCE_POLL)
#include "engine/poller.h"
#include "core/stub.h"

#include <stddef.h>

poller_t *poller_new(void)
{
    /* TODO(engine): 구현 — U-EN-05 */
    STUB_RETURN(NULL);
}

int poller_add(poller_t *p, int fd, unsigned events, void *ptr)
{
    UNUSED(p);
    UNUSED(fd);
    UNUSED(events);
    UNUSED(ptr);
    /* TODO(engine): 구현 — U-EN-05 */
    STUB_RETURN(-1);
}

int poller_mod(poller_t *p, int fd, unsigned events, void *ptr)
{
    UNUSED(p);
    UNUSED(fd);
    UNUSED(events);
    UNUSED(ptr);
    /* TODO(engine): 구현 — (테스트 없음) */
    STUB_RETURN(-1);
}

int poller_del(poller_t *p, int fd)
{
    UNUSED(p);
    UNUSED(fd);
    /* TODO(engine): 구현 — (테스트 없음) */
    STUB_RETURN(-1);
}

int poller_wait(poller_t *p, poller_event_t *out, int max, int timeout_ms)
{
    UNUSED(p);
    UNUSED(out);
    UNUSED(max);
    UNUSED(timeout_ms);
    /* TODO(engine): 구현 — U-EN-05 */
    STUB_RETURN(-1);
}

void poller_free(poller_t *p)
{
    UNUSED(p);
    /* TODO(engine): 구현 — U-EN-05 */
}

const char *poller_backend_name(void)
{
    /* TODO(engine): 구현 — (테스트 없음) */
    STUB_RETURN("");
}

#else
/* 이 플랫폼에서는 다른 poller 파일이 쓰인다. 빈 번역 단위 경고를 피하기 위한 선언. */
typedef int poller_epoll_unused_t;
#endif
