/* src/engine/poller_epoll.c
 *
 * Linux epoll poller (D-022: 빌드 대상에 맞는 파일 하나만 심볼을 정의). 명세: poller.h
 */
#if defined(__linux__) && !defined(HTTPD_FORCE_POLL)
#include "engine/poller.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

enum { P_READ = 1u, P_WRITE = 2u, P_ERROR = 4u };

typedef struct {
    bool used;
    void *ptr;
} slot_t;

struct poller {
    int ep;
    slot_t *slots; /* fd로 인덱스 */
    size_t nslots;
    struct epoll_event *evbuf;
    int evcap;
};

poller_t *poller_new(void)
{
    poller_t *p = calloc(1, sizeof(*p));

    if (p == NULL) {
        return NULL;
    }
    p->ep = epoll_create1(EPOLL_CLOEXEC);
    if (p->ep < 0) {
        int e = errno;

        free(p);
        errno = e;
        return NULL;
    }
    return p;
}

static slot_t *slot_get(poller_t *p, int fd, bool grow)
{
    if (fd < 0) {
        errno = EBADF;
        return NULL;
    }
    if ((size_t)fd >= p->nslots) {
        size_t n = p->nslots == 0 ? 64 : p->nslots;
        slot_t *s;

        if (!grow) {
            errno = ENOENT;
            return NULL;
        }
        while (n <= (size_t)fd) {
            n *= 2;
        }
        s = realloc(p->slots, n * sizeof(*s));
        if (s == NULL) {
            return NULL;
        }
        memset(s + p->nslots, 0, (n - p->nslots) * sizeof(*s));
        p->slots = s;
        p->nslots = n;
    }
    return &p->slots[fd];
}

static int ctl(poller_t *p, int op, int fd, unsigned events)
{
    struct epoll_event ev;

    memset(&ev, 0, sizeof(ev));
    ev.events = ((events & P_READ) ? EPOLLIN : 0u) | ((events & P_WRITE) ? EPOLLOUT : 0u);
    ev.data.fd = fd;
    return epoll_ctl(p->ep, op, fd, &ev);
}

int poller_add(poller_t *p, int fd, unsigned events, void *ptr)
{
    slot_t *s = slot_get(p, fd, true);

    if (s == NULL) {
        return -1;
    }
    if (s->used) {
        errno = EEXIST;
        return -1;
    }
    if (ctl(p, EPOLL_CTL_ADD, fd, events) != 0) {
        return -1;
    }
    s->used = true;
    s->ptr = ptr;
    return 0;
}

int poller_mod(poller_t *p, int fd, unsigned events, void *ptr)
{
    slot_t *s = slot_get(p, fd, false);

    if (s == NULL || !s->used) {
        errno = ENOENT;
        return -1;
    }
    if (ctl(p, EPOLL_CTL_MOD, fd, events) != 0) {
        return -1;
    }
    s->ptr = ptr;
    return 0;
}

int poller_del(poller_t *p, int fd)
{
    slot_t *s = slot_get(p, fd, false);

    if (s == NULL || !s->used) {
        errno = ENOENT;
        return -1;
    }
    /* fd가 이미 닫혔으면 커널이 지웠으므로 실패해도 등록 표만 비운다 */
    (void)ctl(p, EPOLL_CTL_DEL, fd, 0);
    memset(s, 0, sizeof(*s));
    return 0;
}

int poller_wait(poller_t *p, poller_event_t *out, int max, int timeout_ms)
{
    int n, nout = 0;

    if (max < 1) {
        errno = EINVAL;
        return -1;
    }
    if (p->evcap < max) {
        struct epoll_event *b = realloc(p->evbuf, (size_t)max * sizeof(*b));

        if (b == NULL) {
            return -1;
        }
        p->evbuf = b;
        p->evcap = max;
    }
    n = epoll_wait(p->ep, p->evbuf, max, timeout_ms);
    if (n < 0) {
        return errno == EINTR ? 0 : -1;
    }
    for (int i = 0; i < n; i++) {
        unsigned e = p->evbuf[i].events;
        int fd = p->evbuf[i].data.fd;
        slot_t *s = slot_get(p, fd, false);

        if (s == NULL || !s->used) {
            continue;
        }
        out[nout].fd = fd;
        out[nout].events = ((e & EPOLLIN) ? P_READ : 0u) | ((e & EPOLLOUT) ? P_WRITE : 0u) |
                           ((e & (EPOLLERR | EPOLLHUP)) ? P_ERROR : 0u);
        out[nout].ptr = s->ptr;
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
    close(p->ep);
    free(p->slots);
    free(p->evbuf);
    free(p);
    errno = e;
}

const char *poller_backend_name(void)
{
    return "epoll";
}

#else
/* 이 플랫폼에서는 다른 poller 파일이 쓰인다. 빈 번역 단위 경고를 피하기 위한 선언. */
typedef int poller_epoll_unused_t;
#endif
