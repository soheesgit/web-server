/* src/common/http_framing.c
 *
 * 본문 길이·경계 추적·keep-alive. 명세: http_framing.h
 */
#include "common/http_framing.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "common/http_headers.h"

/* HTTP 비교에는 로케일 대신 ASCII 대소문자 접기만 적용한다. */
static unsigned char lower_ascii(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

/* 헤더 값의 양끝 공백(SP, HTAB)을 잘라낸 부분 뷰를 만든다. */
static str_t trim_ows_local(str_t s)
{
    while (s.len > 0 && (s.ptr[0] == ' ' || s.ptr[0] == '\t')) {
        s.ptr++;
        s.len--;
    }
    while (s.len > 0 && (s.ptr[s.len - 1] == ' ' || s.ptr[s.len - 1] == '\t')) {
        s.len--;
    }
    return s;
}

/* 길이가 같은지 먼저 본 뒤 ASCII 대소문자 무시로 리터럴과 비교한다. */
static bool equals_ci(str_t s, const char *lit)
{
    size_t n = strlen(lit);
    if (s.len != n || (n != 0 && s.ptr == NULL)) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (lower_ascii((unsigned char)s.ptr[i]) != lower_ascii((unsigned char)lit[i])) {
            return false;
        }
    }
    return true;
}

/* Content-Length를 10진수로 읽는다. 곱셈 전 범위를 확인해 uint64_t 넘침을 막는다. */
static int parse_decimal(str_t s, uint64_t *out)
{
    uint64_t value = 0;
    if (s.len == 0 || s.ptr == NULL) {
        return -1;
    }
    for (size_t i = 0; i < s.len; i++) {
        unsigned char c = (unsigned char)s.ptr[i];
        unsigned digit;
        if (c < '0' || c > '9') {
            return -1;
        }
        digit = (unsigned)(c - '0');
        if (value > (UINT64_MAX - digit) / 10) {
            return -1;
        }
        value = value * 10 + digit;
    }
    *out = value;
    return 0;
}

/* 여러 Connection 헤더와 각 쉼표 목록에서 wanted 항목이 있는지 찾는다. */
static bool is_connection_token(const http_headers_t *h, const char *wanted)
{
    for (size_t i = 0; i < h->n && i < HTTP_MAX_HEADERS; i++) {
        str_t v;
        size_t pos = 0;
        if (!equals_ci(h->v[i].name, "Connection")) {
            continue;
        }
        v = h->v[i].value;
        while (pos <= v.len) {
            size_t end = pos;
            str_t item;
            while (end < v.len && v.ptr[end] != ',') {
                end++;
            }
            item.ptr = v.ptr == NULL ? NULL : v.ptr + pos;
            item.len = end - pos;
            item = trim_ows_local(item);
            if (equals_ci(item, wanted)) {
                return true;
            }
            if (end == v.len) {
                break;
            }
            pos = end + 1;
        }
    }
    return false;
}

