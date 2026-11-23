/* src/engine/engine_pool.c
 *
 * v1 스레드 풀과 엔진 공통 진입점. 명세: engine.h
 */
#include "engine/engine.h"

#include "core/platform.h"
#include "engine/internal_loop.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <pthread.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void *worker_main(void *arg)
{
    engine_t *e = arg;
    for (;;) {
        work_item_t *item;
        pthread_mutex_lock(&e->lock);
        while (e->queue_len == 0 && !e->worker_stop)
            pthread_cond_wait(&e->ready, &e->lock);
        if (e->queue_len == 0 && e->worker_stop) {
            pthread_mutex_unlock(&e->lock);
            return NULL;
        }
        item = e->queue[e->queue_head];
        e->queue[e->queue_head] = NULL;
        e->queue_head = (e->queue_head + 1) % e->queue_cap;
        e->queue_len--;
        item->next = e->active;
        e->active = item;
        pthread_cond_broadcast(&e->changed);
        pthread_mutex_unlock(&e->lock);

        e->cb->serve_blocking(e->ctx, item->obj);
        e->cb->on_close(e->ctx, item->obj);

        pthread_mutex_lock(&e->lock);
        {
            work_item_t **link = &e->active;
            while (*link != NULL && *link != item)
                link = &(*link)->next;
            if (*link == item)
                *link = item->next;
        }
        free(item);
        pthread_cond_broadcast(&e->changed);
        pthread_mutex_unlock(&e->lock);
    }
}

engine_t *engine_new(engine_kind_t kind, int workers, int queue_len, const engine_callbacks_t *cb,
                     void *ctx)
{
    engine_t *e;
    if (kind == ENGINE_EVENT_LOOP)
        return loop_engine_new(cb, ctx);
    if (cb == NULL || kind != ENGINE_THREAD_POOL || workers < 1 || queue_len < 1 ||
        cb->on_accept == NULL || cb->serve_blocking == NULL || cb->on_close == NULL) {
        errno = EINVAL;
        return NULL;
    }
    e = calloc(1, sizeof(*e));
    if (e == NULL)
        return NULL;
    e->kind = kind;
    e->worker_count = workers;
    e->queue_cap = (size_t)queue_len;
    e->cb = cb;
    e->ctx = ctx;
    e->queue = calloc(e->queue_cap, sizeof(*e->queue));
    e->workers = calloc((size_t)workers, sizeof(*e->workers));
    if (e->queue == NULL || e->workers == NULL)
        goto fail;
    if (pthread_mutex_init(&e->lock, NULL) != 0) {
        errno = ENOMEM;
        goto fail;
    }
    if (pthread_cond_init(&e->ready, NULL) != 0) {
        pthread_mutex_destroy(&e->lock);
        errno = ENOMEM;
        goto fail;
    }
    if (pthread_cond_init(&e->changed, NULL) != 0) {
        pthread_cond_destroy(&e->ready);
        pthread_mutex_destroy(&e->lock);
        errno = ENOMEM;
        goto fail;
    }
    return e;
fail:
    free(e->workers);
    free(e->queue);
    free(e);
    return NULL;
}

static void close_unaccepted(engine_t *e, int fd)
{
    if (e->cb->on_busy != NULL)
        e->cb->on_busy(e->ctx, fd);
    else
        (void)close(fd);
}

static void accept_ready(engine_t *e, int listen_fd)
{
    for (;;) {
        struct sockaddr_storage peer;
        socklen_t peer_len = sizeof(peer);
        int fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
        work_item_t *item;
        void *obj;
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            if (errno == EMFILE || errno == ENFILE) {
                struct timespec ts = {0, 100000000};
                (void)nanosleep(&ts, NULL);
            }
            return;
        }
        if (platform_set_cloexec(fd) != 0) {
            (void)close(fd);
            continue;
        }
        pthread_mutex_lock(&e->lock);
        if (e->queue_len >= e->queue_cap) {
            pthread_mutex_unlock(&e->lock);
            close_unaccepted(e, fd);
            continue;
        }
        pthread_mutex_unlock(&e->lock);
        item = calloc(1, sizeof(*item));
        if (item == NULL) {
            close_unaccepted(e, fd);
            continue;
        }
        obj = e->cb->on_accept(e->ctx, fd, &peer, platform_now_ms());
        if (obj == NULL) {
            (void)close(fd);
            free(item);
            continue;
        }
        item->fd = fd;
        item->obj = obj;
        pthread_mutex_lock(&e->lock);
        if (e->queue_len >= e->queue_cap) {
            pthread_mutex_unlock(&e->lock);
            e->cb->on_close(e->ctx, obj);
            free(item);
            continue;
        }
        e->queue[e->queue_tail] = item;
        e->queue_tail = (e->queue_tail + 1) % e->queue_cap;
        e->queue_len++;
        pthread_cond_signal(&e->ready);
        pthread_mutex_unlock(&e->lock);
    }
}

