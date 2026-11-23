/* src/downstream/uri.c
 *
 * URI 분리·디코딩·정규화. 명세: uri.h
 */
#include "downstream/uri.h"

#include <errno.h>
#include <string.h>

int uri_split(str_t target, str_t *out_path, str_t *out_query)
{
    const char *hash;
    const char *q;
    size_t n;

    if (target.len == 0) {
        errno = EINVAL;
        return -1;
    }
    hash = memchr(target.ptr, '#', target.len);
    n = hash != NULL ? (size_t)(hash - target.ptr) : target.len;
    q = memchr(target.ptr, '?', n);
    if (q == NULL) {
        out_path->ptr = target.ptr;
        out_path->len = n;
        out_query->ptr = NULL;
        out_query->len = 0;
        return 0;
    }
    out_path->ptr = target.ptr;
    out_path->len = (size_t)(q - target.ptr);
    out_query->ptr = q + 1;
    out_query->len = n - out_path->len - 1;
    return 0;
}

int uri_parse_absolute(str_t target, str_t *out_authority, str_t *out_rest)
{
    static const char root[] = "/";
    const size_t scheme_len = 7; /* "http://" */
    str_t scheme;
    size_t i;

    if (target.len < scheme_len) {
        errno = EINVAL;
        return -1;
    }
    scheme.ptr = target.ptr;
    scheme.len = scheme_len;
    if (!str_eq_ci(scheme, "http://")) {
        errno = EINVAL;
        return -1;
    }
    i = scheme_len;
    while (i < target.len && target.ptr[i] != '/' && target.ptr[i] != '?' && target.ptr[i] != '#') {
        i++;
    }
    if (i == scheme_len || memchr(target.ptr + scheme_len, '@', i - scheme_len) != NULL) {
        errno = EINVAL;
        return -1;
    }
    if (i < target.len && target.ptr[i] != '/') {
        errno = EINVAL;
        return -1;
    }
    out_authority->ptr = target.ptr + scheme_len;
    out_authority->len = i - scheme_len;
    if (i == target.len) {
        out_rest->ptr = root;
        out_rest->len = 1;
    } else {
        out_rest->ptr = target.ptr + i;
        out_rest->len = target.len - i;
    }
    return 0;
}

/* 16진 숫자 하나의 값. 16진 숫자가 아니면 -1. */
static int hex_val(char c)
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

int uri_decode_path(str_t in, char *out, size_t cap, size_t *out_len)
{
    size_t w = 0;

    for (size_t r = 0; r < in.len; r++) {
        char c = in.ptr[r];

        if (c == '%') {
            int hi;
            int lo;

            if (r + 2 >= in.len) {
                errno = EINVAL;
                return -1;
            }
            hi = hex_val(in.ptr[r + 1]);
            lo = hex_val(in.ptr[r + 2]);
            if (hi < 0 || lo < 0 || (hi == 0 && lo == 0)) {
                errno = EINVAL;
                return -1;
            }
            c = (char)(hi * 16 + lo);
            r += 2;
        }
        if (w + 1 >= cap) {
            errno = ENAMETOOLONG;
            return -1;
        }
        out[w++] = c;
    }
    if (w >= cap) {
        errno = ENAMETOOLONG;
        return -1;
    }
    out[w] = '\0';
    if (out_len != NULL) {
        *out_len = w;
    }
    return 0;
}

int path_normalize(char *path)
{
    size_t r = 0;
    size_t w = 1; /* path[0..w)는 결과. 항상 '/'로 끝난다 */

    if (path[0] != '/') {
        errno = EINVAL;
        return -1;
    }
    while (path[r] != '\0') {
        size_t start;
        size_t n;

        while (path[r] == '/') {
            r++;
        }
        if (path[r] == '\0') {
            break;
        }
        start = r;
        while (path[r] != '\0' && path[r] != '/') {
            r++;
        }
        n = r - start;
        if (n == 1 && path[start] == '.') {
            continue;
        }
        if (n == 2 && path[start] == '.' && path[start + 1] == '.') {
            if (w == 1) {
                errno = EPERM;
                return -1;
            }
            w--; /* 끝의 '/' */
            while (path[w - 1] != '/') {
                w--;
            }
            continue;
        }
        memmove(path + w, path + start, n);
        w += n;
        if (path[r] == '/') {
            path[w++] = '/';
        }
    }
    path[w] = '\0';
    return 0;
}
