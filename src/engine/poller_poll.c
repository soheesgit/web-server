/* src/engine/poller_poll.c
 *
 * poll(2) poller (그 밖의 OS 또는 -DHTTPD_FORCE_POLL). 명세: poller.h
 */
#if (!defined(__linux__) && !defined(__APPLE__) && !defined(__FreeBSD__)) || defined(HTTPD_FORCE_POLL)
#include "engine/poller.h"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    struct pollfd pfd;
    void *ptr;
} poll_item_t;

struct poller {
    poll_item_t *items;
    size_t n;
    size_t cap;
};

static size_t find_fd(const poller_t *p, int fd)
{
    size_t i;
    for (i = 0; i < p->n; i++) if (p->items[i].pfd.fd == fd) return i;
    return p->n;
}

poller_t *poller_new(void)
{
    return calloc(1, sizeof(poller_t));
}

int poller_add(poller_t *p, int fd, unsigned events, void *ptr)
{
    poll_item_t *items;
    size_t cap;
    if (p == NULL || fd < 0 || (events & ~3u) != 0) { errno = EINVAL; return -1; }
    if (find_fd(p, fd) != p->n) { errno = EEXIST; return -1; }
    if (p->n == p->cap) {
        cap = p->cap == 0 ? 16 : p->cap * 2;
        if (cap < p->cap || cap > SIZE_MAX / sizeof(*items)) { errno = ENOMEM; return -1; }
        items = realloc(p->items, cap * sizeof(*items));
        if (items == NULL) { errno = ENOMEM; return -1; }
        p->items = items;
        p->cap = cap;
    }
    p->items[p->n].pfd.fd = fd;
    p->items[p->n].pfd.events = (short)(((events & 1u) ? POLLIN : 0) | ((events & 2u) ? POLLOUT : 0));
    p->items[p->n].pfd.revents = 0;
    p->items[p->n].ptr = ptr;
    p->n++;
    return 0;
}

int poller_mod(poller_t *p, int fd, unsigned events, void *ptr)
{
    size_t i;
    if (p == NULL || (events & ~3u) != 0) { errno = EINVAL; return -1; }
    i = find_fd(p, fd);
    if (i == p->n) { errno = ENOENT; return -1; }
    p->items[i].pfd.events = (short)(((events & 1u) ? POLLIN : 0) | ((events & 2u) ? POLLOUT : 0));
    p->items[i].ptr = ptr;
    return 0;
}

int poller_del(poller_t *p, int fd)
{
    size_t i;
    if (p == NULL) { errno = EINVAL; return -1; }
    i = find_fd(p, fd);
    if (i == p->n) { errno = ENOENT; return -1; }
    if (i + 1 < p->n) memmove(&p->items[i], &p->items[i + 1], (p->n - i - 1) * sizeof(*p->items));
    p->n--;
    return 0;
}

int poller_wait(poller_t *p, poller_event_t *out, int max, int timeout_ms)
{
    struct pollfd *fds;
    size_t i;
    int n, count = 0;
    if (p == NULL || out == NULL || max < 1) { errno = EINVAL; return -1; }
    fds = calloc(p->n == 0 ? 1 : p->n, sizeof(*fds));
    if (fds == NULL) return -1;
    for (i = 0; i < p->n; i++) fds[i] = p->items[i].pfd;
    n = poll(fds, p->n, timeout_ms);
    if (n < 0) {
        int saved = errno;
        free(fds);
        if (saved == EINTR) return 0;
        errno = saved;
        return -1;
    }
    for (i = 0; i < p->n && count < max; i++) {
        unsigned events = 0;
        if (fds[i].revents == 0) continue;
        if (fds[i].revents & (POLLIN | POLLHUP)) events |= 1u;
        if (fds[i].revents & POLLOUT) events |= 2u;
        if (fds[i].revents & (POLLERR | POLLNVAL | POLLHUP)) events |= 4u;
        out[count++] = (poller_event_t){p->items[i].pfd.fd, events, p->items[i].ptr};
    }
    free(fds);
    return count;
}

void poller_free(poller_t *p)
{
    int saved = errno;
    if (p != NULL) { free(p->items); free(p); }
    errno = saved;
}

const char *poller_backend_name(void)
{
    return "poll";
}
#else
typedef int poller_poll_unused_t;
#endif
