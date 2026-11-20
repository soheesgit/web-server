/* src/downstream/listener.c
 *
 * 대기 소켓. 명세: listener.h
 */
#include "downstream/listener.h"
#include "core/stub.h"

int listener_open(const char *host, uint16_t port, int backlog, char *err, size_t errcap)
{
    UNUSED(host);
    UNUSED(port);
    UNUSED(backlog);
    UNUSED(err);
    UNUSED(errcap);
    /* TODO(downstream): 구현 — U-DS-36, E-CF-03, E-CF-04 */
    STUB_RETURN(-1);
}

int listener_accept(int lfd, int *out_cfd, struct sockaddr_storage *out_peer)
{
    UNUSED(lfd);
    UNUSED(out_cfd);
    UNUSED(out_peer);
    /* TODO(downstream): 구현 — U-EN-02 */
    STUB_RETURN(-1);
}

int listener_port(int lfd)
{
    UNUSED(lfd);
    /* TODO(downstream): 구현 — U-DS-36 */
    STUB_RETURN(-1);
}

void listener_close(int lfd)
{
    UNUSED(lfd);
    /* TODO(downstream): 구현 — U-DS-36 */
}
