/* v2 연결 소유권, fd 이벤트 배치 및 단조 시계 마감 관리. */
#include "engine/internal_loop.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

#include "core/platform.h"
#include "engine/poller.h"

#define LOOP_BATCH 128
#define LOOP_ACCEPT_BATCH 64
#define LOOP_TICK_MS 50
#define NO_TIMER SIZE_MAX

typedef struct loop_client loop_client_t;

typedef struct loop_watch {
    struct loop_watch *next;
    loop_client_t *owner;
    int fd;
} loop_watch_t;

struct loop_client {
    loop_client_t *prev, *next;
    loop_watch_t *watches;
    void *obj;
    int fd;
    size_t timer;
    int64_t deadline;
};

struct engine_loop {
    poller_t *poller;
    const engine_callbacks_t *cb;
    void *ctx;
    loop_client_t *clients, *current;
    loop_watch_t *retired;
    loop_client_t **timers;
    size_t timer_count, timer_cap;
};

static void timer_swap(engine_loop_t *loop, size_t a, size_t b)
{
    loop_client_t *first = loop->timers[a];
    loop->timers[a] = loop->timers[b];
    loop->timers[a]->timer = a;
    loop->timers[b] = first;
    first->timer = b;
}

static void timer_up(engine_loop_t *loop, size_t i)
{
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (loop->timers[parent]->deadline <= loop->timers[i]->deadline) {
            break;
        }
        timer_swap(loop, i, parent);
        i = parent;
    }
}

static void timer_down(engine_loop_t *loop, size_t i)
{
    while (i < loop->timer_count / 2) {
        size_t child = i * 2 + 1;
        if (child + 1 < loop->timer_count &&
            loop->timers[child + 1]->deadline < loop->timers[child]->deadline) {
            child++;
        }
        if (loop->timers[i]->deadline <= loop->timers[child]->deadline) {
            break;
        }
        timer_swap(loop, i, child);
        i = child;
    }
}

static void timer_remove(engine_loop_t *loop, loop_client_t *c)
{
    size_t index = c->timer;
    if (index == NO_TIMER) {
        return;
    }
    c->timer = NO_TIMER;
    size_t last = --loop->timer_count;
    if (index == last) {
        return;
    }
    loop_client_t *moved = loop->timers[last];
    moved->timer = index;
    loop->timers[index] = moved;
    timer_up(loop, index);
    timer_down(loop, moved->timer);
}

static int timer_refresh(engine_loop_t *loop, loop_client_t *c)
{
    int64_t deadline = loop->cb->deadline_ms(loop->ctx, c->obj);
    if (deadline == c->deadline) {
        return 0;
    }
    timer_remove(loop, c);
    c->deadline = deadline;
    if (deadline < 0) {
        return 0;
    }
    if (loop->timer_count == loop->timer_cap) {
        size_t cap = loop->timer_cap ? loop->timer_cap * 2 : 64;
        if (cap < loop->timer_cap || cap > SIZE_MAX / sizeof(*loop->timers)) {
            errno = ENOMEM;
            return -1;
        }
        loop_client_t **timers = realloc(loop->timers, cap * sizeof(*timers));
        if (timers == NULL) {
            return -1;
        }
        loop->timers = timers;
        loop->timer_cap = cap;
    }
    c->timer = loop->timer_count++;
    loop->timers[c->timer] = c;
    timer_up(loop, c->timer);
    return 0;
}

static int add_watch(engine_loop_t *loop, loop_client_t *c, int fd, unsigned events)
{
    for (loop_watch_t *watch = c->watches; watch != NULL; watch = watch->next) {
        if (watch->fd == fd) {
            return poller_mod(loop->poller, fd, events, watch);
        }
    }
    loop_watch_t *watch = malloc(sizeof(*watch));
    if (watch == NULL) {
        return -1;
    }
    *watch = (loop_watch_t){c->watches, c, fd};
    if (poller_add(loop->poller, fd, events, watch) != 0) {
        free(watch);
        return -1;
    }
    c->watches = watch;
    return 0;
}

