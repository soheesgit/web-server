/* src/engine/engine_loop.c
 *
 * v2 이벤트 루프와 추가 fd 등록. 명세: engine.h
 *
 * 연결마다 기록(rec) 하나를 둔다. poller의 ptr에는 기록 번호 + 1을 넣고(0은 대기 소켓),
 * 마감은 최소 힙으로 관리한다(마감이 바뀌면 새 항목을 넣고, 꺼낼 때 낡은 항목은 버린다).
 */
#include "engine/engine.h"

#include "core/log.h"
#include "core/platform.h"
#include "engine/internal_loop.h"
#include "engine/poller.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define LOOP_MAX_EVENTS 256
#define LOOP_STOP_POLL_MS 100 /* stop 플래그 확인 주기 */
#define LOOP_ACCEPT_BATCH 64 /* 한 번에 받는 연결 수 상한 (다른 연결을 굶기지 않게) */
#define LOOP_REC_FDS 4 /* 연결 하나가 감시하는 fd 상한 (클라이언트 + pipe 2 + 업스트림) */

typedef struct {
    bool alive;
    void *obj;
    int fds[LOOP_REC_FDS]; /* 감시 중인 fd, 빈 칸 -1 */
    int64_t deadline_ms;   /* 마지막으로 힙에 넣은 마감, 없음 -1 */
    int next_free;         /* 빈 기록 목록 */
} rec_t;

typedef struct {
    int64_t at;
    int id;
} timer_t_;

struct loop {
    const engine_callbacks_t *cb;
    void *ctx;
    poller_t *poller;
    rec_t *recs;
    int nrecs, caprecs, free_head, live;
    int cur; /* 지금 콜백 중인 기록, 없으면 -1 (engine_watch의 obj → 기록 찾기) */
    timer_t_ *heap;
    size_t nheap, capheap;
    int *fd_rec; /* fd → 감시하는 기록 번호 + 1, 0이면 없음 */
    size_t nfd_rec;
};

static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};

    (void)nanosleep(&ts, NULL);
}

static void *id_ptr(int id)
{
    return (void *)(uintptr_t)(id + 1);
}

static int ptr_id(void *p)
{
    return (int)(uintptr_t)p - 1;
}

/* ── 마감 힙 ────────────────────────────────────────────────────────────── */

static int heap_push(loop_t *l, int64_t at, int id)
{
    size_t i;

    if (l->nheap == l->capheap) {
        size_t cap = l->capheap == 0 ? 256 : l->capheap * 2;
        timer_t_ *h = realloc(l->heap, cap * sizeof(*h));

        if (h == NULL) {
            return -1;
        }
        l->heap = h;
        l->capheap = cap;
    }
    i = l->nheap++;
    while (i > 0 && l->heap[(i - 1) / 2].at > at) {
        l->heap[i] = l->heap[(i - 1) / 2];
        i = (i - 1) / 2;
    }
    l->heap[i] = (timer_t_){at, id};
    return 0;
}

static timer_t_ heap_pop(loop_t *l)
{
    timer_t_ top = l->heap[0];
    timer_t_ last = l->heap[--l->nheap];
    size_t i = 0;

    for (;;) {
        size_t c = i * 2 + 1;

        if (c >= l->nheap) {
            break;
        }
        if (c + 1 < l->nheap && l->heap[c + 1].at < l->heap[c].at) {
            c++;
        }
        if (l->heap[c].at >= last.at) {
            break;
        }
        l->heap[i] = l->heap[c];
        i = c;
    }
    if (l->nheap > 0) {
        l->heap[i] = last;
    }
    return top;
}

/* 낡은 항목(닫힌 연결, 바뀐 마감)을 맨 위에서 걷어낸다. */
static void heap_trim(loop_t *l)
{
    while (l->nheap > 0) {
        const rec_t *r = &l->recs[l->heap[0].id];

        if (r->alive && r->deadline_ms == l->heap[0].at) {
            return;
        }
        (void)heap_pop(l);
    }
}

