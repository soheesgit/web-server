/* src/engine/engine_pool.c
 *
 * v1 스레드 풀과 엔진 공통 진입점. 명세: engine.h
 */
#include "engine/engine.h"

#include "core/log.h"
#include "core/platform.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define ENGINE_STOP_POLL_MS 100 /* stop 플래그 확인 주기 */
#define ENGINE_EMFILE_PAUSE_MS 10

typedef struct {
    void *obj;
    int fd;
} job_t;

struct engine {
    engine_kind_t kind;
    const engine_callbacks_t *cb;
    void *ctx;
    int workers;
    pthread_t *threads;
    int n_threads;  /* 시작한 워커 수 */
    int *active_fd; /* 워커별 처리 중인 연결 fd, 없으면 -1. mu */
    job_t *q;       /* 작업 큐 (원형). mu */
    int qcap, qhead, qlen;
    int busy;  /* 처리 중인 연결 수. mu */
    bool quit; /* 워커 종료 지시. mu */
    pthread_mutex_t mu;
    pthread_cond_t cv_work; /* 큐에 작업이 들어옴 또는 quit */
};

static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};

    (void)nanosleep(&ts, NULL);
}

static bool callbacks_ok(engine_kind_t kind, const engine_callbacks_t *cb)
{
    if (cb == NULL || cb->on_accept == NULL || cb->on_close == NULL) {
        return false;
    }
    if (kind == ENGINE_THREAD_POOL) {
        return cb->serve_blocking != NULL;
    }
    return cb->on_event != NULL && cb->on_timeout != NULL && cb->deadline_ms != NULL;
}

engine_t *engine_new(engine_kind_t kind, int workers, int queue_len, const engine_callbacks_t *cb,
                     void *ctx)
{
    engine_t *e;

    if (!callbacks_ok(kind, cb) || (kind == ENGINE_THREAD_POOL && (workers < 1 || queue_len < 1))) {
        errno = EINVAL;
        return NULL;
    }
    if (kind != ENGINE_THREAD_POOL) {
        errno = ENOSYS; /* v2 이벤트 루프는 아직 */
        return NULL;
    }
    e = calloc(1, sizeof(*e));
    if (e == NULL) {
        return NULL;
    }
    e->kind = kind;
    e->cb = cb;
    e->ctx = ctx;
    e->workers = workers;
    e->qcap = queue_len;
    e->threads = calloc((size_t)workers, sizeof(*e->threads));
    e->active_fd = malloc((size_t)workers * sizeof(*e->active_fd));
    e->q = calloc((size_t)queue_len, sizeof(*e->q));
    if (e->threads == NULL || e->active_fd == NULL || e->q == NULL) {
        free(e->threads);
        free(e->active_fd);
        free(e->q);
        free(e);
        errno = ENOMEM;
        return NULL;
    }
    for (int i = 0; i < workers; i++) {
        e->active_fd[i] = -1;
    }
    pthread_mutex_init(&e->mu, NULL);
    pthread_cond_init(&e->cv_work, NULL);
    return e;
}

typedef struct {
    engine_t *e;
    int idx;
} worker_arg_t;

static void *worker_main(void *arg)
{
    worker_arg_t wa = *(worker_arg_t *)arg;
    engine_t *e = wa.e;

    free(arg);
    for (;;) {
        job_t j;

        pthread_mutex_lock(&e->mu);
        while (e->qlen == 0 && !e->quit) {
            pthread_cond_wait(&e->cv_work, &e->mu);
        }
        if (e->quit) {
            pthread_mutex_unlock(&e->mu);
            return NULL;
        }
        j = e->q[e->qhead];
        e->qhead = (e->qhead + 1) % e->qcap;
        e->qlen--;
        e->active_fd[wa.idx] = j.fd;
        e->busy++;
        pthread_mutex_unlock(&e->mu);

        e->cb->serve_blocking(e->ctx, j.obj);

        /* on_close가 fd를 닫기 전에 지워 둬야, 종료 중 shutdown이 다른 fd를 건드리지 않는다 */
        pthread_mutex_lock(&e->mu);
        e->active_fd[wa.idx] = -1;
        pthread_mutex_unlock(&e->mu);
        e->cb->on_close(e->ctx, j.obj);
        pthread_mutex_lock(&e->mu);
        e->busy--;
        pthread_mutex_unlock(&e->mu);
    }
}

static int start_workers(engine_t *e)
{
    while (e->n_threads < e->workers) {
        worker_arg_t *wa = malloc(sizeof(*wa));
        int rc;

        if (wa == NULL) {
            return -1;
        }
        wa->e = e;
        wa->idx = e->n_threads;
        rc = pthread_create(&e->threads[e->n_threads], NULL, worker_main, wa);
        if (rc != 0) {
            free(wa);
            errno = rc;
            return -1;
        }
        e->n_threads++;
    }
    return 0;
}

