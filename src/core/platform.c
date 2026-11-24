/* src/core/platform.c
 *
 * OS 차이를 감춘 저수준 호출. 명세: platform.h
 */
#include "core/platform.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/sendfile.h>
#elif defined(__APPLE__)
/* macOS <sys/socket.h>는 _POSIX_C_SOURCE가 정의되면 sendfile 선언을 숨긴다 */
struct sf_hdtr;
int sendfile(int fd, int s, off_t offset, off_t *len, struct sf_hdtr *hdtr, int flags);
#endif

int platform_set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        return -1;
    }
    if ((flags & O_NONBLOCK) != 0) {
        return 0;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int platform_set_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0) {
        return -1;
    }
    if ((flags & FD_CLOEXEC) != 0) {
        return 0;
    }
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

void platform_ignore_sigpipe(void)
{
    struct sigaction sa;
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    (void)sigaction(SIGPIPE, &sa, NULL);
}

ssize_t platform_send(int fd, const void *p, size_t n)
{
    ssize_t sent;
    if (n == 0) {
        return 0;
    }
    do {
        /* macOS도 최신 SDK에는 MSG_NOSIGNAL이 있다. 있으면 호출 단위로 SIGPIPE를 막는다.
         * SO_NOSIGPIPE는 상대가 닫힌 뒤에는 설정이 거부될 수 있어 대체 경로로만 쓴다 */
#if defined(MSG_NOSIGNAL)
        sent = send(fd, p, n, MSG_NOSIGNAL);
#else
#if defined(SO_NOSIGPIPE)
        {
            int one = 1;
            /* 실패해도 무시한다. 상대가 닫힌 소켓에서는 macOS가 setsockopt를 거부할 수 있는데,
             * 그때도 아래 send가 EPIPE를 돌려줘야 한다 (U-CORE-30) */
            (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
        }
#endif
        sent = send(fd, p, n, 0);
#endif
    } while (sent < 0 && errno == EINTR);
    return sent;
}

ssize_t platform_sendfile(int out_fd, int in_fd, off_t *inout_off, size_t count)
{
    if (inout_off == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (count == 0) {
        return 0;
    }
#if defined(__linux__)
    {
        /* sendfile에는 MSG_NOSIGNAL이 없다. 이 스레드에서 SIGPIPE를 잠시 막고, 호출이 만든
         * SIGPIPE는 sigtimedwait로 거둬 프로세스에 전달되지 않게 한다. 일부를 보낸 뒤 끊기면
         * 반환값이 양수여도 SIGPIPE가 대기 중일 수 있어 반환값으로 판단하지 않는다. */
        sigset_t pipe_set, old_set, pending;
        struct timespec zero = {0, 0};
        int was_pending;
        int saved;
        ssize_t n;

        (void)sigemptyset(&pipe_set);
        (void)sigaddset(&pipe_set, SIGPIPE);
        (void)pthread_sigmask(SIG_BLOCK, &pipe_set, &old_set);
        (void)sigemptyset(&pending);
        (void)sigpending(&pending);
        was_pending = sigismember(&pending, SIGPIPE) == 1;
        do {
            n = sendfile(out_fd, in_fd, inout_off, count);
        } while (n < 0 && errno == EINTR);
        saved = errno;
        (void)sigemptyset(&pending);
        if (!was_pending && sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1) {
            (void)sigtimedwait(&pipe_set, NULL, &zero);
        }
        (void)pthread_sigmask(SIG_SETMASK, &old_set, NULL);
        errno = saved;
        return n;
    }
#elif defined(__APPLE__)
    {
        off_t sent = 0;
        int rc;
        do {
            rc = sendfile(in_fd, out_fd, *inout_off, &sent, NULL, 0);
        } while (rc < 0 && errno == EINTR && sent == 0);
        if (sent > 0) {
            *inout_off += sent;
            return (ssize_t)sent;
        }
        return rc < 0 ? -1 : 0;
    }
#else
    {
        char buf[16384];
        size_t want = count < sizeof(buf) ? count : sizeof(buf);
        ssize_t nr, nw;
        do {
            nr = pread(in_fd, buf, want, *inout_off);
        } while (nr < 0 && errno == EINTR);
        if (nr <= 0) {
            return nr;
        }
        nw = platform_send(out_fd, buf, (size_t)nr);
        if (nw > 0) {
            *inout_off += nw;
        }
        return nw;
    }
#endif
}

int64_t platform_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
        return -1;
    }
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

time_t platform_wall_time(void)
{
    return time(NULL);
}

int platform_raise_nofile(uint64_t want, uint64_t *out_got)
{
    struct rlimit lim;
    rlim_t target;
    if (getrlimit(RLIMIT_NOFILE, &lim) < 0) {
        return -1;
    }
    target = want > (uint64_t)lim.rlim_max ? lim.rlim_max : (rlim_t)want;
#if defined(__APPLE__) && defined(OPEN_MAX)
    if (target > OPEN_MAX) {
        target = OPEN_MAX;
    }
#endif
    if (lim.rlim_cur < target) {
        lim.rlim_cur = target;
        if (setrlimit(RLIMIT_NOFILE, &lim) < 0) {
            return -1;
        }
    }
    if (out_got != NULL) {
        *out_got = (uint64_t)lim.rlim_cur;
    }
    return 0;
}
