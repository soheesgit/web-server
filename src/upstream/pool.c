/* src/upstream/pool.c
 *
 * 연결 획득·반납. 명세: upstream.h
 */
#include "upstream/upstream.h"

#include "core/platform.h"
#include "upstream/internal_state.h"

#include <errno.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void conn_destroy(upstream_conn_t *c)
{
    if (c == NULL) return;
    if (c->fd >= 0) (void)close(c->fd);
    buf_cleanup(&c->rbuf);
    free(c);
}

static int conn_new(backend_t *b, upstream_conn_t **out)
{
    upstream_conn_t *c = calloc(1, sizeof(*c));
    int one = 1;
    if (c == NULL) return -1;
    c->fd = -1;
    c->backend = b;
    if (buf_init(&c->rbuf, 16384, 1024u * 1024u) != 0) { free(c); return -1; }
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (c->fd < 0) { conn_destroy(c); return -1; }
    if (platform_set_cloexec(c->fd) != 0 || platform_set_nonblock(c->fd) != 0 ||
        setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0) {
        int saved = errno;
        conn_destroy(c);
        errno = saved;
        return -1;
    }
    if (connect(c->fd, (struct sockaddr *)&b->addr, sizeof(b->addr)) == 0) {
        c->connecting = false;
    } else if (errno == EINPROGRESS || errno == EWOULDBLOCK) {
        c->connecting = true;
    } else {
        int saved = errno;
        conn_destroy(c);
        errno = saved;
        return -1;
    }
    c->reused = false;
    *out = c;
    return 0;
}

int upstream_acquire(upstream_group_t *g, backend_t *b, upstream_conn_t **out_c)
{
    if (g == NULL || b == NULL || out_c == NULL) { errno = EINVAL; return -1; }
    for (;;) {
        idle_node_t *node;
        ssize_t n;
        char peek_byte;
        pthread_mutex_lock(&b->lock);
        node = b->idle;
        if (node != NULL) {
            b->idle = node->next;
            b->idle_count--;
        }
        pthread_mutex_unlock(&b->lock);
        if (node == NULL) break;
        n = recv(node->conn->fd, &peek_byte, 1, MSG_PEEK | MSG_DONTWAIT);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            upstream_conn_t *c = node->conn;
            free(node);
            c->reused = true;
            c->connecting = false;
            buf_reset(&c->rbuf);
            *out_c = c;
            return 0;
        }
        conn_destroy(node->conn);
        free(node);
    }
    return conn_new(b, out_c);
}

int upstream_connect_result(upstream_conn_t *c)
{
    int err = 0;
    socklen_t len = sizeof(err);
    if (c == NULL) { errno = EINVAL; return -1; }
    if (!c->connecting) return 0;
    if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) return -1;
    if (err != 0) { errno = err; return -1; }
    c->connecting = false;
    return 0;
}

void upstream_release(upstream_conn_t *c, bool reusable)
{
    int saved = errno;
    idle_node_t *node = NULL;
    backend_t *b;
    if (c == NULL) return;
    b = c->backend;
    if (reusable && !c->connecting && b != NULL) {
        node = malloc(sizeof(*node));
        if (node != NULL) {
            pthread_mutex_lock(&b->lock);
            if (b->idle_count < b->idle_limit) {
                buf_reset(&c->rbuf);
                node->conn = c;
                node->next = b->idle;
                b->idle = node;
                b->idle_count++;
                pthread_mutex_unlock(&b->lock);
                errno = saved;
                return;
            }
            pthread_mutex_unlock(&b->lock);
            free(node);
        }
    }
    conn_destroy(c);
    errno = saved;
}
