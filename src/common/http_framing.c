/* src/common/http_framing.c
 *
 * 본문 길이·경계 추적·keep-alive. 명세: http_framing.h
 */
#include "common/http_framing.h"

#include <stdint.h>
#include <string.h>

#include "common/http_headers.h"
#include "core/str.h"

/* 쉼표 목록에서 다음 항목을 꺼낸다. 빈 항목도 호출자가 검사할 수 있게 남긴다. */
static bool list_item(str_t *rest, str_t *out)
{
    const char *comma;

    if (rest->ptr == NULL) {
        return false;
    }
    comma = memchr(rest->ptr, ',', rest->len);
    if (comma == NULL) {
        *out = str_trim_ows(*rest);
        *rest = (str_t){0};
    } else {
        size_t len = (size_t)(comma - rest->ptr);
        *out = str_trim_ows((str_t){rest->ptr, len});
        *rest = (str_t){comma + 1, rest->len - len - 1};
    }
    return true;
}

/* 모든 Content-Length 값(쉼표 목록 포함)이 같은 10진수인지 확인한다. */
static int resolve_content_length(const http_headers_t *h, uint64_t *out_len)
{
    bool have = false;
    uint64_t want = 0;

    for (size_t i = 0; i < h->n; i++) {
        str_t rest = h->v[i].value;
        str_t item;

        if (!str_eq_ci(h->v[i].name, "Content-Length")) {
            continue;
        }
        if (rest.len == 0) {
            return -1;
        }
        while (list_item(&rest, &item)) {
            uint64_t v;
            if (str_to_u64(item, UINT64_MAX, &v) != 0) {
                return -1;
            }
            if (have && v != want) {
                return -1;
            }
            have = true;
            want = v;
        }
    }
    *out_len = want;
    return have ? 0 : -1;
}

/* 받은 순서로 코딩을 검사한다. chunked는 한 번만, 마지막에 허용한다. */
static bool te_ends_with_chunked(const http_headers_t *h)
{
    bool chunked = false;

    for (size_t i = 0; i < h->n; i++) {
        str_t rest = h->v[i].value;
        str_t item;

        if (!str_eq_ci(h->v[i].name, "Transfer-Encoding")) {
            continue;
        }
        if (rest.len == 0) {
            return false;
        }
        while (list_item(&rest, &item)) {
            if (chunked || !http_token_valid(item)) {
                return false;
            }
            chunked = str_eq_ci(item, "chunked");
        }
    }
    return chunked;
}

int http_resolve_framing(const http_headers_t *h, bool is_response, int status,
                         http_method_t req_method, http_framing_t *out_f, int *out_status)
{
    int err = is_response ? 502 : 400;
    size_t n_te = http_header_count(h, "Transfer-Encoding");
    size_t n_cl = http_header_count(h, "Content-Length");

    if (is_response && (req_method == HTTP_HEAD || (status >= 100 && status < 200) ||
                        status == 204 || status == 304)) {
        out_f->kind = FRAMING_NONE;
        out_f->length = 0;
        return 0;
    }
    if (n_te > 0 && n_cl > 0) {
        *out_status = err; /* request smuggling 방지 */
        return -1;
    }
    if (n_te > 0) {
        if (!te_ends_with_chunked(h)) {
            *out_status = err;
            return -1;
        }
        out_f->kind = FRAMING_CHUNKED;
        out_f->length = 0;
        return 0;
    }
    if (n_cl > 0) {
        uint64_t len;
        if (resolve_content_length(h, &len) != 0) {
            *out_status = err;
            return -1;
        }
        out_f->kind = FRAMING_LENGTH;
        out_f->length = len;
        return 0;
    }
    out_f->kind = is_response ? FRAMING_CLOSE : FRAMING_NONE;
    out_f->length = 0;
    return 0;
}

/* CHUNKED 파서 상태. tracker.state의 하위 8비트 = 단계, 그 위 = 현재 청크 크기 자릿수.
 * remaining은 크기 줄을 읽는 동안 크기를 누적하고, 데이터 단계에서는 남은 데이터 바이트다. */
enum {
    CK_SIZE_START, /* 첫 16진 자리 대기 */
    CK_SIZE,       /* 16진 자리 계속 */
    CK_EXT_SEP,    /* 청크 확장 앞 공백과 세미콜론 */
    CK_EXT,        /* chunk-ext: 줄 끝까지 건너뜀 */
    CK_SIZE_LF,    /* 크기 줄의 CR 뒤 LF 대기 */
    CK_DATA,       /* 청크 데이터 */
    CK_DATA_CR,    /* 데이터 뒤 줄 끝 (CR 또는 LF) 대기 */
    CK_DATA_LF,    /* 데이터 뒤 CR 다음 LF 대기 */
    CK_TRAILER,    /* 트레일러 줄의 시작 (빈 줄이면 끝) */
    CK_TRAILER_NAME,
    CK_TRAILER_VALUE,
    CK_TRAILER_LF,
    CK_FINAL_LF, /* 마지막 빈 줄의 CR 뒤 LF 대기 */
    CK_DONE,
    CK_ERROR
};
#define CK_MAX_DIGITS 16

static int hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* 크기 줄이 끝났을 때: 크기가 0이면 트레일러, 아니면 데이터 */
static int after_size_line(const http_body_tracker_t *t)
{
    return t->remaining == 0 ? CK_TRAILER : CK_DATA;
}

static bool valid_field_byte(char c)
{
    unsigned char byte = (unsigned char)c;
    return (byte >= 0x20 || byte == '\t') && byte != 0x7F;
}

