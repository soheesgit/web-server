/* HTTP 상태 문구와 공통 오류 응답. */
#include "common/http_status.h"

const char *http_reason(int status)
{
    static const char *const reasons[506] = {
        [100] = "Continue",
        [200] = "OK",
        [201] = "Created",
        [204] = "No Content",
        [301] = "Moved Permanently",
        [302] = "Found",
        [304] = "Not Modified",
        [400] = "Bad Request",
        [403] = "Forbidden",
        [404] = "Not Found",
        [405] = "Method Not Allowed",
        [408] = "Request Timeout",
        [413] = "Content Too Large",
        [414] = "URI Too Long",
        [431] = "Request Header Fields Too Large",
        [500] = "Internal Server Error",
        [501] = "Not Implemented",
        [502] = "Bad Gateway",
        [503] = "Service Unavailable",
        [504] = "Gateway Timeout",
        [505] = "HTTP Version Not Supported",
    };

    if (status < 0 || (size_t)status >= sizeof(reasons) / sizeof(*reasons) ||
        reasons[status] == NULL) {
        return "Unknown";
    }
    return reasons[status];
}

int http_error_body(int status, buf_t *out)
{
    /* 응답 상태 줄과 오류 페이지가 같은 상태 문구를 사용하도록 한곳에서 조회한다. */
    const char *reason = http_reason(status);
    const char *format = "<!doctype html><title>%03d %s</title><h1>%03d %s</h1>\n";
    return buf_appendf(out, format, status, reason, status, reason);
}
