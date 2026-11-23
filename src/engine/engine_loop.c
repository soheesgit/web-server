/* src/engine/engine_loop.c
 *
 * v2 이벤트 루프. 명세: engine.h
 * 스레드 하나가 poller로 대기 소켓과 모든 연결 fd를 감시한다. 엔진은 accept한 클라이언트 fd를
 * EV_READ로 처음 등록하고, 그 뒤 무엇을 기다릴지는 콜백이 engine_watch로 알린다. 마감
 * (deadline_ms)이 지난 객체에는 on_timeout을 부른다. 콜백은 블로킹하지 않는다.
 */
#include "engine/engine.h"

#include "core/platform.h"
#include "engine/internal_loop.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum { LOOP_BATCH = 256, LOOP_TICK_MS = 100, ACCEPT_PAUSE_MS = 100 };

engine_t *loop_engine_new(const engine_callbacks_t *cb, void *ctx)
{
    engine_t *e;
    if (cb == NULL || cb->on_accept == NULL || cb->on_event == NULL || cb->on_timeout == NULL ||
        cb->deadline_ms == NULL || cb->on_close == NULL) {
        errno = EINVAL;
        return NULL;
    }
    e = calloc(1, sizeof(*e));
    if (e == NULL)
        return NULL;
    e->kind = ENGINE_EVENT_LOOP;
    e->cb = cb;
    e->ctx = ctx;
    e->poller = poller_new();
    if (e->poller == NULL) {
        int saved = errno;
        free(e);
        errno = saved;
        return NULL;
    }
    return e;
}

/* fd 표를 fd 번호가 들어갈 만큼 늘린다. 새 칸은 등록 안 됨(obj NULL)이다. */
static int fds_reserve(engine_t *e, int fd)
{
    size_t need = (size_t)fd + 1, cap;
    loop_fd_t *p;
    if (need <= e->fds_cap)
        return 0;
    cap = e->fds_cap == 0 ? 256 : e->fds_cap;
    while (cap < need)
        cap *= 2;
    p = realloc(e->fds, cap * sizeof(*p));
    if (p == NULL) {
        errno = ENOMEM;
        return -1;
    }
    memset(p + e->fds_cap, 0, (cap - e->fds_cap) * sizeof(*p));
    e->fds = p;
    e->fds_cap = cap;
    return 0;
}

static loop_fd_t *fd_entry(engine_t *e, int fd)
{
    return fd >= 0 && (size_t)fd < e->fds_cap && e->fds[fd].obj != NULL ? &e->fds[fd] : NULL;
}

