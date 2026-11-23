/* src/downstream/listener.c
 *
 * 대기 소켓. 명세: listener.h
 */
#include "downstream/listener.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "core/log.h"
#include "core/platform.h"

/* 시스템 오류 번호를 사람이 읽을 메시지와 함께 '<단계>: <오류>'로 기록한다. */
static void set_error(char *err, size_t errcap, const char *stage, int code)
{
    char message[128];
    char *message_text;

    if (err == NULL || errcap == 0) {
        return;
    }
    message[0] = '\0';
    message_text = strerror_r(code, message, sizeof(message));
    if (message_text == NULL) {
        message_text = "unknown error";
    }
    (void)snprintf(err, errcap, "%s: %s", stage, message_text);
}

/* 역할: 지정 주소/포트에 non-blocking TCP 대기 소켓을 열고 listen 상태로 만든다. */
int listener_open(const char *host, uint16_t port, int backlog, char *err, size_t errcap)
{
    struct addrinfo hints;
    struct addrinfo *results = NULL;
    struct addrinfo *ai;
    char service[6];
    int fd = -1;
    int rc;
    int saved_errno;
    /* 이름을 IPv4 주소로 해석한 뒤 socket → 옵션 → bind → listen 순서로 대기 소켓을 연다. */
    if (host == NULL) {
        host = "0.0.0.0";
    }
    (void)snprintf(service, sizeof(service), "%u", (unsigned)port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    /* getaddrinfo가 돌려준 첫 사용 가능한 IPv4 주소를 선택한다. */
    rc = getaddrinfo(host, service, &hints, &results);
    if (rc != 0) {
        if (err != NULL && errcap > 0) {
            (void)snprintf(err, errcap, "getaddrinfo: %s", gai_strerror(rc));
        }
        errno = EADDRNOTAVAIL;
        return -1;
    }
    for (ai = results; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd >= 0) {
            break;
        }
    }
    if (fd < 0) {
        saved_errno = errno;
        set_error(err, errcap, "socket", saved_errno);
        freeaddrinfo(results);
        errno = saved_errno;
        return -1;
    }
    /* 포트 재사용을 허용하고, 프로세스 상속을 막은 non-blocking fd를 만든다. */
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) {
        saved_errno = errno;
        set_error(err, errcap, "setsockopt", saved_errno);
        goto fail;
    }
    if (platform_set_cloexec(fd) != 0 || platform_set_nonblock(fd) != 0) {
        saved_errno = errno;
        set_error(err, errcap, "fcntl", saved_errno);
        goto fail;
    }
    if (bind(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
        saved_errno = errno;
        set_error(err, errcap, "bind", saved_errno);
        goto fail;
    }
    if (backlog < 1) {
        backlog = 128;
    }
    if (listen(fd, backlog) != 0) {
        saved_errno = errno;
        set_error(err, errcap, "listen", saved_errno);
        goto fail;
    }
    freeaddrinfo(results);
    if (err != NULL && errcap > 0) {
        err[0] = '\0';
    }
    return fd;

fail:
    /* 중간 단계에서 실패하면 이 함수가 만든 fd와 주소 목록을 모두 정리한다. */
    close(fd);
    freeaddrinfo(results);
    errno = saved_errno;
    return -1;
}

/* 역할: 대기 소켓에서 연결 하나를 accept하고 상대 주소와 cloexec fd를 돌려준다. */
int listener_accept(int lfd, int *out_cfd, struct sockaddr_storage *out_peer)
{
    struct sockaddr_storage local_peer;
    socklen_t peer_len;
    int cfd;
    /* EINTR은 다시 시도하고, 연결 소켓에는 close-on-exec만 설정한다. */
    if (out_cfd == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (out_peer == NULL) {
        out_peer = &local_peer;
    }
    peer_len = sizeof(*out_peer);
    do {
        cfd = accept(lfd, (struct sockaddr *)out_peer, &peer_len);
    } while (cfd < 0 && errno == EINTR);
    if (cfd < 0) {
        if (errno == ECONNABORTED) {
            errno = EAGAIN;
        } else if (errno == EMFILE || errno == ENFILE) {
            log_msg(LOG_WARN, "accept: file descriptor limit reached");
        }
        return -1;
    }
    if (platform_set_cloexec(cfd) != 0) {
        int saved_errno = errno;
        close(cfd);
        errno = saved_errno;
        return -1;
    }
    *out_cfd = cfd;
    return 0;
}

/* 역할: bind된 대기 소켓에서 커널이 선택한 실제 포트를 조회한다. */
int listener_port(int lfd)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    /* bind 후 커널이 정한 포트(특히 port=0으로 연 경우)를 getsockname으로 조회한다. */
    if (getsockname(lfd, (struct sockaddr *)&addr, &len) != 0) {
        return -1;
    }
    if (addr.sin_family != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    return (int)ntohs(addr.sin_port);
}

/* 역할: 대기 소켓을 닫는다. 음수 fd는 그대로 두고 errno도 보존한다. */
void listener_close(int lfd)
{
    /* 닫기 전 errno를 보관해 정리 함수가 호출자 오류 상태를 덮지 않게 한다. */
    int saved_errno = errno;
    if (lfd >= 0) {
        (void)close(lfd);
    }
    errno = saved_errno;
}
