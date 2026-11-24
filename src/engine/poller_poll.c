/* 기본 POSIX poll과 HTTPD_FORCE_POLL 빌드 (D-022). */
#if (!defined(__linux__) && !defined(__APPLE__) && !defined(__FreeBSD__)) ||                       \
    defined(HTTPD_FORCE_POLL)
#include "engine/poller.h"

#include <fcntl.h>
#include <poll.h>

#include "engine/internal_poller.h"

struct poller {
    poll_table_t table;
    struct pollfd *ready;
    size_t ready_cap;
    nfds_t cursor;
};

poller_t *poller_new(void)
{
    return calloc(1, sizeof(poller_t));
}

int poller_add(poller_t *p, int fd, unsigned events, void *ptr)
{
    if (!poll_mask_valid(events)) {
        errno = EINVAL;
        return -1;
    }
    if (poll_lookup(&p->table, fd) != NULL) {
        errno = EEXIST;
        return -1;
    }
    if (fcntl(fd, F_GETFD) < 0 || poll_reserve(&p->table, fd) != 0) {
        return -1;
    }
    poll_store(&p->table, fd, events, ptr);
    return 0;
}

int poller_mod(poller_t *p, int fd, unsigned events, void *ptr)
{
    poll_slot_t *slot = poll_lookup(&p->table, fd);
    if (slot == NULL || !poll_mask_valid(events)) {
        errno = slot == NULL ? ENOENT : EINVAL;
        return -1;
    }
    poll_store(&p->table, fd, events, ptr);
    return 0;
}

int poller_del(poller_t *p, int fd)
{
    if (poll_lookup(&p->table, fd) == NULL) {
        errno = ENOENT;
        return -1;
    }
    poll_erase(&p->table, fd);
    return 0;
}

int poller_wait(poller_t *p, poller_event_t *out, int max, int timeout_ms)
{
    if (max < 1 || timeout_ms < -1) {
        errno = EINVAL;
        return -1;
    }
    if (p->table.cap > p->ready_cap) {
        struct pollfd *fds = realloc(p->ready, p->table.cap * sizeof(*fds));
        if (fds == NULL) {
            return -1;
        }
        p->ready = fds;
        p->ready_cap = p->table.cap;
    }
    nfds_t count = 0;
    for (size_t i = 0; i < p->table.end; i++) {
        unsigned mask = p->table.slots[i].events;
        if (mask != 0) {
            short events = (mask & POLLER_READ ? POLLIN : 0) | (mask & POLLER_WRITE ? POLLOUT : 0);
            p->ready[count++] = (struct pollfd){(int)i, events, 0};
        }
    }
    int rc = poll(p->ready, count, timeout_ms);
    if (rc <= 0) {
        return rc < 0 && errno == EINTR ? 0 : rc;
    }
    int n = 0;
    for (nfds_t i = 0; i < count && n < max; i++) {
        if (p->cursor >= count) {
            p->cursor = 0;
        }
        struct pollfd ev = p->ready[p->cursor++];
        unsigned mask = 0;
        if (ev.revents & POLLIN) {
            mask |= POLLER_READ;
        }
        if (ev.revents & POLLOUT) {
            mask |= POLLER_WRITE;
        }
        if (ev.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            mask |= POLLER_ERROR;
        }
        if (mask != 0) {
            out[n++] = (poller_event_t){ev.fd, mask, p->table.slots[ev.fd].ptr};
        }
    }
    return n;
}

void poller_free(poller_t *p)
{
    if (p != NULL) {
        int saved = errno;
        free(p->table.slots);
        free(p->ready);
        free(p);
        errno = saved;
    }
}

const char *poller_backend_name(void)
{
    return "poll";
}

#else
typedef int poller_poll_unused_t;
#endif