int engine_run(engine_t *e, int listen_fd, volatile sig_atomic_t *stop, int shutdown_grace_ms)
{
    int i, rc = 0;
    int64_t deadline;
    if (e == NULL || stop == NULL || listen_fd < 0 || e->running) {
        errno = EINVAL;
        return -1;
    }
    if (e->kind == ENGINE_EVENT_LOOP)
        return loop_engine_run(e, listen_fd, stop, shutdown_grace_ms);
    e->running = true;
    for (i = 0; i < e->worker_count; i++) {
        int prc = pthread_create(&e->workers[i], NULL, worker_main, e);
        if (prc != 0) {
            errno = prc;
            rc = -1;
            break;
        }
        e->workers_started++;
    }
    if (rc == 0) {
        struct pollfd pfd = {listen_fd, POLLIN, 0};
        while (!*stop) {
            int n = poll(&pfd, 1, 100);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                rc = -1;
                break;
            }
            if (n > 0 && (pfd.revents & POLLIN) != 0)
                accept_ready(e, listen_fd);
            if (n > 0 && (pfd.revents & (POLLERR | POLLNVAL)) != 0) {
                errno = EIO;
                rc = -1;
                break;
            }
        }
    }
    deadline = platform_now_ms() + (shutdown_grace_ms < 0 ? 0 : shutdown_grace_ms);
    for (;;) {
        int empty;
        pthread_mutex_lock(&e->lock);
        empty = e->queue_len == 0 && e->active == NULL;
        if (!empty && platform_now_ms() >= deadline) {
            work_item_t *w;
            for (w = e->active; w != NULL; w = w->next)
                (void)shutdown(w->fd, SHUT_RDWR);
            for (i = 0; (size_t)i < e->queue_len; i++) {
                work_item_t *q = e->queue[(e->queue_head + (size_t)i) % e->queue_cap];
                if (q != NULL)
                    (void)shutdown(q->fd, SHUT_RDWR);
            }
            empty = 1;
        }
        pthread_mutex_unlock(&e->lock);
        if (empty)
            break;
        {
            struct timespec ts = {0, 10000000};
            (void)nanosleep(&ts, NULL);
        }
    }
    pthread_mutex_lock(&e->lock);
    e->worker_stop = true;
    pthread_cond_broadcast(&e->ready);
    pthread_mutex_unlock(&e->lock);
    for (i = 0; i < e->workers_started; i++) {
        int prc = pthread_join(e->workers[i], NULL);
        if (prc != 0 && rc == 0) {
            errno = prc;
            rc = -1;
        }
    }
    e->workers_started = 0;
    e->running = false;
    return rc;
}

void engine_free(engine_t *e)
{
    int saved = errno;
    size_t i;
    if (e == NULL)
        return;
    if (e->kind == ENGINE_EVENT_LOOP) {
        loop_engine_free(e);
        errno = saved;
        return;
    }
    if (e->workers_started > 0) {
        pthread_mutex_lock(&e->lock);
        e->worker_stop = true;
        pthread_cond_broadcast(&e->ready);
        pthread_mutex_unlock(&e->lock);
        for (i = 0; i < (size_t)e->workers_started; i++)
            (void)pthread_join(e->workers[i], NULL);
    }
    for (i = 0; i < e->queue_cap; i++) {
        work_item_t *item = e->queue[i];
        if (item != NULL) {
            e->cb->on_close(e->ctx, item->obj);
            free(item);
        }
    }
    pthread_cond_destroy(&e->changed);
    pthread_cond_destroy(&e->ready);
    pthread_mutex_destroy(&e->lock);
    free(e->workers);
    free(e->queue);
    free(e);
    errno = saved;
}
