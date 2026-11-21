/* src/common/http_framing.c
 *
 * 본문 길이·경계 추적·keep-alive. 명세: http_framing.h
 */
#include "common/http_framing.h"

#include <stdint.h>

#include "common/http_headers.h"
#include "core/str.h"

/* v를 쉼표로 나눈 i번째 항목(OWS 제거)을 out에 넣는다. 항목이 없으면 false. */
static bool list_item(str_t v, size_t idx, str_t *out)
{
    size_t start = 0;
    size_t n = 0;

    for (size_t k = 0; k <= v.len; k++) {
        if (k == v.len || v.ptr[k] == ',') {
            if (n == idx) {
                str_t item = {v.ptr + start, k - start};
                *out = str_trim_ows(item);
                return true;
            }
            n++;
            start = k + 1;
        }
    }
    return false;
}

/* 모든 Content-Length 값(쉼표 목록 포함)이 같은 10진수인지 확인한다. */
static int resolve_content_length(const http_headers_t *h, uint64_t *out_len)
{
    bool have = false;
    uint64_t want = 0;

    for (size_t i = 0; i < h->n; i++) {
        str_t item;

        if (!str_eq_ci(h->v[i].name, "Content-Length")) {
            continue;
        }
        for (size_t idx = 0; list_item(h->v[i].value, idx, &item); idx++) {
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

/* 마지막 Transfer-Encoding 헤더의 마지막 코딩이 chunked인가 */
static bool te_ends_with_chunked(const http_headers_t *h)
{
    const http_header_t *last = NULL;
    str_t item = {NULL, 0};
    str_t found = {NULL, 0};

    for (size_t i = 0; i < h->n; i++) {
        if (str_eq_ci(h->v[i].name, "Transfer-Encoding")) {
            last = &h->v[i];
        }
    }
    if (last == NULL) {
        return false;
    }
    for (size_t idx = 0; list_item(last->value, idx, &item); idx++) {
        found = item;
    }
    return str_eq_ci(found, "chunked");
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
    CK_EXT,        /* chunk-ext: 줄 끝까지 건너뜀 */
    CK_SIZE_LF,    /* 크기 줄의 CR 뒤 LF 대기 */
    CK_DATA,       /* 청크 데이터 */
    CK_DATA_CR,    /* 데이터 뒤 줄 끝 (CR 또는 LF) 대기 */
    CK_DATA_LF,    /* 데이터 뒤 CR 다음 LF 대기 */
    CK_TRAILER,    /* 트레일러 줄의 시작 (빈 줄이면 끝) */
    CK_TRAILER_LN, /* 트레일러 줄 내용: LF까지 건너뜀 */
    CK_FINAL_LF,   /* 마지막 빈 줄의 CR 뒤 LF 대기 */
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
            } else if (phase == CK_SIZE && (c == ';' || c == ' ' || c == '\t')) {
                phase = CK_EXT;
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
        case CK_EXT:
            if (c == '\r') {
                phase = CK_SIZE_LF;
            } else if (c == '\n') {
                phase = after_size_line(t);
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
            } else {
                phase = CK_TRAILER_LN;
            }
            i++;
            break;
        case CK_TRAILER_LN:
            if (c == '\n') {
                phase = CK_TRAILER;
            }
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
        str_t item;

        if (!str_eq_ci(h->v[i].name, "Connection")) {
            continue;
        }
        for (size_t idx = 0; list_item(h->v[i].value, idx, &item); idx++) {
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
