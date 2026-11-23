/* src/common/http_status.c
 *
 * 사유 문구와 에러 페이지. 명세: http_status.h
 */
#include "common/http_status.h"

#include <errno.h>

/* 역할: HTTP 상태 코드에 대응하는 고정 사유 문구를 돌려준다. */
const char *http_reason(int status)
{
    /* 상태 줄과 에러 페이지가 함께 쓰는 고정 사유 문구 표다. */
    switch (status) {
    case 100: return "Continue";
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default: return "Unknown";
    }
}

/* 역할: 상태 코드와 사유 문구로 기본 HTML 에러 본문을 버퍼에 만든다. */
int http_error_body(int status, buf_t *out)
{
    size_t old_len;
    const char *reason;
    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    /* 문구를 재사용해 HTML 에러 본문을 만들고, 덧붙이기 실패 시 버퍼 길이를 복구한다. */
    old_len = out->len;
    reason = http_reason(status);
    if (buf_appendf(out, "<!doctype html><title>%03d %s</title><h1>%03d %s</h1>\n", status,
                    reason, status, reason) != 0) {
        out->len = old_len;
        return -1;
    }
    return 0;
}
