/* src/upstream/pool.c
 *
 * 연결 획득·반납. 명세: upstream.h
 */
#include "upstream/upstream.h"

#include "core/platform.h"
#include "upstream/internal_upstream.h"

#include <errno.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#define POOL_RBUF_INITIAL (16 * 1024)
#define POOL_RBUF_MAX (1024 * 1024)

static void conn_destroy(upstream_conn_t *c)
{
    int e = errno;

    if (c->fd >= 0) {
        close(c->fd);
    }
    buf_cleanup(&c->rbuf);
    free(c);
    errno = e;
}

/* 처리 중 요청 수: 빌려 간 연결 하나 = 요청 하나 (picker가 본다). */
static void count_inflight(struct backend *b, int d)
{
    pthread_mutex_lock(&b->group->mu);
    b->inflight += d;
    pthread_mutex_unlock(&b->group->mu);
}

/* 최근 반납한 유휴 연결부터 꺼낸다. 없으면 NULL. */
static upstream_conn_t *pop_idle(struct backend *b)
{
    upstream_conn_t *c = NULL;

    pthread_mutex_lock(&b->group->mu);
    if (b->n_idle > 0) {
        c = b->idle[--b->n_idle];
    }
    pthread_mutex_unlock(&b->group->mu);
    return c;
}

/* 유휴 중 백엔드가 닫았거나 예상 밖 바이트를 보냈으면 false. */
static bool still_alive(int fd)
{
    char ch;
    ssize_t n = recv(fd, &ch, 1, MSG_PEEK | MSG_DONTWAIT);

    return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
}

static int open_socket(const struct backend *b, upstream_conn_t *c)
{
    int one = 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        return -1;
    }
    c->fd = fd;
    if (platform_set_cloexec(fd) != 0 || platform_set_nonblock(fd) != 0 ||
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0) {
        return -1;
    }
    if (connect(fd, (const struct sockaddr *)&b->addr, sizeof(b->addr)) == 0) {
        c->connecting = false;
        return 0;
    }
    if (errno == EINPROGRESS) {
        c->connecting = true;
        return 0;
    }
    return -1;
}

int upstream_acquire(upstream_group_t *g, backend_t *b, upstream_conn_t **out_c)
{
    upstream_conn_t *c;

    (void)g; /* b->group과 같다 */
    while ((c = pop_idle(b)) != NULL) {
        if (still_alive(c->fd)) {
            c->reused = true;
            c->connecting = false;
            buf_reset(&c->rbuf);
            count_inflight(b, 1);
            *out_c = c;
            return 0;
        }
        conn_destroy(c);
    }

    c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return -1;
    }
    c->fd = -1;
    c->backend = b;
    if (buf_init(&c->rbuf, POOL_RBUF_INITIAL, POOL_RBUF_MAX) != 0 || open_socket(b, c) != 0) {
        conn_destroy(c);
        return -1;
    }
    count_inflight(b, 1);
    *out_c = c;
    return 0;
}

int upstream_connect_result(upstream_conn_t *c)
{
    int soerr = 0;
    socklen_t len = sizeof(soerr);

    if (!c->connecting) {
        return 0;
    }
    if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &soerr, &len) != 0) {
        return -1;
    }
    if (soerr != 0) {
        errno = soerr;
        return -1;
    }
    c->connecting = false;
    return 0;
}

void upstream_release(upstream_conn_t *c, bool reusable)
{
    struct backend *b;
    bool pooled = false;

    if (c == NULL) {
        return;
    }
    b = c->backend;
    count_inflight(b, -1);
    if (reusable && !c->connecting) {
        buf_reset(&c->rbuf);
        pthread_mutex_lock(&b->group->mu);
        if (b->n_idle < b->group->reg->max_idle) {
            b->idle[b->n_idle++] = c;
            pooled = true;
        }
        pthread_mutex_unlock(&b->group->mu);
    }
    if (!pooled) {
        conn_destroy(c);
    }
}
