/* HTTP 메시지 본문 길이, 청크 경계 및 연결 유지 판정. */
#include "common/http_framing.h"

#include "common/http_headers.h"
#include "common/internal_http_text.h"

/* 받은 순서대로 헤더의 목록을 검사하고 일치하는 길이만 받아들인다. */
static bool read_content_length(str_t list, http_framing_t *framing)
{
    if (list.ptr == NULL) {
        return false;
    }
    do {
        uint64_t length;
        if (str_to_u64(http_pop_list_item(&list), UINT64_MAX, &length) < 0 ||
            (framing->kind == FRAMING_LENGTH && framing->length != length)) {
            return false;
        }
        *framing = (http_framing_t){FRAMING_LENGTH, length};
    } while (list.ptr != NULL);
    return true;
}

static bool read_transfer_encoding(str_t list, http_framing_t *framing)
{
    if (list.ptr == NULL) {
        return false;
    }
    do {
        str_t coding = http_pop_list_item(&list);
        if (framing->kind == FRAMING_CHUNKED || !http_token_valid(coding)) {
            return false;
        }
        if (str_eq_ci(coding, "chunked")) {
            framing->kind = FRAMING_CHUNKED;
        }
    } while (list.ptr != NULL);
    return true;
}

int http_resolve_framing(const http_headers_t *h, bool is_response, int status,
                         http_method_t req_method, http_framing_t *out_f, int *out_status)
{
    http_framing_t framing = {FRAMING_NONE, 0};
    bool has_encoding = false;

    if (is_response &&
        (req_method == HTTP_HEAD || status / 100 == 1 || status == 204 || status == 304)) {
        *out_f = framing;
        return 0;
    }

    for (size_t i = 0; i < h->n; ++i) {
        const http_header_t *field = &h->v[i];
        if (str_eq_ci(field->name, "Content-Length")) {
            if (has_encoding || !read_content_length(field->value, &framing)) {
                goto invalid_framing;
            }
        } else if (str_eq_ci(field->name, "Transfer-Encoding")) {
            if (framing.kind == FRAMING_LENGTH || !read_transfer_encoding(field->value, &framing)) {
                goto invalid_framing;
            }
            has_encoding = true;
        }
    }

    if (has_encoding && framing.kind != FRAMING_CHUNKED) {
        goto invalid_framing;
    }
    if (is_response && framing.kind == FRAMING_NONE) {
        framing.kind = FRAMING_CLOSE;
    }
    *out_f = framing;
    return 0;

invalid_framing:
    *out_status = is_response ? 502 : 400;
    return -1;
}

bool http_keep_alive(int minor, const http_headers_t *h)
{
    bool keep = minor >= 1;
    for (size_t i = 0; i < h->n; ++i) {
        const http_header_t *field = &h->v[i];
        if (!str_eq_ci(field->name, "Connection")) {
            continue;
        }

        for (str_t list = field->value; list.len != 0;) {
            str_t token = http_pop_list_item(&list);
            if (str_eq_ci(token, "close")) {
                return false;
            }
            keep = keep || str_eq_ci(token, "keep-alive");
        }
    }
    return keep;
}

typedef enum {
    CHUNK_SIZE,
    CHUNK_BEFORE_EXTENSION,
    CHUNK_EXTENSION,
    CHUNK_SIZE_LF,
    CHUNK_DATA,
    CHUNK_DATA_END,
    CHUNK_DATA_LF,
    CHUNK_TRAILER_START,
    CHUNK_TRAILER_NAME,
    CHUNK_TRAILER_VALUE,
    CHUNK_TRAILER_LF,
    CHUNK_END_LF,
    CHUNK_COMPLETE,
    CHUNK_INVALID,
} chunk_phase_t;

enum { CHUNK_MAX_DIGITS = 16, CHUNK_STATE_RADIX = CHUNK_MAX_DIGITS + 1 };

static int hex_digit(unsigned char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    unsigned char lower = c | 0x20;
    return lower >= 'a' && lower <= 'f' ? lower - 'a' + 10 : -1;
}

static chunk_phase_t chunk_size_end(uint64_t size)
{
    return size != 0 ? CHUNK_DATA : CHUNK_TRAILER_START;
}

