/* src/engine/poller_epoll.c
 *
 * Linux epoll poller (D-022). 명세: poller.h
 */
#if defined(__linux__) && !defined(HTTPD_FORCE_POLL)
#include "engine/poller.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

/* fd 번호로 바로 찾는 등록 표 (연결이 많아도 이벤트마다 O(1)). */
typedef struct {
    bool used;
    void *ptr;
} ep_item_t;
struct poller {
    int fd;
    ep_item_t *items;
    size_t cap;
};

static bool registered(const poller_t *p, int fd)
{
    return fd >= 0 && (size_t)fd < p->cap && p->items[fd].used;
}

static uint32_t to_epoll(unsigned events)
{
    uint32_t out = EPOLLRDHUP;
    if (events & 1u)
        out |= EPOLLIN;
    if (events & 2u)
        out |= EPOLLOUT;
    return out;
}

poller_t *poller_new(void)
{
    poller_t *p = calloc(1, sizeof(*p));
    if (p == NULL)
        return NULL;
    p->fd = epoll_create1(EPOLL_CLOEXEC);
    if (p->fd < 0) {
        free(p);
        return NULL;
    }
    return p;
}

int poller_add(poller_t *p, int fd, unsigned events, void *ptr)
{
    struct epoll_event ev;
    if (p == NULL || fd < 0 || (events & ~3u) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (registered(p, fd)) {
        errno = EEXIST;
        return -1;
    }
    if ((size_t)fd >= p->cap) {
        size_t cap = p->cap == 0 ? 256 : p->cap;
        ep_item_t *items;
        while (cap <= (size_t)fd)
            cap *= 2;
        items = realloc(p->items, cap * sizeof(*items));
        if (items == NULL) {
            errno = ENOMEM;
            return -1;
        }
        memset(items + p->cap, 0, (cap - p->cap) * sizeof(*items));
        p->items = items;
        p->cap = cap;
    }
    ev.events = to_epoll(events);
    ev.data.fd = fd;
    if (epoll_ctl(p->fd, EPOLL_CTL_ADD, fd, &ev) != 0)
        return -1;
    p->items[fd] = (ep_item_t){true, ptr};
    return 0;
}

int poller_mod(poller_t *p, int fd, unsigned events, void *ptr)
{
    struct epoll_event ev;
    if (p == NULL || (events & ~3u) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (!registered(p, fd)) {
        errno = ENOENT;
        return -1;
    }
    ev.events = to_epoll(events);
    ev.data.fd = fd;
    if (epoll_ctl(p->fd, EPOLL_CTL_MOD, fd, &ev) != 0)
        return -1;
    p->items[fd].ptr = ptr;
    return 0;
}

int poller_del(poller_t *p, int fd)
{
    if (p == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (!registered(p, fd)) {
        errno = ENOENT;
        return -1;
    }
    /* 이미 닫힌 fd(EBADF)나 같은 번호로 새로 열린 fd(ENOENT)는 epoll에서 이미 빠졌다.
     * 기록은 지워야 같은 번호를 다시 등록할 수 있다. */
    p->items[fd] = (ep_item_t){false, NULL};
    if (epoll_ctl(p->fd, EPOLL_CTL_DEL, fd, NULL) != 0 && errno != EBADF && errno != ENOENT)
        return -1;
    return 0;
}

int poller_wait(poller_t *p, poller_event_t *out, int max, int timeout_ms)
{
    struct epoll_event *events;
    int n, i;
    if (p == NULL || out == NULL || max < 1 || timeout_ms < -1) {
        errno = EINVAL;
        return -1;
    }
    events = calloc((size_t)max, sizeof(*events));
    if (events == NULL)
        return -1;
    n = epoll_wait(p->fd, events, max, timeout_ms);
    if (n < 0) {
        int saved = errno;
        free(events);
        if (saved == EINTR)
            return 0;
        errno = saved;
        return -1;
    }
    for (i = 0; i < n; i++) {
        unsigned e = 0;
        int fd = events[i].data.fd;
        if (events[i].events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP))
            e |= 1u;
        if (events[i].events & EPOLLOUT)
            e |= 2u;
        if (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))
            e |= 4u;
        out[i] = (poller_event_t){fd, e, registered(p, fd) ? p->items[fd].ptr : NULL};
    }
    free(events);
    return n;
}

void poller_free(poller_t *p)
{
    int saved = errno;
    if (p != NULL) {
        (void)close(p->fd);
        free(p->items);
        free(p);
    }
    errno = saved;
}

const char *poller_backend_name(void)
{
    return "epoll";
}
#else
typedef int poller_epoll_unused_t;
#endif
