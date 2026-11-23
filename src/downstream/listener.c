/* IPv4 대기 소켓과 accept. */
/* GNU 확장 대신 POSIX strerror_r를 사용해 오류 문자열을 버퍼로 받는다. */
#undef _GNU_SOURCE
#include "downstream/listener.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "core/log.h"

static int set_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD);
    return flags < 0 ? -1 : fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static int open_error(int fd, const char *step, char *err, size_t errcap)
{
    /* 닫기와 메시지 작성 중 errno가 바뀌어도 처음 실패한 단계의 값을 돌려준다. */
    int saved = errno;
    if (fd >= 0) {
        (void)close(fd);
    }
    if (errcap != 0) {
        char message[128];
        if (strerror_r(saved, message, sizeof(message)) == 0) {
            (void)snprintf(err, errcap, "%s: %s", step, message);
        } else {
            (void)snprintf(err, errcap, "%s: errno %d", step, saved);
        }
    }
    errno = saved;
    return -1;
}

int listener_open(const char *host, uint16_t port, int backlog, char *err, size_t errcap)
{
    /* AI_PASSIVE는 host가 NULL일 때 모든 IPv4 인터페이스에 바인딩할 주소를 고른다. */
    struct addrinfo hints = {
        .ai_flags = AI_PASSIVE, .ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *addresses;
    struct sockaddr_in address;
    char service[6];
    (void)snprintf(service, sizeof(service), "%u", (unsigned)port);
    int resolved = getaddrinfo(host, service, &hints, &addresses);
    if (resolved != 0) {
        int saved = resolved == EAI_SYSTEM ? errno : (resolved == EAI_MEMORY ? ENOMEM : EINVAL);
        if (errcap != 0) {
            (void)snprintf(err, errcap, "getaddrinfo: %s", gai_strerror(resolved));
        }
        errno = saved;
        return -1;
    }
    memcpy(&address, addresses->ai_addr, sizeof(address));
    freeaddrinfo(addresses);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return open_error(fd, "socket", err, errcap);
    }
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 || set_cloexec(fd) < 0) {
        return open_error(fd, "fcntl", err, errcap);
    }
    int reuse = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        return open_error(fd, "setsockopt", err, errcap);
    }
    if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) < 0) {
        return open_error(fd, "bind", err, errcap);
    }
    if (listen(fd, backlog >= 1 ? backlog : 128) < 0) {
        return open_error(fd, "listen", err, errcap);
    }
    return fd;
}

int listener_accept(int lfd, int *out_cfd, struct sockaddr_storage *out_peer)
{
    struct sockaddr_storage peer = {0};
    socklen_t length;
    int cfd;
    do {
        length = sizeof(peer);
        cfd = accept(lfd, (struct sockaddr *)&peer, &length);
    } while (cfd < 0 && errno == EINTR);
    if (cfd < 0) {
        int saved = errno;
        if (saved == EMFILE || saved == ENFILE) {
            log_msg(LOG_WARN, "accept: fd 한도 (errno %d)", saved);
        }
        /* 수락 전에 사라진 연결은 대기 큐가 비었을 때처럼 호출자가 다시 시도한다. */
        errno = saved == ECONNABORTED ? EAGAIN : saved;
        return -1;
    }

    /* D-036의 POSIX 경로다. 수락한 소켓의 blocking 모드는 conn_new에서 정한다. */
    if (set_cloexec(cfd) < 0) {
        listener_close(cfd);
        return -1;
    }
    if (out_peer != NULL) {
        *out_peer = peer;
    }
    *out_cfd = cfd;
    return 0;
}

int listener_port(int lfd)
{
    /* 포트 0으로 바인딩했으면 커널이 정한 실제 포트는 getsockname으로 조회한다. */
    struct sockaddr_storage address;
    socklen_t length = sizeof(address);
    if (getsockname(lfd, (struct sockaddr *)&address, &length) < 0) {
        return -1;
    }
    if (address.ss_family != AF_INET || length < sizeof(struct sockaddr_in)) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    const struct sockaddr_in *ipv4 = (const struct sockaddr_in *)&address;
    return ntohs(ipv4->sin_port);
}

void listener_close(int lfd)
{
    int saved = errno;
    if (lfd >= 0) {
        (void)close(lfd);
    }
    errno = saved;
}