/* 이미 가져온 이벤트 배치가 참조할 수 있으므로 배치 끝까지 등록 객체를 보존한다. */
static void retire(engine_loop_t *loop, loop_watch_t **link)
{
    loop_watch_t *watch = *link;
    *link = watch->next;
    watch->owner = NULL;
    watch->next = loop->retired;
    loop->retired = watch;
}

static void release_retired(engine_loop_t *loop)
{
    while (loop->retired != NULL) {
        loop_watch_t *watch = loop->retired;
        loop->retired = watch->next;
        free(watch);
    }
}

int engine_loop_watch(engine_loop_t *loop, int fd, unsigned events, void *obj)
{
    loop_client_t *c = loop->current;
    if (c == NULL || obj == NULL || (c->obj != NULL && c->obj != obj)) {
        errno = EINVAL;
        return -1;
    }
    return add_watch(loop, c, fd, events);
}

int engine_loop_unwatch(engine_loop_t *loop, int fd)
{
    loop_client_t *c = loop->current;
    if (c == NULL || fd == c->fd) {
        errno = c == NULL ? ENOENT : EINVAL;
        return -1;
    }
    for (loop_watch_t **link = &c->watches; *link != NULL; link = &(*link)->next) {
        if ((*link)->fd == fd) {
            if (poller_del(loop->poller, fd) != 0) {
                return -1;
            }
            retire(loop, link);
            return 0;
        }
    }
    errno = ENOENT;
    return -1;
}

static void unwatch_client(engine_loop_t *loop, loop_client_t *c)
{
    while (c->watches != NULL) {
        (void)poller_del(loop->poller, c->watches->fd);
        retire(loop, &c->watches);
    }
}

static void close_client(engine_loop_t *loop, loop_client_t *c)
{
    timer_remove(loop, c);
    unwatch_client(loop, c);
    if (c->prev != NULL) {
        c->prev->next = c->next;
    } else {
        loop->clients = c->next;
    }
    if (c->next != NULL) {
        c->next->prev = c->prev;
    }
    loop->cb->on_close(loop->ctx, c->obj);
    free(c);
}

static void add_client(engine_loop_t *loop, int fd, const struct sockaddr_storage *peer)
{
    loop_client_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        close(fd);
        return;
    }
    c->fd = fd;
    c->timer = NO_TIMER;
    c->deadline = -1;
    loop->current = c;
    c->obj = loop->cb->on_accept(loop->ctx, fd, peer, platform_now_ms());
    loop->current = NULL;
    if (c->obj == NULL) {
        unwatch_client(loop, c);
        close(fd);
        free(c);
        return;
    }
    c->next = loop->clients;
    if (c->next != NULL) {
        c->next->prev = c;
    }
    loop->clients = c;
    if (add_watch(loop, c, fd, EV_READ) != 0 || timer_refresh(loop, c) != 0) {
        close_client(loop, c);
    }
}

static int accept_ready(engine_loop_t *loop, int lfd, const volatile sig_atomic_t *stop)
{
    for (int i = 0; i < LOOP_ACCEPT_BATCH && !*stop; i++) {
        struct sockaddr_storage peer = {0};
        socklen_t len = sizeof(peer);
        int fd = accept(lfd, (struct sockaddr *)&peer, &len);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED || errno == EPROTO) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
                return 1; /* 대기 소켓을 잠시 멈추고 진행 중인 연결부터 처리한다. */
            }
            return -1;
        }
        if (*stop || platform_set_cloexec(fd) != 0 || platform_set_nonblock(fd) != 0) {
            close(fd);
            continue;
        }
        add_client(loop, fd, &peer);
    }
    return 0;
}