/* 역할: CL/TE 헤더를 해석해 본문 경계 방식을 NONE/LENGTH/CHUNKED/CLOSE 중 하나로 정한다. */
int http_resolve_framing(const http_headers_t *h, bool is_response, int status,
                         http_method_t req_method, http_framing_t *out_f, int *out_status)
{
    bool has_te = false;
    bool has_cl = false;
    bool have_length = false;
    bool last_is_chunked = false;
    uint64_t length = 0;
    size_t coding_count = 0;

    if (h == NULL || out_f == NULL || out_status == NULL) {
        if (out_status != NULL) {
            *out_status = is_response ? 502 : 400;
        }
        errno = EINVAL;
        return -1;
    }
    /* HEAD와 본문 금지 상태는 Content-Length/Transfer-Encoding보다 먼저 NONE으로 결정한다. */
    if (is_response && (req_method == HTTP_HEAD || (status >= 100 && status < 200) || status == 204 ||
                        status == 304)) {
        out_f->kind = FRAMING_NONE;
        out_f->length = 0;
        return 0;
    }

    /* 같은 이름의 헤더가 여러 줄일 수 있어 전체 목록을 순서대로 돈다. */
    for (size_t i = 0; i < h->n && i < HTTP_MAX_HEADERS; i++) {
        const http_header_t *field = &h->v[i];
        if (equals_ci(field->name, "Transfer-Encoding")) {
            str_t v = field->value;
            size_t pos = 0;
            has_te = true;
            /* Transfer-Encoding을 쉼표로 나눠 마지막 전송 코딩을 기억한다. */
            while (pos <= v.len) {
                size_t end = pos;
                size_t semi;
                str_t coding;
                str_t base;
                while (end < v.len && v.ptr[end] != ',') {
                    end++;
                }
                coding.ptr = v.ptr == NULL ? NULL : v.ptr + pos;
                coding.len = end - pos;
                coding = trim_ows_local(coding);
                if (coding.len == 0) {
                    *out_status = is_response ? 502 : 400;
                    return -1;
                }
                /* 세미콜론 뒤 매개변수와 코딩 이름을 분리한다. 마지막 코딩 자체가 chunked여야 한다. */
                semi = 0;
                while (semi < coding.len && coding.ptr[semi] != ';') {
                    semi++;
                }
                base.ptr = coding.ptr;
                base.len = semi;
                base = trim_ows_local(base);
                if (!http_token_valid(base)) {
                    *out_status = is_response ? 502 : 400;
                    return -1;
                }
                if (semi < coding.len) {
                    /* 코딩 매개변수는 비어 있거나 제어 문자를 포함할 수 없다. */
                    for (size_t j = semi + 1; j < coding.len; j++) {
                        unsigned char c = (unsigned char)coding.ptr[j];
                        if (c < 0x20 || c == 0x7f) {
                            *out_status = is_response ? 502 : 400;
                            return -1;
                        }
                    }
                }
                coding_count++;
                last_is_chunked = equals_ci(base, "chunked") && semi == coding.len;
                if (end == v.len) {
                    break;
                }
                pos = end + 1;
            }
        } else if (equals_ci(field->name, "Content-Length")) {
            str_t v = field->value;
            size_t pos = 0;
            has_cl = true;
            /* Content-Length 한 줄 안의 쉼표 목록도 각 값을 따로 읽어 서로 같은지 확인한다. */
            while (pos <= v.len) {
                size_t end = pos;
                uint64_t current;
                str_t item;
                while (end < v.len && v.ptr[end] != ',') {
                    end++;
                }
                item.ptr = v.ptr == NULL ? NULL : v.ptr + pos;
                item.len = end - pos;
                item = trim_ows_local(item);
                if (parse_decimal(item, &current) != 0 || (have_length && current != length)) {
                    *out_status = is_response ? 502 : 400;
                    return -1;
                }
                if (!have_length) {
                    length = current;
                    have_length = true;
                }
                if (end == v.len) {
                    break;
                }
                pos = end + 1;
            }
        }
    }

    /* TE와 CL이 함께 있으면 요청 경계가 다르게 해석될 수 있어 거부한다. */
    if ((has_te && has_cl) || (has_te && (coding_count == 0 || !last_is_chunked))) {
        *out_status = is_response ? 502 : 400;
        return -1;
    }
    if (has_te) {
        out_f->kind = FRAMING_CHUNKED;
        out_f->length = 0;
    } else if (has_cl) {
        out_f->kind = FRAMING_LENGTH;
        out_f->length = length;
    } else {
        out_f->kind = is_response ? FRAMING_CLOSE : FRAMING_NONE;
        out_f->length = 0;
    }
    return 0;
}

/* 역할: 본문 경계 추적기를 새 메시지의 framing에 맞춰 초기화한다. */
void http_body_tracker_init(http_body_tracker_t *t, http_framing_t f)
{
    if (t == NULL) {
        return;
    }
    /* 추적 상태를 본문 길이 방식에 맞춰 초기화한다. */
    t->f = f;
    t->remaining = f.kind == FRAMING_LENGTH ? f.length : 0;
    t->state = 0;
}

