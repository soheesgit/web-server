/* Linux epoll. 빌드 대상 선택은 D-022를 따른다. */
#if defined(__linux__) && !defined(HTTPD_FORCE_POLL)
#include "engine/poller.h"

#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "engine/internal_poller.h"

struct poller {
    int fd;
    poll_table_t table;
    struct epoll_event *ready;
    int ready_cap;
};

poller_t *poller_new(void)
{
    poller_t *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        return NULL;
    }
    p->fd = epoll_create1(EPOLL_CLOEXEC);
    if (p->fd < 0) {
        poller_free(p);
        return NULL;
    }
    return p;
}

static int change(poller_t *p, int fd, unsigned before, unsigned after)
{
    if (before == 0 && after == 0) {
        return 0;
    }
    struct epoll_event event = {.data.fd = fd};
    if (after & POLLER_READ) {
        event.events |= EPOLLIN | EPOLLRDHUP;
    }
    if (after & POLLER_WRITE) {
        event.events |= EPOLLOUT;
    }
    int op = before == 0 ? EPOLL_CTL_ADD : after == 0 ? EPOLL_CTL_DEL : EPOLL_CTL_MOD;
    return epoll_ctl(p->fd, op, fd, &event);
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
    if ((events == 0 && fcntl(fd, F_GETFD) < 0) || poll_reserve(&p->table, fd) != 0 ||
        change(p, fd, 0, events) != 0) {
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
    if (slot->events != events && change(p, fd, slot->events, events) != 0) {
        return -1;
    }
    poll_store(&p->table, fd, events, ptr);
    return 0;
}

int poller_del(poller_t *p, int fd)
{
    poll_slot_t *slot = poll_lookup(&p->table, fd);
    if (slot == NULL) {
        errno = ENOENT;
        return -1;
    }
    if (change(p, fd, slot->events, 0) != 0) {
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
    if (max > p->ready_cap) {
        struct epoll_event *events = realloc(p->ready, (size_t)max * sizeof(*events));
        if (events == NULL) {
            return -1;
        }
        p->ready = events;
        p->ready_cap = max;
    }
    int rc = epoll_wait(p->fd, p->ready, max, timeout_ms);
    if (rc < 0) {
        return errno == EINTR ? 0 : -1;
    }
    int n = 0;
    for (int i = 0; i < rc; i++) {
        int fd = p->ready[i].data.fd;
        poll_slot_t *slot = poll_lookup(&p->table, fd);
        if (slot == NULL || slot->events == 0) {
            continue;
        }
        uint32_t ev = p->ready[i].events;
        unsigned mask = 0;
        if (ev & (EPOLLIN | EPOLLRDHUP)) {
            mask |= POLLER_READ;
        }
        if (ev & EPOLLOUT) {
            mask |= POLLER_WRITE;
        }
        if (ev & (EPOLLERR | EPOLLHUP)) {
            mask |= POLLER_ERROR;
        }
        out[n++] = (poller_event_t){fd, mask, slot->ptr};
    }
    return n;
}

void poller_free(poller_t *p)
{
    if (p != NULL) {
        int saved = errno;
        if (p->fd >= 0) {
            close(p->fd);
        }
        free(p->table.slots);
        free(p->ready);
        free(p);
        errno = saved;
    }
}

const char *poller_backend_name(void)
{
    return "epoll";
}

#else
typedef int poller_epoll_unused_t;
#endif
