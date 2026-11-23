#include "downstream/uri.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

int uri_split(str_t target, str_t *out_path, str_t *out_query)
{
    if (target.len == 0) {
        errno = EINVAL;
        return -1;
    }

    const char *fragment = memchr(target.ptr, '#', target.len);
    const char *end = fragment == NULL ? target.ptr + target.len : fragment;
    const char *query = memchr(target.ptr, '?', (size_t)(end - target.ptr));
    const char *path_end = query == NULL ? end : query;
    const char *query_start = query == NULL ? end : query + 1;

    *out_path = (str_t){target.ptr, (size_t)(path_end - target.ptr)};
    *out_query = (str_t){query_start, (size_t)(end - query_start)};
    return 0;
}

int uri_parse_absolute(str_t target, str_t *out_authority, str_t *out_rest)
{
    static const char scheme[] = "http://";
    size_t start = sizeof(scheme) - 1;

    if (target.len < start || !str_eq_ci((str_t){target.ptr, start}, scheme)) {
        errno = EINVAL;
        return -1;
    }

    size_t end = start;
    while (end < target.len && strchr("/?#", target.ptr[end]) == NULL) {
        end++;
    }
    if (end == start || (end < target.len && target.ptr[end] != '/') ||
        memchr(target.ptr + start, '@', end - start) != NULL) {
        errno = EINVAL;
        return -1;
    }

    *out_authority = (str_t){target.ptr + start, end - start};
    *out_rest = end == target.len ? (str_t){"/", 1} : (str_t){target.ptr + end, target.len - end};
    return 0;
}

static int hex_digit(unsigned char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c |= 0x20;
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

int uri_decode_path(str_t in, char *out, size_t cap, size_t *out_len)
{
    size_t written = 0;
    if (cap == 0) {
        errno = ENAMETOOLONG;
        return -1;
    }
    for (size_t i = 0; i < in.len; i++) {
        unsigned char ch = (unsigned char)in.ptr[i];
        if (ch == '%') {
            if (in.len - i < 3) {
                errno = EINVAL;
                return -1;
            }
            int hi = hex_digit((unsigned char)in.ptr[i + 1]);
            int lo = hex_digit((unsigned char)in.ptr[i + 2]);
            if (hi < 0 || lo < 0) {
                errno = EINVAL;
                return -1;
            }
            ch = (unsigned char)(hi * 16 + lo);
            i += 2;
        }
        if (ch == '\0') {
            errno = EINVAL;
            return -1;
        }
        if (written == cap - 1) {
            errno = ENAMETOOLONG;
            return -1;
        }
        out[written++] = (char)ch;
    }
    out[written] = '\0';
    if (out_len != NULL) {
        *out_len = written;
    }
    return 0;
}

int path_normalize(char *path)
{
    if (path[0] != '/') {
        errno = EINVAL;
        return -1;
    }

    char *read_at = path + 1;
    char *write_at = read_at;
    bool trailing_slash = false;
    while (*read_at != '\0') {
        if (*read_at == '/') {
            trailing_slash = true;
            read_at++;
            continue;
        }

        char *segment = read_at;
        while (*read_at != '\0' && *read_at != '/') {
            read_at++;
        }
        size_t len = (size_t)(read_at - segment);
        if (len == 1 && segment[0] == '.') {
            trailing_slash = true;
            continue;
        }
        if (len == 2 && segment[0] == '.' && segment[1] == '.') {
            if (write_at == path + 1) {
                errno = EPERM;
                return -1;
            }
            while (write_at > path + 1 && write_at[-1] != '/') {
                write_at--;
            }
            if (write_at > path + 1) {
                write_at--;
            }
            trailing_slash = true;
            continue;
        }
        if (write_at > path + 1) {
            *write_at++ = '/';
        }
        memmove(write_at, segment, len);
        write_at += len;
        trailing_slash = false;
    }
    if (trailing_slash && write_at > path + 1) {
        *write_at++ = '/';
    }
    *write_at = '\0';
    return 0;
}
