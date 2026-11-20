/* src/downstream/response_writer.c
 *
 * 응답 머리 직렬화. 명세: response_writer.h
 */
#include "downstream/response_writer.h"
#include "core/stub.h"

int response_write_head(const http_head_out_t *head, int client_minor, time_t now, buf_t *out)
{
    UNUSED(head);
    UNUSED(client_minor);
    UNUSED(now);
    UNUSED(out);
    /* TODO(downstream): 구현 — U-DS-30, E-ST-01, E-KA-03, E-KA-04, E-KA-05, E-KA-07, E-CG-08 */
    STUB_RETURN(-1);
}

int response_write_chunk(const void *p, size_t n, buf_t *out)
{
    UNUSED(p);
    UNUSED(n);
    UNUSED(out);
    /* TODO(downstream): 구현 — E-CG-08 */
    STUB_RETURN(-1);
}

int response_write_last_chunk(buf_t *out)
{
    UNUSED(out);
    /* TODO(downstream): 구현 — E-CG-08 */
    STUB_RETURN(-1);
}
