/* macOS·FreeBSD kqueue. EV_ERROR는 시스템 매크로다 (D-021). */
#if (defined(__APPLE__) || defined(__FreeBSD__)) && !defined(HTTPD_FORCE_POLL)
#include "engine/poller.h"

#include <fcntl.h>
#include <sys/event.h>
#include <time.h>
#include <unistd.h>

#include "core/platform.h"
#include "engine/internal_poller.h"

struct poller {
    int fd;
    poll_table_t table;
    struct kevent *ready;
    int ready_cap;
};

poller_t *poller_new(void)
{
    poller_t *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        return NULL;
    }
    p->fd = kqueue();
    if (p->fd < 0 || platform_set_cloexec(p->fd) != 0) {
        poller_free(p);
        return NULL;
    }
    return p;
}

static int change_filter(poller_t *p, int fd, short filter, bool enable)
{
    struct kevent ev;
    EV_SET(&ev, (uintptr_t)fd, filter, enable ? EV_ADD : EV_DELETE, 0, 0, NULL);
    int rc;
    do {
        rc = kevent(p->fd, &ev, 1, NULL, 0, NULL);
    } while (rc < 0 && errno == EINTR);
    return rc;
}

static int change(poller_t *p, int fd, unsigned before, unsigned after)
{
    unsigned diff = before ^ after;
    if ((diff & POLLER_READ) &&
        change_filter(p, fd, EVFILT_READ, (after & POLLER_READ) != 0) != 0) {
        return -1;
    }
    if ((diff & POLLER_WRITE) &&
        change_filter(p, fd, EVFILT_WRITE, (after & POLLER_WRITE) != 0) != 0) {
        int saved = errno;
        if (diff & POLLER_READ) {
            (void)change_filter(p, fd, EVFILT_READ, (before & POLLER_READ) != 0);
        }
        errno = saved;
        return -1;
    }
    return 0;
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
    if (change(p, fd, slot->events, events) != 0) {
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
        struct kevent *events = realloc(p->ready, (size_t)max * sizeof(*events));
        if (events == NULL) {
            return -1;
        }
        p->ready = events;
        p->ready_cap = max;
    }
    struct timespec timeout = {timeout_ms / 1000, (timeout_ms % 1000) * 1000000L};
    int rc = kevent(p->fd, NULL, 0, p->ready, max, timeout_ms < 0 ? NULL : &timeout);
    if (rc < 0) {
        return errno == EINTR ? 0 : -1;
    }
    int n = 0;
    for (int i = 0; i < rc; i++) {
        const struct kevent *event = &p->ready[i];
        int fd = (int)event->ident;
        poll_slot_t *slot = poll_lookup(&p->table, fd);
        if (slot == NULL || slot->events == 0) {
            continue;
        }
        unsigned mask = event->filter == EVFILT_READ ? POLLER_READ : POLLER_WRITE;
        if ((event->flags & EV_ERROR) ||
            ((event->flags & EV_EOF) && (event->filter == EVFILT_WRITE || event->fflags))) {
            mask |= POLLER_ERROR;
        }
        int idx = 0;
        while (idx < n && out[idx].fd != fd) {
            idx++;
        }
        if (idx == n) {
            out[n++] = (poller_event_t){fd, mask, slot->ptr};
        } else {
            out[idx].events |= mask;
        }
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
    return "kqueue";
}

#else
typedef int poller_kqueue_unused_t;
#endif
