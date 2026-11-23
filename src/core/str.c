/* 문자열 뷰의 길이 기반 비교와 변환. */
#include "core/str.h"

#include <errno.h>
#include <string.h>

str_t str_from_cstr(const char *s)
{
    return (str_t){s, s == NULL ? 0 : strlen(s)};
}

bool str_eq(str_t s, const char *lit)
{
    return s.len == strlen(lit) && (s.len == 0 || memcmp(s.ptr, lit, s.len) == 0);
}

/* HTTP 이름의 ASCII 알파벳만 접는다. 로캘이나 UTF-8 바이트는 건드리지 않는다. */
static unsigned char ascii_lower(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

bool str_eq_ci(str_t s, const char *lit)
{
    return str_eq_ci_str(s, str_from_cstr(lit));
}

bool str_eq_ci_str(str_t a, str_t b)
{
    if (a.len != b.len) {
        return false;
    }
    for (size_t i = 0; i < a.len; ++i) {
        if (ascii_lower((unsigned char)a.ptr[i]) != ascii_lower((unsigned char)b.ptr[i])) {
            return false;
        }
    }
    return true;
}

static bool is_ows(char c)
{
    return c == ' ' || c == '\t';
}

str_t str_trim_ows(str_t s)
{
    size_t first = 0;
    size_t last = s.len;
    while (first < last && is_ows(s.ptr[first])) {
        ++first;
    }
    while (last > first && is_ows(s.ptr[last - 1])) {
        --last;
    }
    return first == last ? (str_t){s.ptr, 0} : (str_t){s.ptr + first, last - first};
}

int str_to_u64(str_t s, uint64_t max, uint64_t *out_v)
{
    if (s.len == 0) {
        errno = EINVAL;
        return -1;
    }

    uint64_t value = 0;
    const uint64_t limit = max / 10;
    const uint64_t last = max % 10;
    bool too_large = false;
    for (size_t i = 0; i < s.len; ++i) {
        unsigned char c = (unsigned char)s.ptr[i];
        if (c < '0' || c > '9') {
            errno = EINVAL;
            return -1;
        }
        unsigned digit = c - '0';
        /* 곱하기 전에 상한을 검사하고, 남은 바이트도 끝까지 숫자인지 확인한다. */
        too_large = too_large || value > limit || (value == limit && digit > last);
        if (!too_large) {
            value = value * 10 + digit;
        }
    }
    if (too_large) {
        errno = ERANGE;
        return -1;
    }
    *out_v = value;
    return 0;
}

int str_to_cstr(str_t s, char *out, size_t cap)
{
    if (s.len >= cap) {
        if (cap > 0) {
            out[0] = '\0';
        }
        errno = ENAMETOOLONG;
        return -1;
    }
    if (s.len > 0) {
        memmove(out, s.ptr, s.len);
    }
    out[s.len] = '\0';
    return 0;
}
