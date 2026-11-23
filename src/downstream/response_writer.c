/* src/downstream/response_writer.c
 *
 * 응답 머리 직렬화. 명세: response_writer.h
 */
#include "downstream/response_writer.h"

#include "common/http_status.h"
#include "downstream/internal_response.h"

#include <errno.h>
#include <stdbool.h>

/* IMF-fixdate. strftime의 %a·%b는 로캘을 따르므로 이름 표를 직접 쓴다. */
static int append_date(buf_t *out, time_t now)
{
    static const char *const wday[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    struct tm tm;

    if (gmtime_r(&now, &tm) == NULL || tm.tm_year < -1900 || tm.tm_year > 8099) {
        errno = EINVAL;
        return -1;
    }
    return buf_appendf(out, "Date: %s, %02d %s %04d %02d:%02d:%02d GMT\r\n", wday[tm.tm_wday],
                       tm.tm_mday, mon[tm.tm_mon], tm.tm_year + 1900, tm.tm_hour, tm.tm_min,
                       tm.tm_sec);
}

static int write_head(const http_head_out_t *head, int client_minor, time_t now, buf_t *out)
{
    int status = head->status;
    http_framing_kind_t kind = head->framing.kind;
    bool no_body = response_status_has_no_body(status);
    bool close =
        head->close || (kind == FRAMING_CHUNKED && client_minor == 0) || kind == FRAMING_CLOSE;

    if (buf_appendf(out, "HTTP/1.1 %03d %s\r\n", status, http_reason(status)) != 0 ||
        append_date(out, now) != 0 || buf_appendf(out, "Server: httpd/0.1\r\n") != 0 ||
        buf_append(out, head->fields.data, head->fields.len) != 0) {
        return -1;
    }
    if (kind == FRAMING_LENGTH && !no_body) {
        if (buf_appendf(out, "Content-Length: %llu\r\n",
                        (unsigned long long)head->framing.length) != 0) {
            return -1;
        }
    } else if (kind == FRAMING_CHUNKED && !no_body && client_minor != 0) {
        if (buf_appendf(out, "Transfer-Encoding: chunked\r\n") != 0) {
            return -1;
        }
    }
    if (close) {
        if (buf_appendf(out, "Connection: close\r\n") != 0) {
            return -1;
        }
    } else if (client_minor == 0) {
        if (buf_appendf(out, "Connection: keep-alive\r\n") != 0) {
            return -1;
        }
    }
    return buf_append(out, "\r\n", 2);
}

int response_write_head(const http_head_out_t *head, int client_minor, time_t now, buf_t *out)
{
    size_t saved = out->len;

    if (head->status < 100 || head->status > 599) {
        errno = EINVAL;
        return -1;
    }
    if (write_head(head, client_minor, now, out) != 0) {
        out->len = saved;
        return -1;
    }
    return 0;
}

int response_write_chunk(const void *p, size_t n, buf_t *out)
{
    size_t saved = out->len;

    if (n == 0) {
        return 0;
    }
    if (buf_appendf(out, "%zx\r\n", n) != 0 || buf_append(out, p, n) != 0 ||
        buf_append(out, "\r\n", 2) != 0) {
        out->len = saved;
        return -1;
    }
    return 0;
}

int response_write_last_chunk(buf_t *out)
{
    return buf_append(out, "0\r\n\r\n", 5);
}