/* ── 연결 기록 ──────────────────────────────────────────────────────────── */

static int rec_new(loop_t *l, void *obj)
{
    int id;
    rec_t *r;

    if (l->free_head >= 0) {
        id = l->free_head;
        l->free_head = l->recs[id].next_free;
    } else {
        if (l->nrecs == l->caprecs) {
            int cap = l->caprecs == 0 ? 64 : l->caprecs * 2;
            rec_t *rs = realloc(l->recs, (size_t)cap * sizeof(*rs));

            if (rs == NULL) {
                return -1;
            }
            l->recs = rs;
            l->caprecs = cap;
        }
        id = l->nrecs++;
    }
    r = &l->recs[id];
    memset(r, 0, sizeof(*r));
    r->alive = true;
    r->obj = obj;
    r->deadline_ms = -1;
    for (int i = 0; i < LOOP_REC_FDS; i++) {
        r->fds[i] = -1;
    }
    l->live++;
    return id;
}

static int rec_find_obj(const loop_t *l, const void *obj)
{
    if (l->cur >= 0 && l->recs[l->cur].alive && l->recs[l->cur].obj == obj) {
        return l->cur;
    }
    for (int i = 0; i < l->nrecs; i++) {
        if (l->recs[i].alive && l->recs[i].obj == obj) {
            return i;
        }
    }
    return -1;
}

/* fd를 감시하는 기록과 그 칸. 없으면 -1. */
static int rec_find_fd(const loop_t *l, int fd, int *out_slot)
{
    int id;

    if (fd < 0 || (size_t)fd >= l->nfd_rec || l->fd_rec[fd] == 0) {
        return -1;
    }
    id = l->fd_rec[fd] - 1;
    for (int k = 0; k < LOOP_REC_FDS; k++) {
        if (l->recs[id].fds[k] == fd) {
            *out_slot = k;
            return id;
        }
    }
    return -1;
}

/* 기록 id의 slot 칸에 fd를 붙이거나(id ≥ 0) 뗀다(id < 0). */
static int fd_bind(loop_t *l, int fd, int id, int slot)
{
    if ((size_t)fd >= l->nfd_rec) {
        size_t n = l->nfd_rec == 0 ? 256 : l->nfd_rec;
        int *t;

        while (n <= (size_t)fd) {
            n *= 2;
        }
        t = realloc(l->fd_rec, n * sizeof(*t));
        if (t == NULL) {
            return -1;
        }
        memset(t + l->nfd_rec, 0, (n - l->nfd_rec) * sizeof(*t));
        l->fd_rec = t;
        l->nfd_rec = n;
    }
    l->fd_rec[fd] = id + 1;
    l->recs[id].fds[slot] = fd;
    return 0;
}

static void fd_unbind(loop_t *l, int id, int slot)
{
    int fd = l->recs[id].fds[slot];

    if (fd >= 0) {
        (void)poller_del(l->poller, fd);
        l->fd_rec[fd] = 0;
        l->recs[id].fds[slot] = -1;
    }
}

/* 콜백 뒤 객체의 다음 마감을 반영한다. */
static void rec_update_deadline(loop_t *l, int id)
{
    rec_t *r = &l->recs[id];
    int64_t d = l->cb->deadline_ms(l->ctx, r->obj);

    if (d == r->deadline_ms) {
        return;
    }
    r->deadline_ms = d;
    if (d >= 0 && heap_push(l, d, id) != 0) {
        log_msg(LOG_ERROR, "event loop: cannot track deadline (out of memory)");
    }
}

