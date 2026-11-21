/* src/common/http_headers.c
 *
 * 헤더 끝 탐색·파싱·쓰기. 명세: http_headers.h
 */
#include "common/http_headers.h"

#include <stddef.h>
#include <string.h>

/* RFC 7230 tchar 표 (0~127) */
static bool is_tchar(unsigned char c)
{
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        return true;
    }
    return c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

/* 헤더 값에 허용되지 않는 바이트인가 (HTAB 외 제어 문자, DEL) */
static bool bad_value_byte(unsigned char c)
{
    return (c < 0x20 && c != '\t') || c == 0x7F;
}

http_parse_result_t http_find_head_end(const char *p, size_t len, size_t max_head, size_t *out_skip,
                                       size_t *out_head_len, int *out_status)
{
    size_t scan_len = len < max_head ? len : max_head;
    size_t i = 0;

    /* 1. 앞의 빈 줄("\r\n" 또는 "\n")을 건너뛴다 */
    while (i < scan_len) {
        if (p[i] == '\n') {
            i++;
        } else if (p[i] == '\r' && scan_len - i >= 2 && p[i + 1] == '\n') {
            i += 2;
        } else {
            break;
        }
    }

    /* 2. 줄 끝 LF 바로 뒤가 빈 줄("\n" 또는 "\r\n")이면 머리 끝이다 */
    for (size_t j = i; j < scan_len; j++) {
        size_t end = 0;

        if (p[j] != '\n') {
            continue;
        }
        if (scan_len - j >= 2 && p[j + 1] == '\n') {
            end = j + 2;
        } else if (scan_len - j >= 3 && p[j + 1] == '\r' && p[j + 2] == '\n') {
            end = j + 3;
        } else {
            continue;
        }
        *out_skip = i;
        *out_head_len = end;
        return HTTP_PARSE_DONE;
    }

    if (len > max_head) {
        *out_status = 431;
        return HTTP_PARSE_ERROR;
    }
    return HTTP_PARSE_INCOMPLETE;
}

int http_parse_headers(const char *p, size_t len, size_t max_headers, http_headers_t *out_h,
                       int *out_status)
{
    size_t limit = max_headers < HTTP_MAX_HEADERS ? max_headers : HTTP_MAX_HEADERS;
    size_t pos = 0;

    out_h->n = 0;
    while (pos < len) {
        const char *line = p + pos;
        const char *nl = memchr(line, '\n', len - pos);
        const char *colon;
        size_t llen;
        str_t name, value;

        if (nl == NULL) {
            *out_status = 400; /* 줄 끝이 없다 */
            return -1;
        }
        llen = (size_t)(nl - line);
        pos += llen + 1;
        if (llen > 0 && line[llen - 1] == '\r') {
            llen--;
        }
        if (llen == 0 || line[0] == ' ' || line[0] == '\t') {
            *out_status = 400; /* 중간 빈 줄, obs-fold */
            return -1;
        }
        colon = memchr(line, ':', llen);
        if (colon == NULL) {
            *out_status = 400;
            return -1;
        }
        name.ptr = line;
        name.len = (size_t)(colon - line);
        if (!http_token_valid(name)) {
            *out_status = 400; /* "Host : a" 포함 */
            return -1;
        }
        value.ptr = colon + 1;
        value.len = llen - name.len - 1;
        value = str_trim_ows(value);
        for (size_t k = 0; k < value.len; k++) {
            if (bad_value_byte((unsigned char)value.ptr[k])) {
                *out_status = 400;
                return -1;
            }
        }
        if (out_h->n >= limit) {
            *out_status = 431;
            return -1;
        }
        out_h->v[out_h->n].name = name;
        out_h->v[out_h->n].value = value;
        out_h->n++;
    }
    return 0;
}

const http_header_t *http_header_get(const http_headers_t *h, const char *name)
{
    for (size_t i = 0; i < h->n; i++) {
        if (str_eq_ci(h->v[i].name, name)) {
            return &h->v[i];
        }
    }
    return NULL;
}

size_t http_header_count(const http_headers_t *h, const char *name)
{
    size_t c = 0;

    for (size_t i = 0; i < h->n; i++) {
        if (str_eq_ci(h->v[i].name, name)) {
            c++;
        }
    }
    return c;
}

bool http_token_valid(str_t s)
{
    if (s.len == 0) {
        return false;
    }
    for (size_t i = 0; i < s.len; i++) {
        if (!is_tchar((unsigned char)s.ptr[i])) {
            return false;
        }
    }
    return true;
}

bool http_is_hop_by_hop(str_t name, const http_headers_t *h)
{
    static const char *const fixed[] = {
        "Connection",
        "Keep-Alive",
        "Proxy-Connection",
        "TE",
        "Trailer",
        "Transfer-Encoding",
        "Upgrade",
        "Proxy-Authenticate",
        "Proxy-Authorization",
    };

    for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
        if (str_eq_ci(name, fixed[i])) {
            return true;
        }
    }
    if (h == NULL) {
        return false;
    }
    /* Connection 헤더 값에 쉼표로 나열된 이름 */
    for (size_t i = 0; i < h->n; i++) {
        str_t v = h->v[i].value;
        size_t start = 0;

        if (!str_eq_ci(h->v[i].name, "Connection")) {
            continue;
        }
        for (size_t k = 0; v.len > 0 && k <= v.len; k++) {
            if (k == v.len || v.ptr[k] == ',') {
                str_t item = {v.ptr + start, k - start};
                if (str_eq_ci_str(str_trim_ows(item), name)) {
                    return true;
                }
                start = k + 1;
            }
        }
    }
    return false;
}

int http_headers_write(const http_headers_t *h, bool skip_hop_by_hop, buf_t *out)
{
    size_t saved = out->len;

    for (size_t i = 0; i < h->n; i++) {
        const http_header_t *x = &h->v[i];

        if (skip_hop_by_hop && http_is_hop_by_hop(x->name, h)) {
            continue;
        }
        if (buf_append(out, x->name.ptr, x->name.len) != 0 || buf_append(out, ": ", 2) != 0 ||
            buf_append(out, x->value.ptr, x->value.len) != 0 || buf_append(out, "\r\n", 2) != 0) {
            out->len = saved;
            return -1;
        }
    }
    return 0;
}
