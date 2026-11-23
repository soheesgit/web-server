/* src/downstream/response_writer.c
 *
 * 응답 머리 직렬화. 명세: response_writer.h
 */
#include "downstream/response_writer.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "common/http_status.h"

/* HTTP 규칙상 응답 본문이 없는 상태 코드인지 검사한다. */
static bool status_has_no_body(int status)
{
    return (status >= 100 && status < 200) || status == 204 || status == 304;
}

/* 역할: 상태 줄, Date/Server, 응답 헤더, framing, 연결 정책을 한 HTTP 머리로 만든다. */
int response_write_head(const http_head_out_t *head, int client_minor, time_t now, buf_t *out)
{
    static const char *const weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char date[40];
    struct tm tm;
    int year;
    int date_len;
    size_t old_len;
    bool no_body;
    bool close;

    /* 입력 상태와 framing을 확인한 뒤, 실패 전 버퍼 길이를 저장해 원자적으로 덧붙인다. */
    if (head == NULL || out == NULL || head->status < 100 || head->status > 599 ||
        (head->fields.len != 0 && head->fields.data == NULL)) {
        errno = EINVAL;
        return -1;
    }
    if (head->framing.kind < FRAMING_NONE || head->framing.kind > FRAMING_CLOSE) {
        errno = EINVAL;
        return -1;
    }
    if (gmtime_r(&now, &tm) == NULL || tm.tm_wday < 0 || tm.tm_wday > 6 || tm.tm_mon < 0 ||
        tm.tm_mon > 11) {
        errno = EINVAL;
        return -1;
    }
    /* Date는 로케일에 영향받지 않도록 영문 요일·월 이름 표로 만든다. */
    year = tm.tm_year + 1900;
    if (year < 0 || year > 9999) {
        errno = EINVAL;
        return -1;
    }
    date_len =
        snprintf(date, sizeof(date), "%s, %02d %s %04d %02d:%02d:%02d GMT", weekdays[tm.tm_wday],
                 tm.tm_mday, months[tm.tm_mon], year, tm.tm_hour, tm.tm_min, tm.tm_sec);
    if (date_len < 0 || (size_t)date_len >= sizeof(date)) {
        errno = EINVAL;
        return -1;
    }
    /* 상태 줄과 고정 헤더를 먼저 쓴 뒤, 응답 계획의 사용자 헤더를 순서대로 붙인다. */
    old_len = out->len;
    no_body = status_has_no_body(head->status);
    close = head->close || head->framing.kind == FRAMING_CLOSE ||
            (head->framing.kind == FRAMING_CHUNKED && client_minor == 0);
    if (buf_appendf(out, "HTTP/1.1 %03d %s\r\nDate: %s\r\nServer: httpd/0.1\r\n", head->status,
                    http_reason(head->status), date) != 0 ||
        buf_append(out, head->fields.data, head->fields.len) != 0) {
        out->len = old_len;
        return -1;
    }
    /* 본문이 있는 응답에만 framing 헤더를 추가한다. 1.0은 chunked를 알 수 없어 종료로 구분한다. */
    if (!no_body) {
        if (head->framing.kind == FRAMING_LENGTH) {
            if (buf_appendf(out, "Content-Length: %llu\r\n",
                            (unsigned long long)head->framing.length) != 0) {
                out->len = old_len;
                return -1;
            }
        } else if (head->framing.kind == FRAMING_CHUNKED && client_minor >= 1) {
            if (buf_append(out, "Transfer-Encoding: chunked\r\n", 28) != 0) {
                out->len = old_len;
                return -1;
            }
        }
    }
    /* 연결 정책은 close가 우선이고, HTTP/1.0 유지일 때만 keep-alive를 명시한다. */
    if (close) {
        if (buf_append(out, "Connection: close\r\n", 19) != 0) {
            out->len = old_len;
            return -1;
        }
    } else if (client_minor == 0) {
        if (buf_append(out, "Connection: keep-alive\r\n", 24) != 0) {
            out->len = old_len;
            return -1;
        }
    }
    if (buf_append(out, "\r\n", 2) != 0) {
        out->len = old_len;
        return -1;
    }
    return 0;
}

/* 역할: 본문 데이터 한 조각을 chunked 전송 형식으로 감싸 버퍼에 붙인다. */
int response_write_chunk(const void *p, size_t n, buf_t *out)
{
    size_t old_len;
    if (out == NULL || (n != 0 && p == NULL)) {
        errno = EINVAL;
        return -1;
    }
    if (n == 0) {
        return 0;
    }
    /* chunked wire 형식은 크기 줄, 데이터, 뒤 CRLF 순서다. 실패 시 부분 결과를 지운다. */
    old_len = out->len;
    if (buf_appendf(out, "%llx\r\n", (unsigned long long)n) != 0 || buf_append(out, p, n) != 0 ||
        buf_append(out, "\r\n", 2) != 0) {
        out->len = old_len;
        return -1;
    }
    return 0;
}

/* 역할: chunked 본문 종료 마커인 0\r\n\r\n을 붙인다. */
int response_write_last_chunk(buf_t *out)
{
    size_t old_len;
    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    /* 청크 전송 종료를 알리는 0 크기 청크와 빈 줄을 붙인다. */
    old_len = out->len;
    if (buf_append(out, "0\r\n\r\n", 5) != 0) {
        out->len = old_len;
        return -1;
    }
    return 0;
}
