/* 요청 대상 분리와 정규화. */
#include "downstream/uri.h"

#include <errno.h>
#include <string.h>

int uri_split(str_t target, str_t *out_path, str_t *out_query)
{
    if (target.len == 0) {
        errno = EINVAL;
        return -1;
    }

    /* 첫 '?'에서 경로가 끝나고, 첫 '#'에서 사용할 대상의 범위가 끝난다. */
    size_t path_end = target.len;
    size_t query_start = target.len;
    size_t end = target.len;
    for (size_t i = 0; i < target.len; ++i) {
        char c = target.ptr[i];
        if (c == '#') {
            end = i;
            break;
        }
        if (c == '?' && path_end == target.len) {
            path_end = i;
            query_start = i + 1;
        }
    }
    if (path_end == target.len) {
        *out_path = (str_t){target.ptr, end};
        *out_query = (str_t){0};
    } else {
        *out_path = (str_t){target.ptr, path_end};
        *out_query = (str_t){target.ptr + query_start, end - query_start};
    }
    return 0;
}

int uri_parse_absolute(str_t target, str_t *out_authority, str_t *out_rest)
{
    enum { SCHEME_LEN = sizeof("http://") - 1 };
    if (target.len <= SCHEME_LEN || !str_eq_ci((str_t){target.ptr, SCHEME_LEN}, "http://")) {
        errno = EINVAL;
        return -1;
    }

    size_t path = SCHEME_LEN;
    while (path < target.len && target.ptr[path] != '/') {
        char c = target.ptr[path];
        if (c == '@' || c == '?' || c == '#') {
            errno = EINVAL;
            return -1;
        }
        ++path;
    }
    if (path == SCHEME_LEN) {
        errno = EINVAL;
        return -1;
    }
    /* 호스트 뒤에 경로를 생략한 절대 URI는 루트 경로로 처리한다. */
    *out_authority = (str_t){target.ptr + SCHEME_LEN, path - SCHEME_LEN};
    *out_rest = path < target.len ? (str_t){target.ptr + path, target.len - path} : STR_LIT("/");
    return 0;
}

static int hex_value(unsigned char c)
{
    if ('0' <= c && c <= '9') {
        return c - '0';
    }
    unsigned char lower = c | 0x20;
    if ('a' <= lower && lower <= 'f') {
        return lower - 'a' + 10;
    }
    return -1;
}

int uri_decode_path(str_t in, char *out, size_t cap, size_t *out_len)
{
    size_t written = 0;

    for (size_t cursor = 0; cursor < in.len;) {
        unsigned char c = (unsigned char)in.ptr[cursor++];
        if (c == '%') {
            if (in.len - cursor < 2) {
                errno = EINVAL;
                return -1;
            }
            int high = hex_value((unsigned char)in.ptr[cursor]);
            int low = hex_value((unsigned char)in.ptr[cursor + 1]);
            if (high < 0 || low < 0) {
                errno = EINVAL;
                return -1;
            }
            c = (unsigned char)(high << 4 | low);
            cursor += 2;
        }
        /* 디코딩 결과는 파일 경로 문자열로도 쓰므로 중간 NUL을 허용하지 않는다. */
        if (c == 0) {
            errno = EINVAL;
            return -1;
        }
        if (cap == 0 || written == cap - 1) {
            errno = ENAMETOOLONG;
            return -1;
        }
        out[written++] = (char)c;
    }
    if (cap == 0) {
        errno = ENAMETOOLONG;
        return -1;
    }
    out[written] = '\0';
    if (out_len != NULL) {
        *out_len = written;
    }
    return 0;
}

int path_normalize(char *path)
{
    if (*path != '/') {
        errno = EINVAL;
        return -1;
    }

    /* cursor는 원본을 읽고, used는 같은 버퍼 앞에 쌓인 정규화 결과의 길이를 나타낸다. */
    const size_t len = strlen(path);
    size_t used = 1;
    bool trailing_slash = false;
    for (size_t cursor = 1; cursor < len;) {
        size_t start = cursor;
        while (cursor < len && path[cursor] != '/') {
            ++cursor;
        }
        size_t width = cursor - start;
        bool dot = width == 1 && path[start] == '.';
        bool parent = width == 2 && path[start] == '.' && path[start + 1] == '.';
        trailing_slash = cursor < len || dot || parent;

        if (parent) {
            /* '..'는 마지막으로 쓴 경로 조각을 지운다. 루트에선 더 지울 조각이 없다. */
            if (used == 1) {
                errno = EPERM;
                return -1;
            }
            while (used > 1 && path[used - 1] != '/') {
                --used;
            }
            if (used > 1) {
                --used;
            }
        } else if (width != 0 && !dot) {
            if (used > 1) {
                path[used++] = '/';
            }
            memmove(path + used, path + start, width);
            used += width;
        }

        if (cursor < len) {
            ++cursor;
        }
    }
    /* 끝의 '/', '/.', '/..'가 가리키던 디렉터리 경로 표기는 유지한다. */
    if (trailing_slash && used > 1) {
        path[used++] = '/';
    }
    path[used] = '\0';
    return 0;
}
