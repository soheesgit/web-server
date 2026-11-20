/* src/core/platform.c
 *
 * OS 차이를 감춘 저수준 호출. 명세: platform.h
 */
#include "core/platform.h"
#include "core/stub.h"

#include <stddef.h>

int platform_set_nonblock(int fd)
{
    UNUSED(fd);
    /* TODO(core): 구현 — U-CORE-28 */
    STUB_RETURN(-1);
}

int platform_set_cloexec(int fd)
{
    UNUSED(fd);
    /* TODO(core): 구현 — U-CORE-29 */
    STUB_RETURN(-1);
}

void platform_ignore_sigpipe(void)
{
    /* TODO(core): 구현 — (테스트 없음) */
    STUB_VOID();
}

ssize_t platform_send(int fd, const void *p, size_t n)
{
    UNUSED(fd);
    UNUSED(p);
    UNUSED(n);
    /* TODO(core): 구현 — U-CORE-30 */
    STUB_RETURN(-1);
}

ssize_t platform_sendfile(int out_fd, int in_fd, off_t *inout_off, size_t count)
{
#if defined(__linux__)
    /* Linux: sendfile(out_fd, in_fd, inout_off, count) */
    UNUSED(out_fd);
    UNUSED(in_fd);
    UNUSED(inout_off);
    UNUSED(count);
    /* TODO(core): 구현 — (테스트 없음) */
    STUB_RETURN(-1);
#elif defined(__APPLE__)
    /* macOS: sendfile(in_fd, out_fd, *inout_off, &len, NULL, 0) */
    UNUSED(out_fd);
    UNUSED(in_fd);
    UNUSED(inout_off);
    UNUSED(count);
    /* TODO(core): 구현 — (테스트 없음) */
    STUB_RETURN(-1);
#else
    /* 그 밖의 OS: pread + platform_send 루프 */
    UNUSED(out_fd);
    UNUSED(in_fd);
    UNUSED(inout_off);
    UNUSED(count);
    /* TODO(core): 구현 — (테스트 없음) */
    STUB_RETURN(-1);
#endif
}

int64_t platform_now_ms(void)
{
    /* TODO(core): 구현 — (테스트 없음) */
    STUB_RETURN(-1);
}

time_t platform_wall_time(void)
{
    /* TODO(core): 구현 — (테스트 없음) */
    STUB_RETURN((time_t)-1);
}

int platform_raise_nofile(uint64_t want, uint64_t *out_got)
{
    UNUSED(want);
    UNUSED(out_got);
    /* TODO(core): 구현 — (테스트 없음) */
    STUB_RETURN(-1);
}