/* 남은 워커를 끝내고 join한다. 큐에 남은 연결은 처리하지 않고 on_close. */
static void stop_workers(engine_t *e)
{
    pthread_mutex_lock(&e->mu);
    e->quit = true;
    pthread_cond_broadcast(&e->cv_work);
    pthread_mutex_unlock(&e->mu);
    for (int i = 0; i < e->n_threads; i++) {
        pthread_join(e->threads[i], NULL);
    }
    e->n_threads = 0;
    while (e->qlen > 0) {
        job_t j = e->q[e->qhead];

        e->qhead = (e->qhead + 1) % e->qcap;
        e->qlen--;
        e->cb->on_close(e->ctx, j.obj);
    }
}

static int accept_one(int lfd, int *out_fd, struct sockaddr_storage *peer)
{
    for (;;) {
        socklen_t len = sizeof(*peer);
        int fd = accept(lfd, (struct sockaddr *)peer, &len);

        if (fd >= 0) {
            if (platform_set_cloexec(fd) != 0) {
                close(fd);
                return -1;
            }
            *out_fd = fd;
            return 0;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == ECONNABORTED) {
            errno = EAGAIN;
        }
        return -1;
    }
}

/* 대기 소켓에 쌓인 연결을 받을 수 있는 만큼 받아 큐에 넣는다. */
static void accept_ready(engine_t *e, int lfd)
{
    for (;;) {
        struct sockaddr_storage peer;
        bool full;
        void *obj;
        int fd;

        memset(&peer, 0, sizeof(peer));
        if (accept_one(lfd, &fd, &peer) != 0) {
            if (errno == EMFILE || errno == ENFILE) {
                log_msg(LOG_WARN, "accept: out of file descriptors");
                sleep_ms(ENGINE_EMFILE_PAUSE_MS);
            }
            return;
        }
        /* 큐를 채우는 쪽은 이 스레드뿐이라 여기서 본 자리는 push까지 남아 있다 */
        pthread_mutex_lock(&e->mu);
        full = e->qlen >= e->qcap;
        pthread_mutex_unlock(&e->mu);
        if (full) {
            if (e->cb->on_busy != NULL) {
                e->cb->on_busy(e->ctx, fd);
            } else {
                close(fd);
            }
            continue;
        }
        obj = e->cb->on_accept(e->ctx, fd, &peer, platform_now_ms());
        if (obj == NULL) {
            close(fd);
            continue;
        }
        pthread_mutex_lock(&e->mu);
        e->q[(e->qhead + e->qlen) % e->qcap] = (job_t){obj, fd};
        e->qlen++;
        pthread_cond_signal(&e->cv_work);
        pthread_mutex_unlock(&e->mu);
    }
}

/* 진행 중 연결을 유예만큼 기다리고, 남으면 fd를 shutdown해 워커를 깨운다. */
static void drain(engine_t *e, int grace_ms)
{
    int64_t deadline = platform_now_ms() + grace_ms;

    pthread_mutex_lock(&e->mu);
    while ((e->busy > 0 || e->qlen > 0) && platform_now_ms() < deadline) {
        pthread_mutex_unlock(&e->mu);
        sleep_ms(10);
        pthread_mutex_lock(&e->mu);
    }
    for (int i = 0; i < e->n_threads; i++) {
        if (e->active_fd[i] >= 0) {
            (void)shutdown(e->active_fd[i], SHUT_RDWR);
        }
    }
    pthread_mutex_unlock(&e->mu);
}

int engine_run(engine_t *e, int listen_fd, volatile sig_atomic_t *stop, int shutdown_grace_ms)
{
    if (start_workers(e) != 0) {
        int err = errno;

        stop_workers(e);
        errno = err;
        return -1;
    }
    while (!*stop) {
        struct pollfd pfd = {listen_fd, POLLIN, 0};
        int rc = poll(&pfd, 1, ENGINE_STOP_POLL_MS);

        if (rc > 0) {
            accept_ready(e, listen_fd);
        } else if (rc < 0 && errno != EINTR) {
            int err = errno;

            drain(e, shutdown_grace_ms);
            stop_workers(e);
            errno = err;
            return -1;
        }
    }
    drain(e, shutdown_grace_ms);
    stop_workers(e);
    return 0;
}

void engine_free(engine_t *e)
{
    int err = errno;

    if (e == NULL) {
        return;
    }
    stop_workers(e);
    pthread_cond_destroy(&e->cv_work);
    pthread_mutex_destroy(&e->mu);
    free(e->q);
    free(e->active_fd);
    free(e->threads);
    free(e);
    errno = err;
}
