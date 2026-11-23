/* src/common/http_status.c
 *
 * 사유 문구와 에러 페이지. 명세: http_status.h
 */
#include "common/http_status.h"
#include "core/stub.h"

const char *http_reason(int status)
{
    UNUSED(status);
    /* TODO(common): 구현 — U-DS-30 */
    STUB_RETURN("");
}

int http_error_body(int status, buf_t *out)
{
    UNUSED(status);
    UNUSED(out);
    /* TODO(common): 구현 — U-HD-02, E-ST-04 */
    STUB_RETURN(-1);
}
