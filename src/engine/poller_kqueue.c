/* src/engine/poller_kqueue.c
 *
 * macOS·FreeBSD kqueue poller (D-022: 빌드 대상에 맞는 파일 하나만 심볼을 정의). 명세: poller.h
 */
#if (defined(__APPLE__) || defined(__FreeBSD__)) && !defined(HTTPD_FORCE_POLL)
#include "engine/poller.h"

#include "core/platform.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* <sys/event.h>와 겹치지 않는 이름으로 poller.h의 비트 값을 쓴다 (D-021). */
enum { P_READ = 1u, P_WRITE = 2u, P_ERROR = 4u };

typedef struct {
    bool used;
    unsigned events; /* 지금 kqueue에 걸어 둔 필터 (P_READ·P_WRITE) */
    void *ptr;
    int out; /* poller_wait 결과에서 이 fd의 위치 + 1. 0이면 아직 없음 */
} slot_t;

struct poller {
    int kq;
    slot_t *slots; /* fd로 인덱스 */
    size_t nslots;
    struct kevent *evbuf;
    int evcap;
};

poller_t *poller_new(void)
{
    poller_t *p = calloc(1, sizeof(*p));

    if (p == NULL) {
        return NULL;
    }
    p->kq = kqueue();
    if (p->kq < 0 || platform_set_cloexec(p->kq) != 0) {
        int e = errno;

        if (p->kq >= 0) {
            close(p->kq);
        }
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

/* 걸어 둔 필터를 want로 맞춘다. */
static int apply(poller_t *p, int fd, slot_t *s, unsigned want)
{
    struct kevent ch[2];
    int n = 0;

    if ((want & P_READ) != (s->events & P_READ)) {
        EV_SET(&ch[n++], (uintptr_t)fd, EVFILT_READ, (want & P_READ) ? EV_ADD : EV_DELETE, 0, 0,
               NULL);
    }
    if ((want & P_WRITE) != (s->events & P_WRITE)) {
        EV_SET(&ch[n++], (uintptr_t)fd, EVFILT_WRITE, (want & P_WRITE) ? EV_ADD : EV_DELETE, 0, 0,
               NULL);
    }
    if (n > 0 && kevent(p->kq, ch, n, NULL, 0, NULL) != 0) {
        return -1;
    }
    s->events = want & (P_READ | P_WRITE);
    return 0;
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
    s->events = 0;
    if (apply(p, fd, s, events) != 0) {
        return -1;
    }
    s->used = true;
    s->ptr = ptr;
    s->out = 0;
    return 0;
}

int poller_mod(poller_t *p, int fd, unsigned events, void *ptr)
{
    slot_t *s = slot_get(p, fd, false);

    if (s == NULL || !s->used) {
        errno = ENOENT;
        return -1;
    }
    if (apply(p, fd, s, events) != 0) {
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
    /* fd가 이미 닫혔으면 커널이 필터를 지웠으므로 실패해도 등록 표만 비운다 */
    (void)apply(p, fd, s, 0);
    memset(s, 0, sizeof(*s));
    return 0;
}

int poller_wait(poller_t *p, poller_event_t *out, int max, int timeout_ms)
{
    struct timespec ts;
    int n, nout = 0;

    if (max < 1) {
        errno = EINVAL;
        return -1;
    }
    /* 한 fd가 읽기·쓰기 두 항목으로 올 수 있어 두 배를 받는다 */
    if (p->evcap < max * 2) {
        struct kevent *b = realloc(p->evbuf, (size_t)max * 2 * sizeof(*b));

        if (b == NULL) {
            return -1;
        }
        p->evbuf = b;
        p->evcap = max * 2;
    }
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
    n = kevent(p->kq, NULL, 0, p->evbuf, p->evcap, timeout_ms < 0 ? NULL : &ts);
    if (n < 0) {
        return errno == EINTR ? 0 : -1;
    }
    for (int i = 0; i < n; i++) {
        const struct kevent *k = &p->evbuf[i];
        int fd = (int)k->ident;
        slot_t *s = slot_get(p, fd, false);
        unsigned bits;

        if (s == NULL || !s->used) {
            continue; /* 이미 지운 fd */
        }
        if (k->flags & EV_ERROR) {
            bits = P_ERROR;
        } else if (k->filter == EVFILT_READ) {
            bits = P_READ; /* EOF도 읽기 가능으로 보고한다 (read가 0을 돌려줌) */
        } else {
            bits = P_WRITE | ((k->flags & EV_EOF) ? P_ERROR : 0u);
        }
        if (s->out > 0) {
            out[s->out - 1].events |= bits;
            continue;
        }
        if (nout == max) {
            continue; /* 레벨 트리거라 다음 wait에서 다시 온다 */
        }
        out[nout].fd = fd;
        out[nout].events = bits;
        out[nout].ptr = s->ptr;
        s->out = ++nout;
    }
    for (int i = 0; i < nout; i++) {
        p->slots[out[i].fd].out = 0;
    }
    return nout;
}

void poller_free(poller_t *p)
{
    int e = errno;

    if (p == NULL) {
        return;
    }
    close(p->kq);
    free(p->slots);
    free(p->evbuf);
    free(p);
    errno = e;
}

const char *poller_backend_name(void)
{
    return "kqueue";
}

#else
/* 이 플랫폼에서는 다른 poller 파일이 쓰인다. 빈 번역 단위 경고를 피하기 위한 선언. */
typedef int poller_kqueue_unused_t;
#endif
