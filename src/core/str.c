/* src/core/str.c
 *
 * 빌린 문자열 뷰. 명세: str.h
 */
#include "core/str.h"

#include <errno.h>
#include <string.h>

static unsigned char ascii_lower(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + ('a' - 'A')) : c;
}

str_t str_from_cstr(const char *s)
{
    if (s == NULL) {
        return (str_t){NULL, 0};
    }
    return (str_t){s, strlen(s)};
}

bool str_eq(str_t s, const char *lit)
{
    size_t len;
    if (lit == NULL || (s.ptr == NULL && s.len != 0)) {
        return false;
    }
    len = strlen(lit);
    return s.len == len && (len == 0 || memcmp(s.ptr, lit, len) == 0);
}

bool str_eq_ci(str_t s, const char *lit)
{
    size_t i, len;
    if (lit == NULL || (s.ptr == NULL && s.len != 0)) {
        return false;
    }
    len = strlen(lit);
    if (s.len != len) {
        return false;
    }
    for (i = 0; i < len; i++) {
        if (ascii_lower((unsigned char)s.ptr[i]) != ascii_lower((unsigned char)lit[i])) {
            return false;
        }
    }
    return true;
}

bool str_eq_ci_str(str_t a, str_t b)
{
    size_t i;
    if ((a.ptr == NULL && a.len != 0) || (b.ptr == NULL && b.len != 0) || a.len != b.len) {
        return false;
    }
    for (i = 0; i < a.len; i++) {
        if (ascii_lower((unsigned char)a.ptr[i]) != ascii_lower((unsigned char)b.ptr[i])) {
            return false;
        }
    }
    return true;
}

str_t str_trim_ows(str_t s)
{
    size_t begin = 0, end = s.len;
    if (s.ptr == NULL && s.len != 0) {
        return (str_t){NULL, 0};
    }
    while (begin < end && (s.ptr[begin] == ' ' || s.ptr[begin] == '\t')) {
        begin++;
    }
    while (end > begin && (s.ptr[end - 1] == ' ' || s.ptr[end - 1] == '\t')) {
        end--;
    }
    return (str_t){s.ptr == NULL ? NULL : s.ptr + begin, end - begin};
}

int str_to_u64(str_t s, uint64_t max, uint64_t *out_v)
{
    uint64_t value = 0;
    size_t i;
    if (out_v == NULL || s.ptr == NULL || s.len == 0) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0; i < s.len; i++) {
        unsigned char c = (unsigned char)s.ptr[i];
        uint64_t digit;
        if (c < '0' || c > '9') {
            errno = EINVAL;
            return -1;
        }
        digit = (uint64_t)(c - '0');
        if (value > (UINT64_MAX - digit) / 10) {
            errno = ERANGE;
            return -1;
        }
        value = value * 10 + digit;
        if (value > max) {
            errno = ERANGE;
            return -1;
        }
    }
    *out_v = value;
    return 0;
}

int str_to_cstr(str_t s, char *out, size_t cap)
{
    if ((s.ptr == NULL && s.len != 0) || (out == NULL && cap != 0)) {
        errno = EINVAL;
        return -1;
    }
    if (s.len == SIZE_MAX || s.len + 1 > cap) {
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