/* 청크 크기 줄의 16진 숫자 하나를 0~15로 바꾼다. */
static int hex_value(unsigned char c)
{
    if (c >= '0' && c <= '9') {
        return (int)(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return (int)(c - 'a') + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return (int)(c - 'A') + 10;
    }
    return -1;
}

/* 트레일러 이름 한 바이트가 헤더 이름 문자(tchar)인지 검사한다. */
static bool trailer_tchar(unsigned char c)
{
    str_t one = {(const char *)&c, 1};
    return http_token_valid(one);
}

/* 역할: 새 입력 조각 중 현재 본문에 속하는 길이를 계산하고 MORE/DONE/ERROR를 반환한다. */
http_body_result_t http_body_track(http_body_tracker_t *t, const char *p, size_t len,
                                   size_t *out_used)
{
    size_t i = 0;
    if (t == NULL || out_used == NULL || (len != 0 && p == NULL)) {
        if (out_used != NULL) {
            *out_used = 0;
        }
        return BODY_ERROR;
    }
    *out_used = 0;
    if (t->f.kind == FRAMING_NONE) {
        return BODY_DONE;
    }
    if (t->f.kind == FRAMING_CLOSE) {
        *out_used = len;
        return BODY_MORE;
    }
    if (t->f.kind == FRAMING_LENGTH) {
        size_t take = len;
        if (t->remaining == 0) {
            return BODY_DONE;
        }
        if ((uint64_t)take > t->remaining) {
            take = (size_t)t->remaining;
        }
        t->remaining -= (uint64_t)take;
        *out_used = take;
        return t->remaining == 0 ? BODY_DONE : BODY_MORE;
    }
    if (t->f.kind != FRAMING_CHUNKED) {
        return BODY_ERROR;
    }

#define BODY_ERROR_AT_CURSOR() do { *out_used = i; return BODY_ERROR; } while (0)
    /* state는 분할 입력 사이에 유지된다:
     * 0=크기 시작, 101~116=16진 숫자 수, 1=확장, 2=크기 CR,
     * 3=데이터, 4/9=데이터 뒤 줄끝, 5=트레일러 시작, 6=이름,
     * 7=값, 11/12=트레일러 CR, 10=완료.
     */
    while (i < len) {
        unsigned char c = (unsigned char)p[i];
        int phase = t->state;

        if (phase == 0) { /* 청크 크기 줄에서 첫 16진 숫자를 읽는다. */
            int digit = hex_value(c);
            if (digit < 0) {
                BODY_ERROR_AT_CURSOR();
            }
            t->remaining = (uint64_t)digit;
            t->state = 101; /* 100 + 1자리 */
        } else if (phase >= 101 && phase <= 116) { /* 크기 숫자를 누적하거나 줄끝으로 이동한다. */
            int digit = hex_value(c);
            unsigned count = (unsigned)(phase - 100);
            if (digit >= 0) {
                if (count >= 16 || t->remaining > (UINT64_MAX - (unsigned)digit) / 16) {
                    BODY_ERROR_AT_CURSOR();
                }
                t->remaining = t->remaining * 16 + (unsigned)digit;
                t->state = (int)(101 + count);
            } else if (c == ';') {
                t->state = 1;
            } else if (c == '\n') {
                t->state = t->remaining == 0 ? 5 : 3;
            } else if (c == '\r') {
                t->state = 2;
            } else {
                BODY_ERROR_AT_CURSOR();
            }
        } else if (phase == 1) { /* chunk extension */
            if (c == '\r') {
                t->state = 2;
            } else if (c == '\n') {
                t->state = t->remaining == 0 ? 5 : 3;
            } else if ((c < 0x20 && c != '\t') || c == 0x7f) {
                BODY_ERROR_AT_CURSOR();
            }
        } else if (phase == 2) { /* 크기 줄 CR 다음 LF */
            if (c != '\n') {
                BODY_ERROR_AT_CURSOR();
            }
            t->state = t->remaining == 0 ? 5 : 3;
        } else if (phase == 3) { /* 현재 청크에서 남은 데이터만 소비한다. */
            uint64_t available = (uint64_t)(len - i);
            size_t take = available > t->remaining ? (size_t)t->remaining : len - i;
            i += take;
            t->remaining -= (uint64_t)take;
            if (t->remaining == 0) {
                t->state = 4;
            }
            continue;
        } else if (phase == 4) { /* 데이터 뒤 줄 끝 */
            if (c == '\n') {
                t->state = 0;
                t->remaining = 0;
            } else if (c == '\r') {
                t->state = 9;
            } else {
                BODY_ERROR_AT_CURSOR();
            }
        } else if (phase == 9) { /* 데이터 뒤 CR 다음 LF */
            if (c != '\n') {
                BODY_ERROR_AT_CURSOR();
            }
            t->state = 0;
            t->remaining = 0;
        } else if (phase == 5) { /* 트레일러 줄을 시작한다. 빈 줄이면 전체 본문이 끝난다. */
            if (c == '\n') {
                t->state = 10;
            } else if (c == '\r') {
                t->state = 12;
            } else if (trailer_tchar(c)) {
                t->state = 6; /* 필드 이름 */
            } else {
                BODY_ERROR_AT_CURSOR();
            }
        } else if (phase == 6) { /* 트레일러 필드 이름 */
            if (c == ':') {
                t->state = 7;
            } else if (!trailer_tchar(c)) {
                BODY_ERROR_AT_CURSOR();
            }
        } else if (phase == 7) { /* 트레일러 필드 값 */
            if (c == '\n') {
                t->state = 5;
            } else if (c == '\r') {
                t->state = 11;
            } else if ((c < 0x20 && c != '\t') || c == 0x7f) {
                BODY_ERROR_AT_CURSOR();
            }
        } else if (phase == 11) { /* 트레일러 값 CR 다음 LF */
            if (c != '\n') {
                BODY_ERROR_AT_CURSOR();
            }
            t->state = 5;
        } else if (phase == 12) { /* 빈 트레일러 종료 CR 다음 LF */
            if (c != '\n') {
                BODY_ERROR_AT_CURSOR();
            }
            t->state = 10;
        } else if (phase == 10) { /* 완료 */
            *out_used = i;
            return BODY_DONE;
        } else {
            BODY_ERROR_AT_CURSOR();
        }
        i++;
        if (t->state == 10) {
            *out_used = i;
            return BODY_DONE;
        }
    }
    *out_used = i;
#undef BODY_ERROR_AT_CURSOR
    return t->state == 10 ? BODY_DONE : BODY_MORE;
}

/* 역할: HTTP 버전 기본값과 Connection 토큰을 합쳐 응답 뒤 연결을 유지할지 정한다. */
bool http_keep_alive(int minor, const http_headers_t *h)
{
    /* close가 있으면 버전 기본값보다 우선한다. HTTP/1.0은 keep-alive를 요청해야 유지한다. */
    if (h == NULL || is_connection_token(h, "close")) {
        return false;
    }
    if (minor >= 1) {
        return true;
    }
    return minor == 0 && is_connection_token(h, "keep-alive");
}
