#include "downstream/listener.h"

#include "core/log.h"
#include "core/platform.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int report_failure(int fd, const char *step, int error, char *err, size_t errcap)
{
    listener_close(fd);
    if (errcap != 0) {
        char detail[128];
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
        const char *message = strerror_r(error, detail, sizeof(detail));
#else
        const char *message = strerror_r(error, detail, sizeof(detail)) == 0 ? detail : "unknown";
#endif
        snprintf(err, errcap, "%s: %s", step, message);
    }
    errno = error;
    return -1;
}

int listener_open(const char *host, uint16_t port, int backlog, char *err, size_t errcap)
{
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
        .ai_flags = AI_PASSIVE | AI_NUMERICSERV,
    };
    struct addrinfo *address;
    char service[6];
    snprintf(service, sizeof(service), "%u", (unsigned)port);
    int gai = getaddrinfo(host, service, &hints, &address);
    if (gai != 0) {
        int error = gai == EAI_SYSTEM ? errno : (gai == EAI_MEMORY ? ENOMEM : EADDRNOTAVAIL);
        if (errcap != 0) {
            snprintf(err, errcap, "getaddrinfo: %s", gai_strerror(gai));
        }
        errno = error;
        return -1;
    }

    int fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    int reuse = 1;
    const char *failure = NULL;
    if (fd < 0) {
        failure = "socket";
    } else if (platform_set_cloexec(fd) < 0 || platform_set_nonblock(fd) < 0) {
        failure = "fcntl";
    } else if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        failure = "setsockopt";
    } else if (bind(fd, address->ai_addr, address->ai_addrlen) < 0) {
        failure = "bind";
    } else if (listen(fd, backlog < 1 ? 128 : backlog) < 0) {
        failure = "listen";
    }
    int error = errno;
    freeaddrinfo(address);
    return failure == NULL ? fd : report_failure(fd, failure, error, err, errcap);
}

int listener_accept(int lfd, int *out_cfd, struct sockaddr_storage *out_peer)
{
    struct sockaddr_storage peer = {0};
    socklen_t size;
    int fd;
    do {
        size = sizeof(peer);
#ifdef __linux__
        fd = accept4(lfd, (struct sockaddr *)&peer, &size, SOCK_CLOEXEC);
#else
        fd = accept(lfd, (struct sockaddr *)&peer, &size);
#endif
    } while (fd < 0 && errno == EINTR);

    if (fd < 0) {
        int error = errno;
        if (error == EMFILE || error == ENFILE) {
            log_msg(LOG_WARN, "accept: file descriptor limit reached");
        }
        errno = error == ECONNABORTED ? EAGAIN : error;
        return -1;
    }
#ifndef __linux__
    if (platform_set_cloexec(fd) < 0) {
        listener_close(fd);
        return -1;
    }
#endif

    if (out_peer != NULL) {
        *out_peer = peer;
    }
    *out_cfd = fd;
    return 0;
}

int listener_port(int lfd)
{
    struct sockaddr_in address;
    socklen_t size = sizeof(address);
    if (getsockname(lfd, (struct sockaddr *)&address, &size) < 0) {
        return -1;
    }
    return ntohs(address.sin_port);
}

void listener_close(int lfd)
{
    if (lfd >= 0) {
        int error = errno;
        close(lfd);
        errno = error;
    }
}