int engine_watch(engine_t *e, int fd, unsigned events, void *obj)
{
    loop_fd_t *f;
    if (e == NULL || fd < 0 || obj == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (e->kind != ENGINE_EVENT_LOOP) {
        errno = ENOTSUP;
        return -1;
    }
    events &= EV_READ | EV_WRITE;
    f = fd_entry(e, fd);
    if (f != NULL) {
        /* 같은 감시를 다시 요청하면 시스템 호출을 하지 않는다 (이벤트마다 불리므로). */
        if (f->watch == events && f->obj == obj)
            return 0;
        if (poller_mod(e->poller, fd, events, obj) != 0)
            return -1;
        f->watch = events;
        f->obj = obj;
        return 0;
    }
    if (fds_reserve(e, fd) != 0 || poller_add(e->poller, fd, events, obj) != 0)
        return -1;
    e->fds[fd] = (loop_fd_t){obj, events, false, 0};
    return 0;
}

int engine_unwatch(engine_t *e, int fd)
{
    loop_fd_t *f;
    if (e == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (e->kind != ENGINE_EVENT_LOOP) {
        errno = ENOTSUP;
        return -1;
    }
    f = fd_entry(e, fd);
    /* 클라이언트 fd는 엔진이 연결과 함께 정리한다. 추가 fd만 해제할 수 있다. */
    if (f == NULL || f->client) {
        errno = ENOENT;
        return -1;
    }
    (void)poller_del(e->poller, fd);
    memset(f, 0, sizeof(*f));
    return 0;
}

static int add_client(engine_t *e, int fd, void *obj)
{
    if (e->live_len == e->live_cap) {
        size_t cap = e->live_cap == 0 ? 256 : e->live_cap * 2;
        int *p = realloc(e->live, cap * sizeof(*p));
        if (p == NULL) {
            errno = ENOMEM;
            return -1;
        }
        e->live = p;
        e->live_cap = cap;
    }
    if (fds_reserve(e, fd) != 0 || poller_add(e->poller, fd, EV_READ, obj) != 0)
        return -1;
    e->fds[fd] = (loop_fd_t){obj, EV_READ, true, e->live_len};
    e->live[e->live_len++] = fd;
    return 0;
}

/* 연결 하나를 감시에서 빼고 on_close한다. live 배열은 마지막 칸을 빈자리로 옮긴다. */
static void close_client(engine_t *e, int fd)
{
    loop_fd_t *f = &e->fds[fd];
    void *obj = f->obj;
    int last = e->live[--e->live_len];
    e->live[f->slot] = last;
    e->fds[last].slot = f->slot;
    (void)poller_del(e->poller, fd);
    memset(f, 0, sizeof(*f));
    e->cb->on_close(e->ctx, obj);
}

/* 추가 fd의 이벤트로 객체를 닫을 때 그 객체의 클라이언트 fd를 찾는다. */
static int client_fd_of(engine_t *e, void *obj)
{
    for (size_t i = 0; i < e->live_len; i++) {
        if (e->fds[e->live[i]].obj == obj)
            return e->live[i];
    }
    return -1;
}

static void note_deadline(engine_t *e, void *obj, int64_t *next)
{
    int64_t d = e->cb->deadline_ms(e->ctx, obj);
    if (d >= 0 && (*next < 0 || d < *next))
        *next = d;
}

static void accept_ready(engine_t *e, int listen_fd, int64_t *pause_until, int64_t *next)
{
    for (;;) {
        struct sockaddr_storage peer;
        socklen_t peer_len = sizeof(peer);
        int fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
        void *obj;
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            /* fd가 다 찼으면 잠시 대기 소켓 감시를 멈춘다. 레벨 트리거라 그대로 두면 계속 깬다. */
            if ((errno == EMFILE || errno == ENFILE) && poller_mod(e->poller, listen_fd, 0, e) == 0)
                *pause_until = platform_now_ms() + ACCEPT_PAUSE_MS;
            return;
        }
        if (platform_set_cloexec(fd) != 0) {
            (void)close(fd);
            continue;
        }
        obj = e->cb->on_accept(e->ctx, fd, &peer, platform_now_ms());
        if (obj == NULL) {
            (void)close(fd);
            continue;
        }
        if (add_client(e, fd, obj) != 0) {
            e->cb->on_close(e->ctx, obj);
            continue;
        }
        note_deadline(e, obj, next);
    }
}

static void dispatch(engine_t *e, const poller_event_t *ev, int64_t now, int64_t *next)
{
    loop_fd_t *f = fd_entry(e, ev->fd);
    void *obj;
    int cfd;
    if (f == NULL) /* 같은 묶음에서 이미 닫힌 연결 */
        return;
    obj = f->obj;
    if (e->cb->on_event(e->ctx, obj, ev->fd, ev->events, now) == 0) {
        note_deadline(e, obj, next);
        return;
    }
    /* 콜백 안의 engine_watch가 표를 재할당했을 수 있어 다시 찾는다. */
    f = fd_entry(e, ev->fd);
    cfd = f != NULL && f->client ? ev->fd : client_fd_of(e, obj);
    if (cfd >= 0)
        close_client(e, cfd);
}

/* 마감이 지난 연결에 on_timeout을 부르고, 남은 가장 이른 마감을 돌려준다.
 * shortcut: 연결 수만큼 훑는다(O(n)), 마감이 자주 겹치고 연결이 수만 개면 최소 힙으로 바꾼다. */
static int64_t expire(engine_t *e, int64_t now)
{
    int64_t next = -1;
    size_t i = e->live_len;
    /* 뒤에서부터 돌아야 close_client가 옮겨 놓는 마지막 칸을 다시 보지 않는다. */
    while (i-- > 0) {
        int fd = e->live[i];
        void *obj = e->fds[fd].obj;
        int64_t d = e->cb->deadline_ms(e->ctx, obj);
        if (d >= 0 && now >= d && e->cb->on_timeout(e->ctx, obj, now) != 0) {
            close_client(e, fd);
            continue;
        }
        note_deadline(e, obj, &next);
    }
    return next;
}

int loop_engine_run(engine_t *e, int listen_fd, volatile sig_atomic_t *stop, int shutdown_grace_ms)
{
    poller_event_t evs[LOOP_BATCH];
    int64_t pause_until = -1, grace_end = -1, next = -1;
    bool listening = true;
    int rc = 0;
    if (poller_add(e->poller, listen_fd, EV_READ, e) != 0)
        return -1;
    e->running = true;
    for (;;) {
        int64_t now = platform_now_ms();
        int timeout = LOOP_TICK_MS, n;
        bool accept_now = false;
        if (*stop && listening) {
            (void)poller_del(e->poller, listen_fd);
            listening = false;
            grace_end = now + (shutdown_grace_ms < 0 ? 0 : shutdown_grace_ms);
        }
        if (!listening && (e->live_len == 0 || now >= grace_end))
            break;
        if (listening && pause_until >= 0 && now >= pause_until &&
            poller_mod(e->poller, listen_fd, EV_READ, e) == 0)
            pause_until = -1;
        if (next >= 0 && next - now < timeout)
            timeout = next <= now ? 0 : (int)(next - now);
        n = poller_wait(e->poller, evs, LOOP_BATCH, timeout);
        if (n < 0) {
            rc = -1;
            break;
        }
        now = platform_now_ms();
        /* 연결 이벤트를 먼저 처리한다. 닫힌 fd 번호가 같은 묶음 안에서 새 연결에 재사용되지 않게.
         */
        for (int i = 0; i < n; i++) {
            if (evs[i].fd == listen_fd)
                accept_now = listening;
            else
                dispatch(e, &evs[i], now, &next);
        }
        if (accept_now)
            accept_ready(e, listen_fd, &pause_until, &next);
        if (next >= 0 && platform_now_ms() >= next)
            next = expire(e, platform_now_ms());
    }
    if (listening)
        (void)poller_del(e->poller, listen_fd);
    while (e->live_len > 0)
        close_client(e, e->live[e->live_len - 1]);
    e->running = false;
    return rc;
}

void loop_engine_free(engine_t *e)
{
    while (e->live_len > 0)
        close_client(e, e->live[e->live_len - 1]);
    poller_free(e->poller);
    free(e->fds);
    free(e->live);
    free(e);
}