/* 감시를 모두 끊고 on_close한다. */
static void rec_close(loop_t *l, int id)
{
    rec_t *r = &l->recs[id];
    int saved = l->cur;

    fd_unbind(l, id, 0); /* 클라이언트 fd */
    l->cur = id;
    l->cb->on_close(l->ctx, r->obj); /* 객체가 추가 fd를 engine_unwatch한다 */
    l->cur = saved;
    for (int k = 0; k < LOOP_REC_FDS; k++) {
        fd_unbind(l, id, k); /* 객체가 놓친 것 */
    }
    r->alive = false;
    r->obj = NULL;
    r->next_free = l->free_head;
    l->free_head = id;
    l->live--;
}

/* ── 이벤트 처리 ────────────────────────────────────────────────────────── */

static void dispatch(loop_t *l, const poller_event_t *ev)
{
    int id = ptr_id(ev->ptr);
    rec_t *r;
    int rc;

    if (id < 0 || id >= l->nrecs || !l->recs[id].alive) {
        return; /* 이번 배치에서 이미 닫힌 연결 */
    }
    r = &l->recs[id];
    l->cur = id;
    rc = l->cb->on_event(l->ctx, r->obj, ev->fd, ev->events, platform_now_ms());
    l->cur = -1;
    if (rc != 0) {
        rec_close(l, id);
        return;
    }
    rec_update_deadline(l, id);
}

static void fire_timers(loop_t *l, int64_t now)
{
    heap_trim(l);
    while (l->nheap > 0 && l->heap[0].at <= now) {
        timer_t_ t = heap_pop(l);
        rec_t *r = &l->recs[t.id];
        int rc;

        r->deadline_ms = -1; /* 이 마감은 소비했다. 객체가 새 마감을 알려 준다 */
        l->cur = t.id;
        rc = l->cb->on_timeout(l->ctx, r->obj, now);
        l->cur = -1;
        if (rc != 0) {
            rec_close(l, t.id);
        } else {
            rec_update_deadline(l, t.id);
        }
        heap_trim(l);
    }
}

static void accept_ready(loop_t *l, int lfd)
{
    for (int n = 0; n < LOOP_ACCEPT_BATCH; n++) {
        struct sockaddr_storage peer;
        socklen_t len = sizeof(peer);
        int fd = accept(lfd, (struct sockaddr *)&peer, &len);
        void *obj;
        int id;

        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            if (errno == EMFILE || errno == ENFILE) {
                log_msg(LOG_WARN, "accept: out of file descriptors");
                sleep_ms(10);
            }
            return;
        }
        if (platform_set_cloexec(fd) != 0) {
            close(fd);
            continue;
        }
        obj = l->cb->on_accept(l->ctx, fd, &peer, platform_now_ms());
        if (obj == NULL) {
            close(fd);
            continue;
        }
        id = rec_new(l, obj);
        if (id < 0 || fd_bind(l, fd, id, 0) != 0 ||
            poller_add(l->poller, fd, EV_READ, id_ptr(id)) != 0) {
            l->cb->on_close(l->ctx, obj); /* fd는 객체가 닫는다 */
            if (id >= 0) {
                if ((size_t)fd < l->nfd_rec) {
                    l->fd_rec[fd] = 0;
                }
                l->recs[id].alive = false;
                l->recs[id].next_free = l->free_head;
                l->free_head = id;
                l->live--;
            }
            continue;
        }
        rec_update_deadline(l, id);
    }
}

/* 다음 wait의 대기 시간: 가장 이른 마감까지, 길어도 stop 확인 주기. */
static int wait_ms(loop_t *l, int64_t now)
{
    heap_trim(l);
    if (l->nheap > 0) {
        int64_t d = l->heap[0].at - now;

        if (d < LOOP_STOP_POLL_MS) {
            return d < 0 ? 0 : (int)d;
        }
    }
    return LOOP_STOP_POLL_MS;
}

/* ── internal_loop.h ───────────────────────────────────────────────────── */

