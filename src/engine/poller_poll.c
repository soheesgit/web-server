/* src/engine/poller_poll.c
 *
 * poll(2) poller (그 밖의 OS 또는 -DHTTPD_FORCE_POLL) (D-022: 빌드 대상에 맞는 파일 하나만 심볼을
 * 정의). 명세: poller.h
 */
#if (!defined(__linux__) && !defined(__APPLE__) && !defined(__FreeBSD__)) ||                       \
    defined(HTTPD_FORCE_POLL)
#include "engine/poller.h"

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

enum { P_READ = 1u, P_WRITE = 2u, P_ERROR = 4u };

struct poller {
    struct pollfd *pfds; /* 등록 순서 (지우면 마지막 항목을 그 자리로 옮김) */
    void **ptrs;         /* pfds와 같은 위치 */
    size_t n, cap;
    int *index; /* fd → pfds 위치 + 1. 0이면 등록 안 됨 */
    size_t nindex;
};

poller_t *poller_new(void)
{
    return calloc(1, sizeof(poller_t));
}

static short to_poll(unsigned events)
{
    return (short)(((events & P_READ) ? POLLIN : 0) | ((events & P_WRITE) ? POLLOUT : 0));
}

static int find(const poller_t *p, int fd)
{
    if (fd < 0 || (size_t)fd >= p->nindex || p->index[fd] == 0) {
        return -1;
    }
    return p->index[fd] - 1;
}

static int grow_index(poller_t *p, int fd)
{
    size_t n = p->nindex == 0 ? 64 : p->nindex;
    int *ix;

    while (n <= (size_t)fd) {
        n *= 2;
    }
    ix = realloc(p->index, n * sizeof(*ix));
    if (ix == NULL) {
        return -1;
    }
    memset(ix + p->nindex, 0, (n - p->nindex) * sizeof(*ix));
    p->index = ix;
    p->nindex = n;
    return 0;
}

int poller_add(poller_t *p, int fd, unsigned events, void *ptr)
{
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    if (find(p, fd) >= 0) {
        errno = EEXIST;
        return -1;
    }
    if ((size_t)fd >= p->nindex && grow_index(p, fd) != 0) {
        return -1;
    }
    if (p->n == p->cap) {
        size_t cap = p->cap == 0 ? 64 : p->cap * 2;
        struct pollfd *pf = realloc(p->pfds, cap * sizeof(*pf));
        void **pt;

        if (pf == NULL) {
            return -1;
        }
        p->pfds = pf;
        pt = realloc(p->ptrs, cap * sizeof(*pt));
        if (pt == NULL) {
            return -1;
        }
        p->ptrs = pt;
        p->cap = cap;
    }
    p->pfds[p->n].fd = fd;
    p->pfds[p->n].events = to_poll(events);
    p->pfds[p->n].revents = 0;
    p->ptrs[p->n] = ptr;
    p->index[fd] = (int)++p->n;
    return 0;
}

int poller_mod(poller_t *p, int fd, unsigned events, void *ptr)
{
    int i = find(p, fd);

    if (i < 0) {
        errno = ENOENT;
        return -1;
    }
    p->pfds[i].events = to_poll(events);
    p->ptrs[i] = ptr;
    return 0;
}

int poller_del(poller_t *p, int fd)
{
    int i = find(p, fd);
    size_t last;

    if (i < 0) {
        errno = ENOENT;
        return -1;
    }
    last = p->n - 1;
    if ((size_t)i != last) {
        p->pfds[i] = p->pfds[last];
        p->ptrs[i] = p->ptrs[last];
        p->index[p->pfds[i].fd] = i + 1;
    }
    p->index[fd] = 0;
    p->n--;
    return 0;
}

int poller_wait(poller_t *p, poller_event_t *out, int max, int timeout_ms)
{
    int n, nout = 0;

    if (max < 1) {
        errno = EINVAL;
        return -1;
    }
    n = poll(p->pfds, (nfds_t)p->n, timeout_ms);
    if (n < 0) {
        return errno == EINTR ? 0 : -1;
    }
    for (size_t i = 0; i < p->n && nout < max && n > 0; i++) {
        short r = p->pfds[i].revents;

        if (r == 0) {
            continue;
        }
        n--;
        out[nout].fd = p->pfds[i].fd;
        out[nout].events = ((r & POLLIN) ? P_READ : 0u) | ((r & POLLOUT) ? P_WRITE : 0u) |
                           ((r & (POLLERR | POLLHUP | POLLNVAL)) ? P_ERROR : 0u);
        out[nout].ptr = p->ptrs[i];
        nout++;
    }
    return nout;
}

void poller_free(poller_t *p)
{
    int e = errno;

    if (p == NULL) {
        return;
    }
    free(p->pfds);
    free(p->ptrs);
    free(p->index);
    free(p);
    errno = e;
}

const char *poller_backend_name(void)
{
    return "poll";
}

#else
/* 이 플랫폼에서는 다른 poller 파일이 쓰인다. 빈 번역 단위 경고를 피하기 위한 선언. */
typedef int poller_poll_unused_t;
#endif
