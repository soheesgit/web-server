/* src/common/http_status.c
 *
 * 사유 문구와 에러 페이지. 명세: http_status.h
 */
#include "common/http_status.h"

#include <stddef.h>

const char *http_reason(int status)
{
    switch (status) {
    case 100:
        return "Continue";
    case 200:
        return "OK";
    case 201:
        return "Created";
    case 204:
        return "No Content";
    case 301:
        return "Moved Permanently";
    case 302:
        return "Found";
    case 304:
        return "Not Modified";
    case 400:
        return "Bad Request";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 408:
        return "Request Timeout";
    case 413:
        return "Content Too Large";
    case 414:
        return "URI Too Long";
    case 431:
        return "Request Header Fields Too Large";
    case 500:
        return "Internal Server Error";
    case 501:
        return "Not Implemented";
    case 502:
        return "Bad Gateway";
    case 503:
        return "Service Unavailable";
    case 504:
        return "Gateway Timeout";
    case 505:
        return "HTTP Version Not Supported";
    default:
        return "Unknown";
    }
}

int http_error_body(int status, buf_t *out)
{
    const char *reason = http_reason(status);

    /* buf_appendf는 실패하면 out을 바꾸지 않는다 */
    return buf_appendf(out, "<!doctype html><title>%03d %s</title><h1>%03d %s</h1>\n", status,
                       reason, status, reason);
}
