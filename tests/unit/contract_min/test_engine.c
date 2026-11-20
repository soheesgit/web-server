/* tests/unit/contract_min/test_engine.c
 *
 * engine/ 계약 최소셋. 기대값의 원래 표: 기획서 11.5절. ID 목록: src/engine/README.md
 */
#include "unit/testlib.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "engine/engine.h"
#include "engine/poller.h"

static volatile sig_atomic_t g_stop;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_accepted, g_served, g_closed;

typedef struct {
    int fd;
} obj_t;

static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void *cb_accept(void *ctx, int fd, const struct sockaddr_storage *peer, int64_t now_ms)
{
    obj_t *o = malloc(sizeof(*o));
    (void)ctx;
    (void)peer;
    (void)now_ms;
    if (o == NULL) {
        return NULL;
    }
    o->fd = fd;
    pthread_mutex_lock(&g_mu);
    g_accepted++;
    pthread_mutex_unlock(&g_mu);
    return o;
}

static void cb_serve(void *ctx, void *obj)
{
    (void)ctx;
    (void)obj;
    pthread_mutex_lock(&g_mu);
    g_served++;
    pthread_mutex_unlock(&g_mu);
}

static int cb_event(void *ctx, void *obj, int fd, unsigned events, int64_t now_ms)
{
    (void)ctx;
    (void)obj;
    (void)fd;
    (void)events;
    (void)now_ms;
    return -1;
}

static int cb_timeout(void *ctx, void *obj, int64_t now_ms)
{
    (void)ctx;
    (void)obj;
    (void)now_ms;
    return -1;
}

static int64_t cb_deadline(void *ctx, void *obj)
{
    (void)ctx;
    (void)obj;
    return -1;
}

static void cb_close(void *ctx, void *obj)
{
    obj_t *o = obj;
    (void)ctx;
    close(o->fd);
    free(o);
    pthread_mutex_lock(&g_mu);
    g_closed++;
    pthread_mutex_unlock(&g_mu);
}

static void cb_busy(void *ctx, int fd)
{
    (void)ctx;
    close(fd);
}

static const engine_callbacks_t CB = {cb_accept,   cb_serve, cb_event, cb_timeout,
                                      cb_deadline, cb_close, cb_busy};

static int open_listener(int *out_port)
{
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    int one = 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        return -1;
    }
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(fd, 128) != 0 ||
        getsockname(fd, (struct sockaddr *)&sa, &sl) != 0 ||
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) != 0) {
        close(fd);
        return -1;
    }
    *out_port = ntohs(sa.sin_port);
    return fd;
}

static void *stop_after_100ms(void *arg)
{
    (void)arg;
    sleep_ms(100);
    g_stop = 1;
    return NULL;
}

static void t_u_en_01(void)
{
    int port = 0;
    int lfd = open_listener(&port);
    engine_t *e;
    pthread_t th;
    int64_t t0;

    CHECK(lfd >= 0);
    e = engine_new(ENGINE_THREAD_POOL, 4, 16, &CB, NULL);
    CHECK(e != NULL);
    g_stop = 0;
    CHECK_EQ_INT(pthread_create(&th, NULL, stop_after_100ms, NULL), 0);
    t0 = mono_ms();
    CHECK_EQ_INT(engine_run(e, lfd, &g_stop, 1000), 0);
    CHECK(mono_ms() - t0 < 3000);
    pthread_join(th, NULL);
    engine_free(e);
    close(lfd);
}

static int g_port;

static void *connector(void *arg)
{
    int64_t deadline;
    (void)arg;

    for (int i = 0; i < 1000; i++) {
        struct sockaddr_in sa;
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        int lag;

        if (fd < 0) {
            break;
        }
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons((uint16_t)g_port);
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        (void)connect(fd, (struct sockaddr *)&sa, sizeof(sa));
        close(fd);
        /* accept 큐(backlog)를 넘지 않도록 엔진이 따라올 때까지 기다린다 */
        deadline = mono_ms() + 3000;
        do {
            pthread_mutex_lock(&g_mu);
            lag = i + 1 - g_accepted;
            pthread_mutex_unlock(&g_mu);
            if (lag > 64) {
                sleep_ms(1);
            }
        } while (lag > 64 && mono_ms() < deadline);
    }
    deadline = mono_ms() + 3000;
    for (;;) {
        int done;
        pthread_mutex_lock(&g_mu);
        done = g_closed >= 1000;
        pthread_mutex_unlock(&g_mu);
        if (done || mono_ms() > deadline) {
            break;
        }
        sleep_ms(5);
    }
    g_stop = 1;
    return NULL;
}

static void t_u_en_02(void)
{
    int lfd = open_listener(&g_port);
    engine_t *e;
    pthread_t th;

    CHECK(lfd >= 0);
    e = engine_new(ENGINE_THREAD_POOL, 4, 1024, &CB, NULL);
    CHECK(e != NULL);
    g_stop = 0;
    g_accepted = g_served = g_closed = 0;
    CHECK_EQ_INT(pthread_create(&th, NULL, connector, NULL), 0);
    CHECK_EQ_INT(engine_run(e, lfd, &g_stop, 1000), 0);
    pthread_join(th, NULL);
    engine_free(e);
    close(lfd);
    CHECK_EQ_INT(g_accepted, 1000);
    CHECK_EQ_INT(g_served, 1000);
    CHECK_EQ_INT(g_closed, 1000);
}

static void t_u_en_05(void)
{
    poller_t *p = poller_new();
    poller_event_t ev[8];
    int sv[2];
    int marker = 0;

    CHECK(p != NULL);
    CHECK_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    CHECK_EQ_INT(poller_add(p, sv[0], 1u, &marker), 0);
    CHECK_EQ_INT(write(sv[1], "x", 1), 1);
    CHECK_EQ_INT(poller_wait(p, ev, 8, 1000), 1);
    CHECK_EQ_INT(ev[0].fd, sv[0]);
    CHECK((ev[0].events & 1u) != 0);
    CHECK(ev[0].ptr == &marker);
    CHECK_EQ_INT(poller_del(p, sv[0]), 0);
    poller_free(p);
    close(sv[0]);
    close(sv[1]);
}

void register_engine_tests(void)
{
    test_register("U-EN-01", "engine_run: pool of 4 stops 100ms after flag", t_u_en_01);
    test_register("U-EN-02", "engine: 1000 connections served exactly once", t_u_en_02);
    test_register("U-EN-05", "poller: socketpair read event returns ptr", t_u_en_05);
}
