/* src/common/http_headers.c
 *
 * 헤더 끝 탐색·파싱·쓰기. 명세: http_headers.h
 */
#include "common/http_headers.h"

#include <errno.h>
#include <string.h>

/* HTTP 헤더 이름은 ASCII 대소문자만 접어서 비교한다. 로케일 규칙은 쓰지 않는다. */
static unsigned char ascii_lower(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') {
        return (unsigned char)(c + ('a' - 'A'));
    }
    return c;
}

/* 길이와 각 바이트를 비교해, 빌린 문자열 뷰와 NUL 종료 리터럴이 같은지 검사한다. */
static bool str_eq_ci_lit(str_t s, const char *lit)
{
    size_t n = strlen(lit);
    if (s.len != n || (n != 0 && s.ptr == NULL)) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (ascii_lower((unsigned char)s.ptr[i]) != ascii_lower((unsigned char)lit[i])) {
            return false;
        }
    }
    return true;
}

/* 두 문자열 뷰를 ASCII 대소문자 무시로 비교한다. */
static bool str_eq_ci_view(str_t a, str_t b)
{
    if (a.len != b.len || (a.len != 0 && (a.ptr == NULL || b.ptr == NULL))) {
        return false;
    }
    for (size_t i = 0; i < a.len; i++) {
        if (ascii_lower((unsigned char)a.ptr[i]) != ascii_lower((unsigned char)b.ptr[i])) {
            return false;
        }
    }
    return true;
}

/* 헤더 값의 양끝에서 HTTP OWS(SP, HTAB)만 떼고 원본을 가리키는 뷰를 돌려준다. */
static str_t trim_ows(str_t s)
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

/* 역할: 앞 빈 줄을 건너뛰고 요청 머리의 첫 종료 위치를 찾아 상한과 비교한다. */
http_parse_result_t http_find_head_end(const char *p, size_t len, size_t max_head, size_t *out_skip,
                                       size_t *out_head_len, int *out_status)
{
    size_t skip = 0;
    size_t end = 0;

    if (out_skip == NULL || out_head_len == NULL || out_status == NULL || (len != 0 && p == NULL)) {
        if (out_status != NULL) {
            *out_status = 431;
        }
        return HTTP_PARSE_ERROR;
    }

    /* 메시지 시작 전의 빈 줄을 건너뛴다. CR만 온 경우는 다음 바이트가 LF일 수 있어 보류한다. */
    while (skip < len) {
        if (p[skip] == '\n') {
            skip++;
        } else if (p[skip] == '\r' && skip + 1 < len && p[skip + 1] == '\n') {
            skip += 2;
        } else {
            break;
        }
    }

    /* 네 종류의 허용된 빈 줄 표기를 검사한다. 첫 종료 위치 뒤의 본문은 보지 않는다. */
    for (size_t i = skip; i < len; i++) {
        size_t term = 0;
        if (p[i] == '\r' && i + 3 < len && p[i + 1] == '\n' && p[i + 2] == '\r' &&
            p[i + 3] == '\n') {
            term = 4;
        } else if (p[i] == '\r' && i + 2 < len && p[i + 1] == '\n' && p[i + 2] == '\n') {
            term = 3;
        } else if (p[i] == '\n' && i + 2 < len && p[i + 1] == '\r' && p[i + 2] == '\n') {
            term = 3;
        } else if (p[i] == '\n' && i + 1 < len && p[i + 1] == '\n') {
            term = 2;
        }
        if (term != 0) {
            end = i + term;
            /* 상한은 본문을 뺀 요청 머리 길이로 판단한다. */
            if (end > max_head) {
                *out_status = 431;
                return HTTP_PARSE_ERROR;
            }
            *out_skip = skip;
            *out_head_len = end;
            return HTTP_PARSE_DONE;
        }
    }

    if (len > max_head) {
        *out_status = 431;
        return HTTP_PARSE_ERROR;
    }
    return HTTP_PARSE_INCOMPLETE;
}

/* 역할: 헤더 줄을 검사해 이름/값 뷰 목록으로 만든다. 입력 메모리는 복사하지 않는다. */
int http_parse_headers(const char *p, size_t len, size_t max_headers, http_headers_t *out_h,
                       int *out_status)
{
    http_headers_t h = {0};
    size_t pos = 0;
    size_t limit = max_headers < HTTP_MAX_HEADERS ? max_headers : HTTP_MAX_HEADERS;

    if (out_h == NULL || out_status == NULL || (len != 0 && p == NULL)) {
        if (out_status != NULL) {
            *out_status = 400;
        }
        return -1;
    }
    /* 성공할 때만 임시 목록을 out_h에 복사해, 실패 시 출력이 반쯤 채워지지 않게 한다. */
    while (pos < len) {
        size_t line_start = pos;
        size_t line_end;
        size_t colon;
        str_t name;
        str_t value;

        /* LF까지 한 줄을 모으고, 바로 앞의 CR이 있으면 CRLF로 처리한다. */
        while (pos < len && p[pos] != '\n') {
            pos++;
        }
        if (pos == len) {
            *out_status = 400;
            return -1;
        }
        line_end = pos;
        pos++;
        if (line_end > line_start && p[line_end - 1] == '\r') {
            line_end--;
        }
        if (line_end == line_start || p[line_start] == ' ' || p[line_start] == '\t') {
            *out_status = 400;
            return -1;
        }
        if (h.n >= limit) {
            *out_status = 431;
            return -1;
        }
        /* 첫 콜론에서 이름과 값을 나눈다. 이름 안의 공백은 token 검사에서 거부된다. */
        colon = line_start;
        while (colon < line_end && p[colon] != ':') {
            colon++;
        }
        if (colon == line_start || colon == line_end) {
            *out_status = 400;
            return -1;
        }
        name.ptr = p + line_start;
        name.len = colon - line_start;
        if (!http_token_valid(name)) {
            *out_status = 400;
            return -1;
        }
        value.ptr = p + colon + 1;
        value.len = line_end - colon - 1;
        /* 헤더 값의 제어 문자는 HTAB만 허용한다. */
        for (size_t i = 0; i < value.len; i++) {
            unsigned char c = (unsigned char)value.ptr[i];
            if ((c < 0x20 && c != '\t') || c == 0x7f) {
                *out_status = 400;
                return -1;
            }
        }
        h.v[h.n].name = name;
        h.v[h.n].value = trim_ows(value);
        h.n++;
    }
    *out_h = h;
    return 0;
}

