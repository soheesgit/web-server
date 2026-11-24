/* v1 스레드 풀. 소켓 소유권은 on_accept 성공 뒤 콜백에 있다. */
#include "engine/engine.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "core/platform.h"

typedef struct {
    int fd;
    void *obj;
} connection_t;

typedef struct {
    engine_t *engine;
    pthread_t thread;
    int active_fd;
} worker_t;

struct engine {
    const engine_callbacks_t *cb;
    void *ctx;
    pthread_mutex_t lock;
    pthread_cond_t ready;
    worker_t *workers;
    connection_t *queue;
    size_t nworkers, capacity, head, length, busy;
    bool stopping;
};

static void *serve(void *arg)
{
    worker_t *worker = arg;
    engine_t *e = worker->engine;

    pthread_mutex_lock(&e->lock);
    for (;;) {
        while (e->length == 0 && !e->stopping) {
            pthread_cond_wait(&e->ready, &e->lock);
        }
        if (e->stopping) {
            break;
        }

        connection_t next = e->queue[e->head];
        e->head = (e->head + 1) % e->capacity;
        e->length--;
        e->busy++;
        worker->active_fd = next.fd;
        pthread_mutex_unlock(&e->lock);

        e->cb->serve_blocking(e->ctx, next.obj);

        pthread_mutex_lock(&e->lock);
        /* shutdown이 이미 닫혔거나 재사용된 fd를 보지 않도록 먼저 지운다. */
        worker->active_fd = -1;
        pthread_mutex_unlock(&e->lock);
        e->cb->on_close(e->ctx, next.obj);
        pthread_mutex_lock(&e->lock);
        e->busy--;
    }
    pthread_mutex_unlock(&e->lock);
    return NULL;
}

engine_t *engine_new(engine_kind_t kind, int workers, int queue_len, const engine_callbacks_t *cb,
                     void *ctx)
{
    if (kind != ENGINE_THREAD_POOL) {
        errno = ENOTSUP;
        return NULL;
    }
    if (workers < 1 || queue_len < 1 || cb == NULL || cb->on_accept == NULL ||
        cb->serve_blocking == NULL || cb->on_close == NULL) {
        errno = EINVAL;
        return NULL;
    }

    engine_t *e = calloc(1, sizeof(*e));
    if (e == NULL) {
        return NULL;
    }
    e->workers = calloc((size_t)workers, sizeof(*e->workers));
    e->queue = calloc((size_t)queue_len, sizeof(*e->queue));
    int error = ENOMEM;
    if (e->workers != NULL && e->queue != NULL) {
        error = pthread_mutex_init(&e->lock, NULL);
        if (error == 0) {
            error = pthread_cond_init(&e->ready, NULL);
            if (error == 0) {
                e->nworkers = (size_t)workers;
                e->capacity = (size_t)queue_len;
                e->cb = cb;
                e->ctx = ctx;
                return e;
            }
            pthread_mutex_destroy(&e->lock);
        }
    }
    free(e->workers);
    free(e->queue);
    free(e);
    errno = error;
    return NULL;
}

static void offer(engine_t *e, int fd, const struct sockaddr_storage *peer)
{
    pthread_mutex_lock(&e->lock);
    bool full = e->length == e->capacity;
    pthread_mutex_unlock(&e->lock);
    /* accept 스레드만 큐에 넣으므로 확보한 자리를 다른 스레드가 가져가지 않는다. */
    if (full) {
        if (e->cb->on_busy != NULL) {
            e->cb->on_busy(e->ctx, fd);
        } else {
            close(fd);
        }
        return;
    }
    void *obj = e->cb->on_accept(e->ctx, fd, peer, platform_now_ms());
    if (obj == NULL) {
        close(fd);
        return;
    }
    pthread_mutex_lock(&e->lock);
    e->queue[(e->head + e->length) % e->capacity] = (connection_t){fd, obj};
    e->length++;
    pthread_cond_signal(&e->ready);
    pthread_mutex_unlock(&e->lock);
}

static int accept_loop(engine_t *e, int lfd, const volatile sig_atomic_t *stop)
{
    while (!*stop) {
        struct pollfd pfd = {lfd, POLLIN, 0};
        int ready = poll(&pfd, 1, 50);
        if (ready < 0 && errno != EINTR) {
            return -1;
        }
        if (ready <= 0 || *stop) {
            continue;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            errno = EIO;
            return -1;
        }
        struct sockaddr_storage peer = {0};
        socklen_t len = sizeof(peer);
        int fd = accept(lfd, (struct sockaddr *)&peer, &len);
        if (fd < 0) {
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
                (void)poll(NULL, 0, 50);
            } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK &&
                       errno != ECONNABORTED && errno != EPROTO) {
                return -1;
            }
            continue;
        }
        if (*stop) {
            close(fd);
            return 0;
        }
        if (platform_set_cloexec(fd) != 0) {
            close(fd);
            continue;
        }
        offer(e, fd, &peer);
    }
    return 0;
}

static void stop_workers(engine_t *e, size_t started, int grace_ms)
{
    int64_t deadline = platform_now_ms() + grace_ms;

    pthread_mutex_lock(&e->lock);
    while ((e->length != 0 || e->busy != 0) && platform_now_ms() < deadline) {
        pthread_mutex_unlock(&e->lock);
        (void)poll(NULL, 0, 10);
        pthread_mutex_lock(&e->lock);
    }
    e->stopping = true;
    for (size_t i = 0; i < started; i++) {
        if (e->workers[i].active_fd >= 0) {
            (void)shutdown(e->workers[i].active_fd, SHUT_RDWR);
        }
    }
    pthread_cond_broadcast(&e->ready);
    pthread_mutex_unlock(&e->lock);
    for (size_t i = 0; i < started; i++) {
        pthread_join(e->workers[i].thread, NULL);
    }
    while (e->length != 0) {
        e->cb->on_close(e->ctx, e->queue[e->head].obj);
        e->head = (e->head + 1) % e->capacity;
        e->length--;
    }
}

int engine_run(engine_t *e, int listen_fd, volatile sig_atomic_t *stop, int shutdown_grace_ms)
{
    size_t started = 0;
    int error = 0;
    for (; started < e->nworkers; started++) {
        worker_t *worker = &e->workers[started];
        worker->engine = e;
        worker->active_fd = -1;
        error = pthread_create(&worker->thread, NULL, serve, worker);
        if (error != 0) {
            break;
        }
    }
    if (error == 0 && accept_loop(e, listen_fd, stop) != 0) {
        error = errno;
    }
    stop_workers(e, started, error == 0 ? shutdown_grace_ms : 0);
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

void engine_free(engine_t *e)
{
    int saved = errno;
    if (e != NULL) {
        pthread_cond_destroy(&e->ready);
        pthread_mutex_destroy(&e->lock);
        free(e->queue);
        free(e->workers);
        free(e);
    }
    errno = saved;
}
