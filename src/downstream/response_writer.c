/* 응답 머리와 chunked 본문 직렬화. */
#include "downstream/response_writer.h"

#include <errno.h>
#include <inttypes.h>

#include "common/http_status.h"

static int append_date(buf_t *out, time_t now)
{
    /* 로캘에 따라 요일과 월 이름이 달라지지 않도록 HTTP 표기의 영문 약자를 쓴다. */
    static const char *const weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    struct tm utc;
    if (gmtime_r(&now, &utc) == NULL || utc.tm_year < -1900 || utc.tm_year > 8099) {
        errno = EINVAL;
        return -1;
    }
    return buf_appendf(out, "Date: %s, %02d %s %04d %02d:%02d:%02d GMT\r\n", weekdays[utc.tm_wday],
                       utc.tm_mday, months[utc.tm_mon], utc.tm_year + 1900, utc.tm_hour, utc.tm_min,
                       utc.tm_sec);
}

static int append_framing(const http_head_out_t *head, int minor, buf_t *out)
{
    int status = head->status;
    bool body_allowed = status >= 200 && status != 204 && status != 304;
    if (body_allowed && head->framing.kind == FRAMING_LENGTH) {
        return buf_appendf(out, "Content-Length: %" PRIu64 "\r\n", head->framing.length);
    }
    if (body_allowed && head->framing.kind == FRAMING_CHUNKED && minor >= 1) {
        return buf_append(out, "Transfer-Encoding: chunked\r\n",
                          sizeof("Transfer-Encoding: chunked\r\n") - 1);
    }
    return 0;
}

static int append_connection(const http_head_out_t *head, int minor, buf_t *out)
{
    /* HTTP/1.0에는 chunked 헤더를 보낼 수 없어 이런 응답은 연결 종료로 경계를 알린다. */
    if (head->close || head->framing.kind == FRAMING_CLOSE ||
        (minor == 0 && head->framing.kind == FRAMING_CHUNKED)) {
        return buf_append(out, "Connection: close\r\n", sizeof("Connection: close\r\n") - 1);
    }
    if (minor == 0) {
        return buf_append(out, "Connection: keep-alive\r\n",
                          sizeof("Connection: keep-alive\r\n") - 1);
    }
    return 0;
}

int response_write_head(const http_head_out_t *head, int client_minor, time_t now, buf_t *out)
{
    if (head->status < 100 || head->status > 599) {
        errno = EINVAL;
        return -1;
    }

    /* 기존 버퍼 뒤에 응답 머리를 붙인다. 한 줄이라도 실패하면 이번 머리를 모두 취소한다. */
    size_t start = out->len;
    if (buf_appendf(out, "HTTP/1.1 %03d %s\r\n", head->status, http_reason(head->status)) < 0 ||
        append_date(out, now) < 0 ||
        buf_append(out, "Server: httpd/0.1\r\n", sizeof("Server: httpd/0.1\r\n") - 1) < 0 ||
        buf_append(out, head->fields.data, head->fields.len) < 0 ||
        append_framing(head, client_minor, out) < 0 ||
        append_connection(head, client_minor, out) < 0 || buf_append(out, "\r\n", 2) < 0) {
        out->len = start;
        return -1;
    }
    return 0;
}

int response_write_chunk(const void *p, size_t n, buf_t *out)
{
    /* 길이 0인 데이터는 건너뛴다. 전송 종료 표시는 호출자가 따로 붙인다. */
    if (n == 0) {
        return 0;
    }
    size_t start = out->len;
    if (buf_appendf(out, "%zx\r\n", n) < 0 || buf_append(out, p, n) < 0 ||
        buf_append(out, "\r\n", 2) < 0) {
        out->len = start;
        return -1;
    }
    return 0;
}

int response_write_last_chunk(buf_t *out)
{
    return buf_append(out, "0\r\n\r\n", 5);
}