static http_body_result_t track_chunked(http_body_tracker_t *t, const char *p, size_t len,
                                        size_t *out_used)
{
    int phase = t->state & 0xFF;
    int digits = t->state >> 8;
    size_t i = 0;

    while (i < len && phase != CK_DONE && phase != CK_ERROR) {
        char c = p[i];
        int v;

        switch (phase) {
        case CK_SIZE_START:
        case CK_SIZE:
            v = hexval(c);
            if (v >= 0) {
                if (digits == CK_MAX_DIGITS) {
                    phase = CK_ERROR;
                    continue;
                }
                t->remaining = t->remaining * 16 + (uint64_t)v;
                digits++;
                phase = CK_SIZE;
            } else if (phase == CK_SIZE && c == ';') {
                phase = CK_EXT;
            } else if (phase == CK_SIZE && (c == ' ' || c == '\t')) {
                phase = CK_EXT_SEP;
            } else if (phase == CK_SIZE && c == '\r') {
                phase = CK_SIZE_LF;
            } else if (phase == CK_SIZE && c == '\n') {
                phase = after_size_line(t);
            } else {
                phase = CK_ERROR;
                continue;
            }
            i++;
            break;
        case CK_EXT_SEP:
            if (c == ';') {
                phase = CK_EXT;
            } else if (c != ' ' && c != '\t') {
                phase = CK_ERROR;
                continue;
            }
            i++;
            break;
        case CK_EXT:
            if (c == '\r') {
                phase = CK_SIZE_LF;
            } else if (c == '\n') {
                phase = after_size_line(t);
            } else if (!valid_field_byte(c)) {
                phase = CK_ERROR;
                continue;
            }
            i++;
            break;
        case CK_SIZE_LF:
            if (c != '\n') {
                phase = CK_ERROR;
                continue;
            }
            phase = after_size_line(t);
            i++;
            break;
        case CK_DATA: {
            size_t take = len - i;
            if ((uint64_t)take > t->remaining) {
                take = (size_t)t->remaining;
            }
            t->remaining -= take;
            i += take;
            if (t->remaining == 0) {
                phase = CK_DATA_CR;
            }
            break;
        }
        case CK_DATA_CR:
            if (c == '\r') {
                phase = CK_DATA_LF;
            } else if (c == '\n') {
                phase = CK_SIZE_START;
                digits = 0;
            } else {
                phase = CK_ERROR;
                continue;
            }
            i++;
            break;
        case CK_DATA_LF:
            if (c != '\n') {
                phase = CK_ERROR;
                continue;
            }
            phase = CK_SIZE_START;
            digits = 0;
            i++;
            break;
        case CK_TRAILER:
            if (c == '\r') {
                phase = CK_FINAL_LF;
            } else if (c == '\n') {
                phase = CK_DONE;
            } else if (http_token_valid((str_t){p + i, 1})) {
                phase = CK_TRAILER_NAME;
            } else {
                phase = CK_ERROR;
                continue;
            }
            i++;
            break;
        case CK_TRAILER_NAME:
            if (c == ':') {
                phase = CK_TRAILER_VALUE;
            } else if (!http_token_valid((str_t){p + i, 1})) {
                phase = CK_ERROR;
                continue;
            }
            i++;
            break;
        case CK_TRAILER_VALUE:
            if (c == '\r') {
                phase = CK_TRAILER_LF;
            } else if (c == '\n') {
                phase = CK_TRAILER;
            } else if (!valid_field_byte(c)) {
                phase = CK_ERROR;
                continue;
            }
            i++;
            break;
        case CK_TRAILER_LF:
            if (c != '\n') {
                phase = CK_ERROR;
                continue;
            }
            phase = CK_TRAILER;
            i++;
            break;
        case CK_FINAL_LF:
            if (c != '\n') {
                phase = CK_ERROR;
                continue;
            }
            phase = CK_DONE;
            i++;
            break;
        default:
            phase = CK_ERROR;
            break;
        }
    }
    t->state = phase | (digits << 8);
    *out_used = i;
    if (phase == CK_ERROR) {
        return BODY_ERROR;
    }
    return phase == CK_DONE ? BODY_DONE : BODY_MORE;
}

void http_body_tracker_init(http_body_tracker_t *t, http_framing_t f)
{
    t->f = f;
    t->remaining = f.kind == FRAMING_LENGTH ? f.length : 0;
    t->state = CK_SIZE_START;
}

http_body_result_t http_body_track(http_body_tracker_t *t, const char *p, size_t len,
                                   size_t *out_used)
{
    *out_used = 0;
    switch (t->f.kind) {
    case FRAMING_NONE:
        return BODY_DONE;
    case FRAMING_LENGTH: {
        size_t take = len;
        if ((uint64_t)take > t->remaining) {
            take = (size_t)t->remaining;
        }
        t->remaining -= take;
        *out_used = take;
        return t->remaining == 0 ? BODY_DONE : BODY_MORE;
    }
    case FRAMING_CHUNKED:
        return track_chunked(t, p, len, out_used);
    case FRAMING_CLOSE:
        *out_used = len;
        return BODY_MORE;
    }
    return BODY_ERROR;
}

bool http_keep_alive(int minor, const http_headers_t *h)
{
    bool keep = false;

    for (size_t i = 0; i < h->n; i++) {
        str_t rest = h->v[i].value;
        str_t item;

        if (!str_eq_ci(h->v[i].name, "Connection")) {
            continue;
        }
        while (list_item(&rest, &item)) {
            if (str_eq_ci(item, "close")) {
                return false;
            }
            if (str_eq_ci(item, "keep-alive")) {
                keep = true;
            }
        }
    }
    return minor >= 1 || keep;
}
