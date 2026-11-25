/* 목록에서 분리한 뒤에만 검사하거나 닫아 정리 스레드와 소유권이 겹치지 않는다. */
#include "upstream/internal_state.h"

#include <errno.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include "core/platform.h"
#include "upstream/internal_demo_trace.h"

static void close_entry(pool_entry_t *p)
{
    if (p->conn.fd >= 0) {
        close(p->conn.fd);
    }
    buf_cleanup(&p->conn.rbuf);
    free(p);
}

void upstream_discard_idle(pool_entry_t *entries)
{
    while (entries != NULL) {
        pool_entry_t *next = entries->next;
        close_entry(entries);
        entries = next;
    }
}

static bool socket_idle(int fd)
{
    char octet;
    ssize_t n;
    do {
        n = recv(fd, &octet, 1, MSG_PEEK);
    } while (n < 0 && errno == EINTR);
    return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
}

static pool_entry_t *pop_idle(backend_t *b)
{
    pool_entry_t *p = b->idle;
    if (p != NULL) {
        b->idle = p->next;
        b->idle_count--;
        p->next = NULL;
    }
    return p;
}

static int open_backend(pool_entry_t *p)
{
    backend_t *b = p->conn.backend;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    p->conn.fd = fd;
    int one = 1;
    if (platform_set_cloexec(fd) != 0 || platform_set_nonblock(fd) != 0 ||
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0) {
        return -1;
    }

    pthread_mutex_lock(&b->group->lock);
    b->sockets++;
    pthread_mutex_unlock(&b->group->lock);
    int rc = connect(fd, (const struct sockaddr *)&b->address, sizeof(b->address));
    if (rc == 0 || errno == EINPROGRESS || errno == EINTR) {
        p->conn.connecting = rc != 0;
        return 0;
    }
    return -1;
}

static bool is_endpoint_error(int error)
{
    return error == ECONNREFUSED || error == ETIMEDOUT || error == EHOSTUNREACH ||
           error == ENETUNREACH || error == ECONNRESET;
}

int upstream_checkout(upstream_group_t *g, backend_t *pinned, const backend_t *exclude,
                      backend_t **out_selected, upstream_conn_t **out)
{
    int64_t now = platform_now_ms();
    pthread_mutex_lock(&g->lock);
    backend_t *b = pinned != NULL ? pinned : upstream_select_locked(g, exclude, now);
    *out_selected = NULL;
    if (b == NULL || !upstream_eligible(b, now)) {
        pthread_mutex_unlock(&g->lock);
        errno = EAGAIN;
        return -1;
    }
    *out_selected = b;
    if (b->health == UP_COOLDOWN) {
        b->health = UP_RECOVERING;
        b->recovery_limit = 1;
        b->recovery_successes = 0;
    }
    b->active++;
    b->assigned++;
    if (b->active > b->peak_active) {
        b->peak_active = b->active;
    }
    pool_entry_t *idle = pop_idle(b);
    pthread_mutex_unlock(&g->lock);

    while (idle != NULL) {
        bool expired = platform_now_ms() - idle->idle_since >= UPSTREAM_IDLE_MS;
        if (!expired && socket_idle(idle->conn.fd)) {
            idle->conn.reused = true;
            pthread_mutex_lock(&g->lock);
            b->reused++;
            pthread_mutex_unlock(&g->lock);
            *out = &idle->conn;
            return 0;
        }
        close_entry(idle);
        pthread_mutex_lock(&g->lock);
        if (expired) {
            b->expired++;
        }
        idle = pop_idle(b);
        pthread_mutex_unlock(&g->lock);
    }

    pool_entry_t *p = calloc(1, sizeof(*p));
    if (p != NULL) {
        p->conn.fd = -1;
        p->conn.backend = b;
        if (buf_init(&p->conn.rbuf, 4096, 1u << 20) == 0 && open_backend(p) == 0) {
            *out = &p->conn;
            return 0;
        }
    }
    int saved = errno;
    if (is_endpoint_error(saved)) {
        upstream_report(b, false, platform_now_ms());
    }
    if (p != NULL) {
        close_entry(p);
    }
    pthread_mutex_lock(&g->lock);
    b->active--;
    pthread_mutex_unlock(&g->lock);
    errno = saved;
    return -1;
}

int upstream_acquire(upstream_group_t *g, backend_t *b, upstream_conn_t **out_c)
{
    backend_t *selected;
    return upstream_checkout(g, b, NULL, &selected, out_c);
}

int upstream_reconnect(upstream_conn_t *c)
{
    close(c->fd);
    c->fd = -1;
    c->reused = false;
    c->connecting = false;
    buf_reset(&c->rbuf);
    return open_backend((pool_entry_t *)c);
}

int upstream_connect_result(upstream_conn_t *c)
{
    if (!c->connecting) {
        return 0;
    }
    int error = 0;
    socklen_t len = sizeof(error);
    if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &error, &len) != 0) {
        return -1;
    }
    if (error != 0) {
        errno = error;
        return -1;
    }
    c->connecting = false;
    return 0;
}

void upstream_release(upstream_conn_t *c, bool reusable)
{
    if (c == NULL) {
        return;
    }
    pool_entry_t *p = (pool_entry_t *)c;
    backend_t *b = c->backend;
    upstream_group_t *g = b->group;
    int saved = errno;
    bool keep = reusable && !c->connecting && socket_idle(c->fd);

    if (keep && c->rbuf.cap > 16384) {
        buf_cleanup(&c->rbuf);
        (void)buf_init(&c->rbuf, 0, 1u << 20);
    }
    pthread_mutex_lock(&g->lock);
    b->active--;
    keep = keep && b->health != UP_COOLDOWN && b->idle_count < g->max_idle;
    if (keep) {
        buf_reset(&c->rbuf);
        p->idle_since = platform_now_ms();
        p->next = b->idle;
        b->idle = p;
        b->idle_count++;
    }
    pthread_mutex_unlock(&g->lock);
    if (!keep) {
        close_entry(p);
    }
    demo_trace_state(g);
    errno = saved;
}