static chunk_phase_t read_chunk_metadata(chunk_phase_t phase, unsigned char c, uint64_t *remaining,
                                         int *digits)
{
    switch (phase) {
    case CHUNK_SIZE: {
        int digit = hex_digit(c);
        if (digit >= 0 && *digits < CHUNK_MAX_DIGITS) {
            *remaining = (*remaining << 4) | (uint64_t)digit;
            ++*digits;
            return CHUNK_SIZE;
        }
        if (*digits == 0) {
            return CHUNK_INVALID;
        }
        if (http_is_ows((char)c)) {
            return CHUNK_BEFORE_EXTENSION;
        }
        if (c == ';') {
            return CHUNK_EXTENSION;
        }
        if (c == '\r') {
            return CHUNK_SIZE_LF;
        }
        return c == '\n' ? chunk_size_end(*remaining) : CHUNK_INVALID;
    }
    case CHUNK_BEFORE_EXTENSION:
        if (c == ';') {
            return CHUNK_EXTENSION;
        }
        return http_is_ows((char)c) ? CHUNK_BEFORE_EXTENSION : CHUNK_INVALID;
    case CHUNK_EXTENSION:
        if (c == '\r') {
            return CHUNK_SIZE_LF;
        }
        if (c == '\n') {
            return chunk_size_end(*remaining);
        }
        return http_field_byte_valid(c) ? CHUNK_EXTENSION : CHUNK_INVALID;
    case CHUNK_SIZE_LF:
        return c == '\n' ? chunk_size_end(*remaining) : CHUNK_INVALID;
    case CHUNK_DATA_END:
        if (c == '\r') {
            return CHUNK_DATA_LF;
        }
        /* LF 단독은 공개 추적기 계약에 따라 허용한다. */
        return c == '\n' ? CHUNK_SIZE : CHUNK_INVALID;
    case CHUNK_DATA_LF:
        return c == '\n' ? CHUNK_SIZE : CHUNK_INVALID;
    case CHUNK_TRAILER_START:
        if (c == '\r') {
            return CHUNK_END_LF;
        }
        if (c == '\n') {
            return CHUNK_COMPLETE;
        }
        return http_token_byte_valid(c) ? CHUNK_TRAILER_NAME : CHUNK_INVALID;
    case CHUNK_TRAILER_NAME:
        if (c == ':') {
            return CHUNK_TRAILER_VALUE;
        }
        return http_token_byte_valid(c) ? CHUNK_TRAILER_NAME : CHUNK_INVALID;
    case CHUNK_TRAILER_VALUE:
        if (c == '\r') {
            return CHUNK_TRAILER_LF;
        }
        if (c == '\n') {
            return CHUNK_TRAILER_START;
        }
        return http_field_byte_valid(c) ? CHUNK_TRAILER_VALUE : CHUNK_INVALID;
    case CHUNK_TRAILER_LF:
        return c == '\n' ? CHUNK_TRAILER_START : CHUNK_INVALID;
    case CHUNK_END_LF:
        return c == '\n' ? CHUNK_COMPLETE : CHUNK_INVALID;
    default:
        return CHUNK_INVALID;
    }
}

static size_t consume_data(uint64_t *remaining, size_t available)
{
    size_t used = *remaining < available ? (size_t)*remaining : available;
    *remaining -= used;
    return used;
}

static http_body_result_t track_chunks(http_body_tracker_t *t, const char *p, size_t len,
                                       size_t *out_used)
{
    /* state에 단계와 크기 줄의 자릿수를 저장한다. */
    chunk_phase_t phase = (chunk_phase_t)(t->state / CHUNK_STATE_RADIX);
    int digits = t->state % CHUNK_STATE_RADIX;
    size_t used = 0;

    while (used < len && phase != CHUNK_INVALID && phase != CHUNK_COMPLETE) {
        if (phase == CHUNK_DATA) {
            used += consume_data(&t->remaining, len - used);
            if (t->remaining == 0) {
                phase = CHUNK_DATA_END;
                digits = 0;
            }
            continue;
        }
        phase = read_chunk_metadata(phase, (unsigned char)p[used], &t->remaining, &digits);
        if (phase != CHUNK_INVALID) {
            ++used;
        }
    }

    t->state = phase * CHUNK_STATE_RADIX + digits;
    *out_used = used;
    if (phase == CHUNK_INVALID) {
        return BODY_ERROR;
    }
    return phase == CHUNK_COMPLETE ? BODY_DONE : BODY_MORE;
}

void http_body_tracker_init(http_body_tracker_t *t, http_framing_t f)
{
    *t = (http_body_tracker_t){.f = f, .remaining = f.kind == FRAMING_LENGTH ? f.length : 0};
}

http_body_result_t http_body_track(http_body_tracker_t *t, const char *p, size_t len,
                                   size_t *out_used)
{
    switch (t->f.kind) {
    case FRAMING_LENGTH:
        *out_used = consume_data(&t->remaining, len);
        return t->remaining == 0 ? BODY_DONE : BODY_MORE;
    case FRAMING_CHUNKED:
        return track_chunks(t, p, len, out_used);
    case FRAMING_CLOSE:
        *out_used = len;
        return BODY_MORE;
    case FRAMING_NONE:
        *out_used = 0;
        return BODY_DONE;
    }
    *out_used = 0;
    return BODY_ERROR;
}