static void dispatch(engine_loop_t *loop, loop_client_t *c, int fd, unsigned events)
{
    int64_t now = platform_now_ms();
    loop->current = c;
    int rc = c->deadline >= 0 && now >= c->deadline
                 ? loop->cb->on_timeout(loop->ctx, c->obj, now)
                 : loop->cb->on_event(loop->ctx, c->obj, fd, events, now);
    loop->current = NULL;
    if (rc < 0 || timer_refresh(loop, c) != 0) {
        close_client(loop, c);
    }
}

engine_loop_t *engine_loop_new(const engine_callbacks_t *cb, void *ctx)
{
    if (cb == NULL || cb->on_accept == NULL || cb->on_event == NULL || cb->on_timeout == NULL ||
        cb->deadline_ms == NULL || cb->on_close == NULL) {
        errno = EINVAL;
        return NULL;
    }
    engine_loop_t *loop = calloc(1, sizeof(*loop));
    if (loop == NULL) {
        return NULL;
    }
    loop->cb = cb;
    loop->ctx = ctx;
    loop->poller = poller_new();
    if (loop->poller == NULL) {
        free(loop);
        return NULL;
    }
    return loop;
}

int engine_loop_run(engine_loop_t *loop, int lfd, const volatile sig_atomic_t *stop, int grace_ms)
{
    if (poller_add(loop->poller, lfd, EV_READ, NULL) != 0) {
        return -1;
    }
    int rc = 0;
    int64_t grace_end = -1, accept_after = -1;
    poller_event_t events[LOOP_BATCH];

    for (;;) {
        int64_t now = platform_now_ms();
        if (*stop && grace_end < 0) {
            grace_end = now + grace_ms;
            if (poller_mod(loop->poller, lfd, 0, NULL) != 0) {
                rc = -1;
                break;
            }
        }
        if (grace_end >= 0 && (loop->clients == NULL || now >= grace_end)) {
            break;
        }
        if (grace_end < 0 && accept_after >= 0 && now >= accept_after) {
            if (poller_mod(loop->poller, lfd, EV_READ, NULL) != 0) {
                rc = -1;
                break;
            }
            accept_after = -1;
        }
        int64_t next = now + LOOP_TICK_MS;
        if (loop->timer_count > 0 && loop->timers[0]->deadline < next) {
            next = loop->timers[0]->deadline;
        }
        if (grace_end >= 0 && grace_end < next) {
            next = grace_end;
        }
        int n = poller_wait(loop->poller, events, LOOP_BATCH, next <= now ? 0 : (int)(next - now));
        if (n < 0) {
            rc = -1;
            break;
        }
        for (int i = 0; i < n; i++) {
            loop_watch_t *watch = events[i].ptr;
            if (watch != NULL) {
                if (watch->owner != NULL) {
                    dispatch(loop, watch->owner, events[i].fd, events[i].events);
                }
            } else if (!*stop) {
                int accepted = accept_ready(loop, lfd, stop);
                if (accepted < 0 || (accepted > 0 && poller_mod(loop->poller, lfd, 0, NULL) != 0)) {
                    rc = -1;
                    break;
                }
                if (accepted > 0) {
                    accept_after = platform_now_ms() + LOOP_TICK_MS;
                }
            }
        }
        if (rc < 0) {
            break;
        }
        for (int i = 0; i < LOOP_BATCH && loop->timer_count > 0; i++) {
            loop_client_t *c = loop->timers[0];
            if (c->deadline > platform_now_ms()) {
                break;
            }
            dispatch(loop, c, c->fd, 0);
        }
        release_retired(loop);
    }

    int saved = errno;
    (void)poller_del(loop->poller, lfd);
    while (loop->clients != NULL) {
        close_client(loop, loop->clients);
    }
    release_retired(loop);
    errno = saved;
    return rc;
}

void engine_loop_free(engine_loop_t *loop)
{
    if (loop != NULL) {
        int saved = errno;
        while (loop->clients != NULL) {
            close_client(loop, loop->clients);
        }
        release_retired(loop);
        free(loop->timers);
        poller_free(loop->poller);
        free(loop);
        errno = saved;
    }
}
