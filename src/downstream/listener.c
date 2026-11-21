/* src/downstream/listener.c
 *
 * 대기 소켓. 명세: listener.h
 */
#include "downstream/listener.h"

#include "core/log.h"
#include "core/platform.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* err에 "<stage>: <메시지>"를 쓴다. errno는 바꾸지 않는다. */
static void set_err(char *err, size_t errcap, const char *stage, const char *msg)
{
    if (errcap > 0) {
        snprintf(err, errcap, "%s: %s", stage, msg);
    }
}

static void set_errno_err(char *err, size_t errcap, const char *stage, int e)
{
    char msg[128];
    const char *s;

#if defined(__GLIBC__) && defined(_GNU_SOURCE)
    s = strerror_r(e, msg, sizeof(msg)); /* GNU 판은 char *를 돌려준다 */
#else
    s = strerror_r(e, msg, sizeof(msg)) == 0 ? msg : "unknown error";
#endif
    set_err(err, errcap, stage, s);
    errno = e;
}

/* host를 IPv4 주소로 바꾼다. 실패하면 err를 채우고 -1. */
static int resolve(const char *host, struct in_addr *out, char *err, size_t errcap)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    int rc;

    if (inet_pton(AF_INET, host, out) == 1) {
        return 0;
    }
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc != 0) {
        set_err(err, errcap, "getaddrinfo", gai_strerror(rc));
        errno = EINVAL;
        return -1;
    }
    *out = ((const struct sockaddr_in *)(const void *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return 0;
}

int listener_open(const char *host, uint16_t port, int backlog, char *err, size_t errcap)
{
    struct sockaddr_in sa;
    int one = 1;
    int fd;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (resolve(host != NULL ? host : "0.0.0.0", &sa.sin_addr, err, errcap) != 0) {
        return -1;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        set_errno_err(err, errcap, "socket", errno);
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) {
        set_errno_err(err, errcap, "setsockopt", errno);
    } else if (bind(fd, (const struct sockaddr *)&sa, sizeof(sa)) != 0) {
        set_errno_err(err, errcap, "bind", errno);
    } else if (listen(fd, backlog < 1 ? 128 : backlog) != 0) {
        set_errno_err(err, errcap, "listen", errno);
    } else if (platform_set_nonblock(fd) != 0) {
        set_errno_err(err, errcap, "fcntl", errno);
    } else if (platform_set_cloexec(fd) != 0) {
        set_errno_err(err, errcap, "fcntl", errno);
    } else {
        return fd;
    }
    listener_close(fd);
    return -1;
}

int listener_accept(int lfd, int *out_cfd, struct sockaddr_storage *out_peer)
{
    struct sockaddr_storage peer;
    socklen_t sl;
    int cfd;

    do {
        sl = sizeof(peer);
#ifdef __linux__
        cfd = accept4(lfd, (struct sockaddr *)&peer, &sl, SOCK_CLOEXEC);
#else
        cfd = accept(lfd, (struct sockaddr *)&peer, &sl);
#endif
    } while (cfd < 0 && errno == EINTR);

    if (cfd < 0) {
        if (errno == EMFILE || errno == ENFILE) {
            int e = errno;
            log_msg(LOG_WARN, "accept: out of file descriptors (errno %d)", e);
            errno = e;
        } else if (errno == ECONNABORTED) {
            errno = EAGAIN;
        }
        return -1;
    }
#ifndef __linux__
    if (platform_set_cloexec(cfd) != 0) {
        int e = errno;
        close(cfd);
        errno = e;
        return -1;
    }
#endif
    *out_cfd = cfd;
    if (out_peer != NULL) {
        *out_peer = peer;
    }
    return 0;
}

int listener_port(int lfd)
{
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);

    if (getsockname(lfd, (struct sockaddr *)&sa, &sl) != 0) {
        return -1;
    }
    return ntohs(sa.sin_port);
}

void listener_close(int lfd)
{
    int e = errno;

    if (lfd >= 0) {
        close(lfd);
    }
    errno = e;
}
