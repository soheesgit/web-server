/* src/core/str.c
 *
 * 빌린 문자열 뷰. 명세: str.h
 */
#include "core/str.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

static unsigned char ascii_lower(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c - 'A' + 'a') : c;
}

static bool mem_eq_ci(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (ascii_lower((unsigned char)a[i]) != ascii_lower((unsigned char)b[i])) {
            return false;
        }
    }
    return true;
}

static bool is_ows(char c)
{
    return c == ' ' || c == '\t';
}

str_t str_from_cstr(const char *s)
{
    str_t r = {NULL, 0};

    if (s != NULL) {
        r.ptr = s;
        r.len = strlen(s);
    }
    return r;
}

bool str_eq(str_t s, const char *lit)
{
    size_t n = strlen(lit);

    if (s.len != n) {
        return false;
    }
    return n == 0 || memcmp(s.ptr, lit, n) == 0;
}

bool str_eq_ci(str_t s, const char *lit)
{
    size_t n = strlen(lit);

    if (s.len != n) {
        return false;
    }
    return n == 0 || mem_eq_ci(s.ptr, lit, n);
}

bool str_eq_ci_str(str_t a, str_t b)
{
    if (a.len != b.len) {
        return false;
    }
    return a.len == 0 || mem_eq_ci(a.ptr, b.ptr, a.len);
}

str_t str_trim_ows(str_t s)
{
    size_t start = 0;
    size_t end = s.len;

    while (start < end && is_ows(s.ptr[start])) {
        start++;
    }
    while (end > start && is_ows(s.ptr[end - 1])) {
        end--;
    }
    s.ptr = s.len == 0 ? s.ptr : s.ptr + start;
    s.len = end - start;
    return s;
}

int str_to_u64(str_t s, uint64_t max, uint64_t *out_v)
{
    uint64_t v = 0;

    if (s.len == 0) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < s.len; i++) {
        unsigned d;

        if (s.ptr[i] < '0' || s.ptr[i] > '9') {
            errno = EINVAL;
            return -1;
        }
        d = (unsigned)(s.ptr[i] - '0');
        if (v > (UINT64_MAX - d) / 10) {
            errno = ERANGE;
            return -1;
        }
        v = v * 10 + d;
    }
    if (v > max) {
        errno = ERANGE;
        return -1;
    }
    *out_v = v;
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
        memcpy(out, s.ptr, s.len);
    }
    out[s.len] = '\0';
    return 0;
}
