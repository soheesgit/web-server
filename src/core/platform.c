/* POSIX 설정과 OS별 소켓 전송 차이. */
#if defined(__APPLE__)
/* SDK는 POSIX 모드에서 Darwin sendfile 선언을 숨긴다. 이 파일에서만 확장을 켠다. */
#undef _POSIX_C_SOURCE
#endif
#include "core/platform.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/sendfile.h>
#endif

static int set_flag(int fd, int get_cmd, int set_cmd, int bit)
{
    int flags = fcntl(fd, get_cmd);
    if (flags < 0) {
        return -1;
    }
    return (flags & bit) != 0 ? 0 : fcntl(fd, set_cmd, flags | bit);
}

int platform_set_nonblock(int fd)
{
    return set_flag(fd, F_GETFL, F_SETFL, O_NONBLOCK);
}

int platform_set_cloexec(int fd)
{
    return set_flag(fd, F_GETFD, F_SETFD, FD_CLOEXEC);
}

void platform_ignore_sigpipe(void)
{
    struct sigaction action = {.sa_handler = SIG_IGN};
    if (sigemptyset(&action.sa_mask) == 0) {
        (void)sigaction(SIGPIPE, &action, NULL);
    }
}

#if defined(__linux__) || (!defined(MSG_NOSIGNAL) && !defined(SO_NOSIGPIPE))
typedef struct {
    sigset_t mask;
    sigset_t previous;
    int already_pending;
} sigpipe_guard_t;

static int block_sigpipe(sigpipe_guard_t *guard)
{
    sigemptyset(&guard->mask);
    sigaddset(&guard->mask, SIGPIPE);
    int rc = pthread_sigmask(SIG_BLOCK, &guard->mask, &guard->previous);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    sigset_t pending;
    if (sigpending(&pending) < 0) {
        int saved = errno;
        (void)pthread_sigmask(SIG_SETMASK, &guard->previous, NULL);
        errno = saved;
        return -1;
    }
    guard->already_pending = sigismember(&pending, SIGPIPE);
    return 0;
}

static void restore_sigpipe(const sigpipe_guard_t *guard)
{
    int saved = errno;
    /* 전송 중 새로 생긴 시그널만 걷어낸 뒤 호출 스레드의 원래 마스크를 복구한다. */
    if (!guard->already_pending) {
        struct timespec immediate = {0};
        while (sigtimedwait(&guard->mask, NULL, &immediate) < 0 && errno == EINTR) {
        }
    }
    (void)pthread_sigmask(SIG_SETMASK, &guard->previous, NULL);
    errno = saved;
}
#endif

ssize_t platform_send(int fd, const void *p, size_t n)
{
    int flags = 0;
#if defined(__APPLE__) || (!defined(MSG_NOSIGNAL) && defined(SO_NOSIGPIPE))
    int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0) {
#if defined(MSG_NOSIGNAL)
        /* Darwin에서 peer가 이미 닫힌 UNIX 소켓은 이 옵션 설정이 실패할 수 있다. */
        if (errno != EINVAL) {
            return -1;
        }
        flags = MSG_NOSIGNAL;
#else
        return -1;
#endif
    }
#elif defined(MSG_NOSIGNAL)
    flags = MSG_NOSIGNAL;
#else
    sigpipe_guard_t guard;
    if (block_sigpipe(&guard) < 0) {
        return -1;
    }
#endif
    ssize_t sent;
    do {
        sent = send(fd, p, n, flags);
    } while (sent < 0 && errno == EINTR);
#if !defined(MSG_NOSIGNAL) && !defined(SO_NOSIGPIPE)
    restore_sigpipe(&guard);
#endif
    return sent;
}

#if !defined(__linux__)
static ssize_t send_file_buffered(int out_fd, int in_fd, off_t *offset, size_t count)
{
    char bytes[16 * 1024];
    size_t want = count < sizeof(bytes) ? count : sizeof(bytes);
    ssize_t n;
    do {
        n = pread(in_fd, bytes, want, *offset);
    } while (n < 0 && errno == EINTR);
    if (n <= 0) {
        return n;
    }
    ssize_t sent = platform_send(out_fd, bytes, (size_t)n);
    if (sent > 0) {
        *offset += sent;
    }
    return sent;
}
#endif

ssize_t platform_sendfile(int out_fd, int in_fd, off_t *inout_off, size_t count)
{
    if (count == 0) {
        return 0;
    }
    if (count > (size_t)SSIZE_MAX) {
        count = (size_t)SSIZE_MAX;
    }

#if defined(__linux__)
    /* sendfile에는 MSG_NOSIGNAL 인자가 없어 전송하는 스레드에서만 SIGPIPE를 막는다. */
    sigpipe_guard_t guard;
    if (block_sigpipe(&guard) < 0) {
        return -1;
    }
    ssize_t sent;
    do {
        sent = sendfile(out_fd, in_fd, inout_off, count);
    } while (sent < 0 && errno == EINTR);
    restore_sigpipe(&guard);
    return sent;
#elif defined(__APPLE__)
    int enabled = 1;
    if (setsockopt(out_fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0) {
        if (errno == EINVAL) {
            return send_file_buffered(out_fd, in_fd, inout_off, count);
        }
        return -1;
    }
    int rc;
    off_t sent;
    do {
        sent = (off_t)count;
        rc = sendfile(in_fd, out_fd, *inout_off, &sent, NULL, 0);
    } while (rc < 0 && errno == EINTR && sent == 0);
    /* Darwin은 EINTR과 EAGAIN에서도 실제 전송량을 별도 인자에 기록한다. */
    if (rc < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
        return -1;
    }
    if (sent > 0) {
        *inout_off += sent;
        return (ssize_t)sent;
    }
    return rc < 0 ? -1 : 0;
#else
    return send_file_buffered(out_fd, in_fd, inout_off, count);
#endif
}

int64_t platform_now_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return -1;
    }
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

time_t platform_wall_time(void)
{
    return time(NULL);
}

int platform_raise_nofile(uint64_t want, uint64_t *out_got)
{
    struct rlimit limits;
    if (getrlimit(RLIMIT_NOFILE, &limits) < 0) {
        return -1;
    }
    uint64_t current = limits.rlim_cur == RLIM_INFINITY ? UINT64_MAX : (uint64_t)limits.rlim_cur;
    uint64_t ceiling = limits.rlim_max == RLIM_INFINITY ? UINT64_MAX : (uint64_t)limits.rlim_max;
#if defined(__APPLE__) && defined(OPEN_MAX)
    if (ceiling > OPEN_MAX) {
        ceiling = OPEN_MAX;
    }
#endif
    uint64_t target = want < ceiling ? want : ceiling;
    if (target > current) {
        limits.rlim_cur = (rlim_t)target;
        if (setrlimit(RLIMIT_NOFILE, &limits) < 0) {
            return -1;
        }
        current = target;
    }
    if (out_got != NULL) {
        *out_got = current;
    }
    return 0;
}