loop_t *loop_new(const engine_callbacks_t *cb, void *ctx)
{
    loop_t *l = calloc(1, sizeof(*l));

    if (l == NULL) {
        return NULL;
    }
    l->poller = poller_new();
    if (l->poller == NULL) {
        int e = errno;

        free(l);
        errno = e;
        return NULL;
    }
    l->cb = cb;
    l->ctx = ctx;
    l->free_head = -1;
    l->cur = -1;
    return l;
}

int loop_run(loop_t *l, int listen_fd, volatile sig_atomic_t *stop, int shutdown_grace_ms)
{
    poller_event_t evs[LOOP_MAX_EVENTS];
    bool listening = true;
    int64_t grace_end = -1;

    if (poller_add(l->poller, listen_fd, EV_READ, NULL) != 0) {
        return -1;
    }
    for (;;) {
        int64_t now = platform_now_ms();
        bool accept_now = false;
        int n;

        if (*stop && listening) {
            (void)poller_del(l->poller, listen_fd); /* 새 연결은 받지 않는다 */
            listening = false;
            grace_end = now + shutdown_grace_ms;
        }
        if (!listening && (l->live == 0 || now >= grace_end)) {
            break;
        }
        n = poller_wait(l->poller, evs, LOOP_MAX_EVENTS, wait_ms(l, now));
        if (n < 0) {
            return -1;
        }
        for (int i = 0; i < n; i++) {
            if (evs[i].ptr == NULL) {
                accept_now = true; /* 배치 끝에: 닫힌 fd 번호가 새 연결에 재사용될 수 있다 */
                continue;
            }
            dispatch(l, &evs[i]);
        }
        fire_timers(l, platform_now_ms());
        if (accept_now && listening) {
            accept_ready(l, listen_fd);
        }
    }
    for (int id = 0; id < l->nrecs; id++) {
        if (l->recs[id].alive) {
            rec_close(l, id); /* 유예가 지났다 */
        }
    }
    return 0;
}

void loop_free(loop_t *l)
{
    int e = errno;

    if (l == NULL) {
        return;
    }
    for (int id = 0; id < l->nrecs; id++) {
        if (l->recs[id].alive) {
            rec_close(l, id);
        }
    }
    poller_free(l->poller);
    free(l->recs);
    free(l->heap);
    free(l->fd_rec);
    free(l);
    errno = e;
}

/* ── engine.h ──────────────────────────────────────────────────────────── */

int engine_watch(engine_t *e, int fd, unsigned events, void *obj)
{
    loop_t *l = engine_loop_of(e);
    int slot = -1;
    int owner;
    int id;
    rec_t *r;

    if (l == NULL) {
        errno = ENOTSUP;
        return -1;
    }
    owner = rec_find_fd(l, fd, &slot);

    id = rec_find_obj(l, obj);
    if (id < 0) {
        errno = ENOENT;
        return -1;
    }
    if (owner == id) {
        return poller_mod(l->poller, fd, events, id_ptr(id));
    }
    if (owner >= 0) {
        fd_unbind(l, owner, slot); /* 다른 객체로 옮긴다 */
    }
    r = &l->recs[id];
    for (slot = 1; slot < LOOP_REC_FDS && r->fds[slot] >= 0; slot++) {
    }
    if (slot == LOOP_REC_FDS) {
        errno = ENOSPC;
        return -1;
    }
    if (fd_bind(l, fd, id, slot) != 0) {
        return -1;
    }
    if (poller_add(l->poller, fd, events, id_ptr(id)) != 0) {
        l->fd_rec[fd] = 0;
        r->fds[slot] = -1;
        return -1;
    }
    return 0;
}

int engine_unwatch(engine_t *e, int fd)
{
    loop_t *l = engine_loop_of(e);
    int slot = -1;
    int id;

    if (l == NULL) {
        errno = ENOTSUP;
        return -1;
    }
    id = rec_find_fd(l, fd, &slot);
    if (id < 0) {
        errno = ENOENT;
        return -1;
    }
    fd_unbind(l, id, slot);
    return 0;
}