/* 역할: 대소문자 무시로 헤더 하나를 찾아 첫 번째 일치 항목을 돌려준다. */
const http_header_t *http_header_get(const http_headers_t *h, const char *name)
{
    if (h == NULL || name == NULL) {
        return NULL;
    }
    /* 중복 헤더는 받은 순서를 유지하므로 첫 번째 일치 항목을 돌려준다. */
    for (size_t i = 0; i < h->n && i < HTTP_MAX_HEADERS; i++) {
        if (str_eq_ci_lit(h->v[i].name, name)) {
            return &h->v[i];
        }
    }
    return NULL;
}

/* 역할: 특정 헤더가 몇 번 들어왔는지 센다. */
size_t http_header_count(const http_headers_t *h, const char *name)
{
    size_t count = 0;
    if (h == NULL || name == NULL) {
        return 0;
    }
    /* Host처럼 중복 개수가 규칙에 포함되는 헤더를 위해 모든 항목을 센다. */
    for (size_t i = 0; i < h->n && i < HTTP_MAX_HEADERS; i++) {
        if (str_eq_ci_lit(h->v[i].name, name)) {
            count++;
        }
    }
    return count;
}

/* 역할: 헤더 이름/메서드에 쓸 수 있는 RFC token 문자만 포함됐는지 검사한다. */
bool http_token_valid(str_t s)
{
    if (s.ptr == NULL || s.len == 0) {
        return false;
    }
    /* RFC token은 영문자·숫자와 아래 기호만으로 이뤄진다. */
    for (size_t i = 0; i < s.len; i++) {
        unsigned char c = (unsigned char)s.ptr[i];
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            continue;
        }
        switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+':
        case '-': case '.': case '^': case '_': case '`': case '|': case '~':
            break;
        default:
            return false;
        }
    }
    return true;
}

/* 역할: 고정 목록과 Connection 목록을 확인해 전달에서 뺄 hop-by-hop 헤더인지 판정한다. */
bool http_is_hop_by_hop(str_t name, const http_headers_t *h)
{
    static const char *const fixed[] = {"Connection", "Keep-Alive", "Proxy-Connection", "TE",
                                        "Trailer", "Transfer-Encoding", "Upgrade",
                                        "Proxy-Authenticate", "Proxy-Authorization"};
    /* 프로토콜에서 항상 hop-by-hop인 고정 이름을 먼저 확인한다. */
    for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
        if (str_eq_ci_lit(name, fixed[i])) {
            return true;
        }
    }
    if (h == NULL) {
        return false;
    }
    /* Connection 헤더가 추가로 지목한 이름도 hop-by-hop으로 본다. */
    for (size_t i = 0; i < h->n && i < HTTP_MAX_HEADERS; i++) {
        str_t hn = h->v[i].name;
        str_t v = h->v[i].value;
        size_t pos = 0;
        if (!str_eq_ci_lit(hn, "Connection")) {
            continue;
        }
        /* 쉼표로 나눈 항목마다 OWS를 제거한 뒤 요청 이름과 비교한다. */
        while (pos <= v.len) {
            size_t end = pos;
            str_t item;
            while (end < v.len && v.ptr[end] != ',') {
                end++;
            }
            item.ptr = v.ptr == NULL ? NULL : v.ptr + pos;
            item.len = end - pos;
            item = trim_ows(item);
            if (item.len != 0 && str_eq_ci_view(name, item)) {
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

/* 역할: 헤더 목록을 HTTP 줄 형식으로 버퍼 뒤에 직렬화한다. */
int http_headers_write(const http_headers_t *h, bool skip_hop_by_hop, buf_t *out)
{
    size_t old_len;
    if (h == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }
    /* 실패 시 이번 호출이 덧붙인 바이트만 되돌리도록 시작 위치를 기억한다. */
    old_len = out->len;
    for (size_t i = 0; i < h->n && i < HTTP_MAX_HEADERS; i++) {
        const http_header_t *item = &h->v[i];
        if (skip_hop_by_hop && http_is_hop_by_hop(item->name, h)) {
            continue;
        }
        if (buf_append(out, item->name.ptr, item->name.len) != 0 ||
            buf_append(out, ": ", 2) != 0 ||
            buf_append(out, item->value.ptr, item->value.len) != 0 ||
            buf_append(out, "\r\n", 2) != 0) {
            out->len = old_len;
            return -1;
        }
    }
    return 0;
}
