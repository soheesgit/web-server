/* src/engine/poller_kqueue.c
 *
 * macOS·FreeBSD kqueue poller (D-022). 명세: poller.h
 */
#if (defined(__APPLE__) || defined(__FreeBSD__)) && !defined(HTTPD_FORCE_POLL)
#include "engine/poller.h"

#include "core/platform.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/event.h>
#include <unistd.h>

typedef struct {
    int fd;
    void *ptr;
} kq_item_t;
struct poller {
    int fd;
    kq_item_t *items;
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

static void set_change(struct kevent *ev, int fd, int16_t filter, int enabled, void *ptr)
{
    EV_SET(ev, (uintptr_t)fd, filter, EV_ADD | (enabled ? EV_ENABLE : EV_DISABLE), 0, 0, ptr);
}

poller_t *poller_new(void)
{
    poller_t *p = calloc(1, sizeof(*p));
    if (p == NULL)
        return NULL;
    p->fd = kqueue();
    if (p->fd < 0 || platform_set_cloexec(p->fd) != 0) {
        int saved = errno;
        if (p->fd >= 0)
            (void)close(p->fd);
        free(p);
        errno = saved;
        return NULL;
    }
    return p;
}

int poller_add(poller_t *p, int fd, unsigned events, void *ptr)
{
    struct kevent changes[2];
    kq_item_t *items;
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
        if (items == NULL)
            return -1;
        p->items = items;
        p->cap = cap;
    }
    set_change(&changes[0], fd, EVFILT_READ, (events & 1u) != 0, ptr);
    set_change(&changes[1], fd, EVFILT_WRITE, (events & 2u) != 0, ptr);
    if (kevent(p->fd, changes, 2, NULL, 0, NULL) < 0)
        return -1;
    p->items[p->n++] = (kq_item_t){fd, ptr};
    return 0;
}

int poller_mod(poller_t *p, int fd, unsigned events, void *ptr)
{
    struct kevent changes[2];
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
    set_change(&changes[0], fd, EVFILT_READ, (events & 1u) != 0, ptr);
    set_change(&changes[1], fd, EVFILT_WRITE, (events & 2u) != 0, ptr);
    if (kevent(p->fd, changes, 2, NULL, 0, NULL) < 0)
        return -1;
    p->items[i].ptr = ptr;
    return 0;
}

int poller_del(poller_t *p, int fd)
{
    struct kevent changes[2];
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
    EV_SET(&changes[0], (uintptr_t)fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&changes[1], (uintptr_t)fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    if (kevent(p->fd, changes, 2, NULL, 0, NULL) < 0 && errno != ENOENT && errno != EBADF)
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
    struct kevent *events;
    struct timespec ts, *tsp = NULL;
    int n, i, count = 0;
    if (p == NULL || out == NULL || max < 1 || timeout_ms < -1) {
        errno = EINVAL;
        return -1;
    }
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        tsp = &ts;
    }
    events = calloc((size_t)max, sizeof(*events));
    if (events == NULL)
        return -1;
    n = kevent(p->fd, NULL, 0, events, max, tsp);
    if (n < 0) {
        int saved = errno;
        free(events);
        if (saved == EINTR)
            return 0;
        errno = saved;
        return -1;
    }
    for (i = 0; i < n; i++) {
        int fd = (int)events[i].ident;
        size_t j;
        unsigned flags = 0;
        for (j = 0; j < (size_t)count; j++)
            if (out[j].fd == fd)
                break;
        if (events[i].filter == EVFILT_READ || (events[i].flags & EV_EOF) != 0)
            flags |= 1u;
        if (events[i].filter == EVFILT_WRITE)
            flags |= 2u;
        if ((events[i].flags & (EV_ERROR | EV_EOF)) != 0)
            flags |= 4u;
        if (j == (size_t)count) {
            size_t idx = index_of(p, fd);
            if (count >= max)
                continue;
            out[count++] = (poller_event_t){fd, flags, idx == p->n ? NULL : p->items[idx].ptr};
        } else
            out[j].events |= flags;
    }
    free(events);
    return count;
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
    return "kqueue";
}
#else
typedef int poller_kqueue_unused_t;
#endif
