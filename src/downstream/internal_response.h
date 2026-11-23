#ifndef HTTPD_DOWNSTREAM_INTERNAL_RESPONSE_H
#define HTTPD_DOWNSTREAM_INTERNAL_RESPONSE_H

#include <stdbool.h>

/** HTTP 응답 상태가 본문을 허용하지 않는지 판정한다. */
static inline bool response_status_has_no_body(int status)
{
    return (status >= 100 && status < 200) || status == 204 || status == 304;
}

#endif
