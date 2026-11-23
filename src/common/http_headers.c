/* 요청과 응답의 헤더 구간 탐색, 해석 및 직렬화. */
#include "common/http_headers.h"

#include <string.h>

#include "common/internal_http_text.h"

http_parse_result_t http_find_head_end(const char *p, size_t len, size_t max_head, size_t *out_skip,
                                       size_t *out_head_len, int *out_status)
{
    /* 수신 버퍼에 본문이나 다음 메시지까지 있어도 헤더 한도까지만 탐색한다. */
    const size_t available = len < max_head ? len : max_head;
    size_t start = 0;
    size_t first = 0;
    bool saw_start_line = false;

    while (start < available) {
        const char *lf = memchr(p + start, '\n', available - start);
        if (lf == NULL) {
            break;
        }

        const size_t end = (size_t)(lf - p) + 1;
        const bool empty = end - start == 1 || (end - start == 2 && p[start] == '\r');
        /* 선행 빈 줄은 건너뛰고, 시작 줄을 본 뒤의 빈 줄에서만 머리를 끝낸다. */
        if (empty && saw_start_line) {
            *out_skip = first;
            *out_head_len = end;
            return HTTP_PARSE_DONE;
        }
        if (!saw_start_line && !empty) {
            first = start;
            saw_start_line = true;
        }
        start = end;
    }

    if (len <= max_head) {
        return HTTP_PARSE_INCOMPLETE;
    }
    *out_status = 431;
    return HTTP_PARSE_ERROR;
}

static bool parse_field(str_t line, http_header_t *out)
{
    if (line.len == 0) {
        return false;
    }
    const char *colon = memchr(line.ptr, ':', line.len);
    if (colon == NULL) {
        return false;
    }

    size_t name_len = (size_t)(colon - line.ptr);
    str_t name = {line.ptr, name_len};
    str_t value = {colon + 1, line.len - name_len - 1};
    /* 이름은 받은 그대로 검사하고, 값은 제어 문자를 검사한 뒤 앞뒤 공백만 뗀다. */
    if (!http_token_valid(name)) {
        return false;
    }
    for (size_t i = 0; i < value.len; ++i) {
        if (!http_field_byte_valid((unsigned char)value.ptr[i])) {
            return false;
        }
    }

    *out = (http_header_t){name, str_trim_ows(value)};
    return true;
}

int http_parse_headers(const char *p, size_t len, size_t max_headers, http_headers_t *out_h,
                       int *out_status)
{
    size_t offset = 0;
    out_h->n = 0;

    while (offset < len) {
        const char *start = p + offset;
        const char *lf = memchr(start, '\n', len - offset);
        http_header_t field;

        if (lf == NULL) {
            *out_status = 400;
            return -1;
        }
        size_t line_len = (size_t)(lf - start);
        offset += line_len + 1;
        if (line_len != 0 && start[line_len - 1] == '\r') {
            --line_len;
        }
        if (!parse_field((str_t){start, line_len}, &field)) {
            *out_status = 400;
            return -1;
        }
        if (out_h->n == max_headers || out_h->n == HTTP_MAX_HEADERS) {
            *out_status = 431;
            return -1;
        }
        /* 길이 헤더 등의 중복을 나중에 검사할 수 있게 원래 순서대로 보존한다. */
        out_h->v[out_h->n++] = field;
    }
    return 0;
}

const http_header_t *http_header_get(const http_headers_t *h, const char *name)
{
    for (size_t i = 0; i < h->n; ++i) {
        const http_header_t *field = &h->v[i];
        if (str_eq_ci(field->name, name)) {
            return field;
        }
    }
    return NULL;
}

size_t http_header_count(const http_headers_t *h, const char *name)
{
    size_t count = 0;
    for (size_t i = 0; i < h->n; ++i) {
        if (str_eq_ci(h->v[i].name, name)) {
            ++count;
        }
    }
    return count;
}

bool http_token_valid(str_t s)
{
    for (size_t i = 0; i < s.len; ++i) {
        if (!http_token_byte_valid((unsigned char)s.ptr[i])) {
            return false;
        }
    }
    return s.len != 0;
}

bool http_is_hop_by_hop(str_t name, const http_headers_t *h)
{
    static const char *const connection_fields[] = {
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
    for (size_t i = 0; i < sizeof(connection_fields) / sizeof(*connection_fields); ++i) {
        if (str_eq_ci(name, connection_fields[i])) {
            return true;
        }
    }

    /* 고정 목록 외에도 Connection 값에 나열된 필드가 이 연결에서만 유효하다. */
    for (size_t i = 0; h != NULL && i < h->n; ++i) {
        const http_header_t *field = &h->v[i];
        if (!str_eq_ci(field->name, "Connection")) {
            continue;
        }
        for (str_t list = field->value; list.len != 0;) {
            str_t item = http_pop_list_item(&list);
            if (item.len != 0 && str_eq_ci_str(name, item)) {
                return true;
            }
        }
    }
    return false;
}

int http_headers_write(const http_headers_t *h, bool skip_hop_by_hop, buf_t *out)
{
    const size_t start = out->len;
    for (size_t i = 0; i < h->n; ++i) {
        http_header_t field = h->v[i];
        if (skip_hop_by_hop && http_is_hop_by_hop(field.name, h)) {
            continue;
        }
        const str_t parts[] = {field.name, STR_LIT(": "), field.value, STR_LIT("\r\n")};
        for (size_t part = 0; part < sizeof(parts) / sizeof(*parts); ++part) {
            if (buf_append(out, parts[part].ptr, parts[part].len) < 0) {
                /* 이번 호출이 붙인 헤더 전체를 논리적으로 되돌린다. */
                out->len = start;
                return -1;
            }
        }
    }
    return 0;
}
