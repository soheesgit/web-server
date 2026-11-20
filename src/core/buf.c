/* src/core/buf.c
 *
 * 가변 버퍼. 명세: buf.h
 */
#include "core/buf.h"
#include "core/stub.h"

int buf_init(buf_t *b, size_t initial, size_t max)
{
    UNUSED(b);
    UNUSED(initial);
    UNUSED(max);
    /* TODO(core): 구현 — U-CORE-08 */
    STUB_RETURN(-1);
}

void buf_cleanup(buf_t *b)
{
    UNUSED(b);
    /* TODO(core): 구현 — (테스트 없음) */
}

int buf_append(buf_t *b, const void *p, size_t n)
{
    UNUSED(b);
    UNUSED(p);
    UNUSED(n);
    /* TODO(core): 구현 — U-CORE-08, U-CORE-10 */
    STUB_RETURN(-1);
}

int buf_appendf(buf_t *b, const char *fmt, ...)
{
    UNUSED(b);
    UNUSED(fmt);
    /* TODO(core): 구현 — (테스트 없음) */
    STUB_RETURN(-1);
}

void buf_consume(buf_t *b, size_t n)
{
    UNUSED(b);
    UNUSED(n);
    /* TODO(core): 구현 — U-CORE-11 */
    STUB_VOID();
}

void buf_reset(buf_t *b)
{
    UNUSED(b);
    /* TODO(core): 구현 — (테스트 없음) */
    STUB_VOID();
}

ssize_t buf_read_fd(buf_t *b, int fd)
{
    UNUSED(b);
    UNUSED(fd);
    /* TODO(core): 구현 — U-CORE-13 */
    STUB_RETURN(-1);
}

ssize_t buf_write_fd(buf_t *b, int fd)
{
    UNUSED(b);
    UNUSED(fd);
    /* TODO(core): 구현 — (테스트 없음) */
    STUB_RETURN(-1);
}
