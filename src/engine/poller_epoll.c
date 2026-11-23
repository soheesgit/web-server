/* src/engine/poller_epoll.c
 *
 * Linux epoll poller (D-022). 명세: poller.h
 */
#if defined(__linux__) && !defined(HTTPD_FORCE_POLL)
#include "engine/poller.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <unistd.h>

typedef struct {
    int fd;
    void *ptr;
} ep_item_t;
struct poller {
    int fd;
    ep_item_t *items;
    size_t n;
    size_t cap;
};

static size_t index_of(const poller_t *p, int fd)
{
    size_t i;
    for (i = 0; i < p->n; i++)
        if (p->items[i].fd == fd)
            return i;
    return p->n;
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
    ep_item_t *items;
    size_t cap;
    if (p == NULL || fd < 0 || (events & ~3u) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (index_of(p, fd) != p->n) {
        errno = EEXIST;
        return -1;
    }
    if (p->n == p->cap) {
        cap = p->cap == 0 ? 16 : p->cap * 2;
        if (cap < p->cap || cap > SIZE_MAX / sizeof(*items)) {
            errno = ENOMEM;
            return -1;
        }
        items = realloc(p->items, cap * sizeof(*items));
        if (items == NULL) {
            errno = ENOMEM;
            return -1;
        }
        p->items = items;
        p->cap = cap;
    }
    ev.events = to_epoll(events);
    ev.data.fd = fd;
    if (epoll_ctl(p->fd, EPOLL_CTL_ADD, fd, &ev) != 0)
        return -1;
    p->items[p->n++] = (ep_item_t){fd, ptr};
    return 0;
}

int poller_mod(poller_t *p, int fd, unsigned events, void *ptr)
{
    struct epoll_event ev;
    size_t i;
    if (p == NULL || (events & ~3u) != 0) {
        errno = EINVAL;
        return -1;
    }
    i = index_of(p, fd);
    if (i == p->n) {
        errno = ENOENT;
        return -1;
    }
    ev.events = to_epoll(events);
    ev.data.fd = fd;
    if (epoll_ctl(p->fd, EPOLL_CTL_MOD, fd, &ev) != 0)
        return -1;
    p->items[i].ptr = ptr;
    return 0;
}

int poller_del(poller_t *p, int fd)
{
    size_t i;
    if (p == NULL) {
        errno = EINVAL;
        return -1;
    }
    i = index_of(p, fd);
    if (i == p->n) {
        errno = ENOENT;
        return -1;
    }
    if (epoll_ctl(p->fd, EPOLL_CTL_DEL, fd, NULL) != 0)
        return -1;
    if (i + 1 < p->n) {
        size_t j;
        for (j = i + 1; j < p->n; j++)
            p->items[j - 1] = p->items[j];
    }
    p->n--;
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
        size_t j = index_of(p, events[i].data.fd);
        if (events[i].events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP))
            e |= 1u;
        if (events[i].events & EPOLLOUT)
            e |= 2u;
        if (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))
            e |= 4u;
        out[i] = (poller_event_t){events[i].data.fd, e, j == p->n ? NULL : p->items[j].ptr};
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
