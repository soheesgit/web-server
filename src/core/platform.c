/* src/core/platform.c
 *
 * OS 차이를 감춘 저수준 호출. 명세: platform.h
 * OS 분기(#ifdef __linux__ 등)는 이 파일에만 둔다.
 */
#include "core/platform.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stddef.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(__linux__)
#include <pthread.h>
#include <stdbool.h>
#include <sys/sendfile.h>
#elif defined(__APPLE__)
/* macOS <sys/socket.h>는 _POSIX_C_SOURCE가 정의되면 sendfile 선언을 숨긴다 */
struct sf_hdtr;
int sendfile(int fd, int s, off_t offset, off_t *len, struct sf_hdtr *hdtr, int flags);
#endif

#define SENDFILE_FALLBACK_CHUNK 65536

int platform_set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL);

    if (fl < 0) {
        return -1;
    }
    if (fl & O_NONBLOCK) {
        return 0;
    }
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0 ? -1 : 0;
}

int platform_set_cloexec(int fd)
{
    int fl = fcntl(fd, F_GETFD);

    if (fl < 0) {
        return -1;
    }
    if (fl & FD_CLOEXEC) {
        return 0;
    }
    return fcntl(fd, F_SETFD, fl | FD_CLOEXEC) < 0 ? -1 : 0;
}

void platform_ignore_sigpipe(void)
{
    struct sigaction sa;

    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    (void)sigemptyset(&sa.sa_mask);
    (void)sigaction(SIGPIPE, &sa, NULL);
}

ssize_t platform_send(int fd, const void *p, size_t n)
{
    ssize_t w;

#if defined(MSG_NOSIGNAL)
    do {
        w = send(fd, p, n, MSG_NOSIGNAL);
    } while (w < 0 && errno == EINTR);
#else
#if defined(SO_NOSIGPIPE)
    {
        int one = 1;
        /* 이미 켜져 있어도 무해하다. 소켓이 아니면 아래 send가 ENOTSOCK을 낸다 */
        (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    }
#endif
    do {
        w = send(fd, p, n, 0);
    } while (w < 0 && errno == EINTR);
#endif
    return w;
}

#if !defined(__linux__) && !defined(__APPLE__)
/* pread + platform_send 루프. 한 번에 최대 SENDFILE_FALLBACK_CHUNK 바이트만 보낸다. */
static ssize_t sendfile_fallback(int out_fd, int in_fd, off_t *inout_off, size_t count)
{
    char tmp[SENDFILE_FALLBACK_CHUNK];
    size_t want = count < sizeof(tmp) ? count : sizeof(tmp);
    ssize_t r;
    ssize_t w;

    if (want == 0) {
        return 0;
    }
    do {
        r = pread(in_fd, tmp, want, *inout_off);
    } while (r < 0 && errno == EINTR);
    if (r <= 0) {
        return r;
    }
    w = platform_send(out_fd, tmp, (size_t)r);
    if (w > 0) {
        *inout_off += w;
    }
    return w;
}
#endif

ssize_t platform_sendfile(int out_fd, int in_fd, off_t *inout_off, size_t count)
{
#if defined(__linux__)
    /* sendfile에는 MSG_NOSIGNAL이 없다. 이 스레드에서 SIGPIPE를 잠시 막고, 호출이 만든
     * SIGPIPE는 sigtimedwait로 거둬 프로세스에 전달되지 않게 한다 */
    sigset_t pipe_set, old_set, pending;
    struct timespec zero = {0, 0};
    bool was_pending;
    ssize_t w;
    int saved;

    (void)sigemptyset(&pipe_set);
    (void)sigaddset(&pipe_set, SIGPIPE);
    (void)pthread_sigmask(SIG_BLOCK, &pipe_set, &old_set);
    (void)sigemptyset(&pending);
    (void)sigpending(&pending);
    was_pending = sigismember(&pending, SIGPIPE) == 1;
    do {
        w = sendfile(out_fd, in_fd, inout_off, count);
    } while (w < 0 && errno == EINTR);
    saved = errno;
    if (w < 0 && saved == EPIPE && !was_pending) {
        (void)sigtimedwait(&pipe_set, NULL, &zero);
    }
    (void)pthread_sigmask(SIG_SETMASK, &old_set, NULL);
    errno = saved;
    return w;
#elif defined(__APPLE__)
    int one = 1;
    off_t len;
    int rc;

    (void)setsockopt(out_fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    for (;;) {
        len = (off_t)count;
        rc = sendfile(in_fd, out_fd, *inout_off, &len, NULL, 0);
        /* 실패해도 len에 실제 보낸 양이 들어온다 */
        *inout_off += len;
        if (rc == 0) {
            return (ssize_t)len;
        }
        if (len > 0) {
            return (ssize_t)len; /* EAGAIN·EINTR이라도 보낸 만큼은 성공으로 돌려준다 */
        }
        if (errno != EINTR) {
            return -1;
        }
    }
#else
    return sendfile_fallback(out_fd, in_fd, inout_off, count);
#endif
}

int64_t platform_now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return -1;
    }
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

time_t platform_wall_time(void)
{
    return time(NULL);
}

int platform_raise_nofile(uint64_t want, uint64_t *out_got)
{
    struct rlimit rl;
    rlim_t target;

    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) {
        return -1;
    }
    if (rl.rlim_cur != RLIM_INFINITY && (uint64_t)rl.rlim_cur < want) {
        target = (rlim_t)want;
        if (rl.rlim_max != RLIM_INFINITY && target > rl.rlim_max) {
            target = rl.rlim_max;
        }
#if defined(__APPLE__) && defined(OPEN_MAX)
        if (target > (rlim_t)OPEN_MAX) {
            target = (rlim_t)OPEN_MAX;
        }
#endif
        if (target > rl.rlim_cur) {
            rl.rlim_cur = target;
            if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
                return -1;
            }
        }
    }
    if (out_got != NULL) {
        *out_got = (uint64_t)rl.rlim_cur;
    }
    return 0;
}
