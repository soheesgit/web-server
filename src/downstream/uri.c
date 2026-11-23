/* src/downstream/uri.c
 *
 * URI 분리·디코딩·정규화. 명세: uri.h
 */
#include "downstream/uri.h"

#include <errno.h>
#include <string.h>

/* URI scheme 비교에서 ASCII 영문자만 소문자로 접는다. */
static unsigned char lower_ascii(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

/* 입력이 대소문자 구분 없이 http://로 시작하는지 확인한다. */
static bool has_http_scheme(str_t s)
{
    static const char scheme[] = "http://";
    if (s.len < sizeof(scheme) - 1 || s.ptr == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(scheme) - 1; i++) {
        if (lower_ascii((unsigned char)s.ptr[i]) != (unsigned char)scheme[i]) {
            return false;
        }
    }
    return true;
}

/* 역할: request-target에서 fragment를 버리고 경로와 query 부분 뷰를 만든다. */
int uri_split(str_t target, str_t *out_path, str_t *out_query)
{
    size_t end;
    size_t query;
    if (target.len == 0 || target.ptr == NULL || out_path == NULL || out_query == NULL) {
        errno = EINVAL;
        return -1;
    }
    /* fragment(# 뒤)는 버린 다음, 남은 부분에서 query(? 뒤)를 분리한다. */
    end = target.len;
    for (size_t i = 0; i < target.len; i++) {
        if (target.ptr[i] == '#') {
            end = i;
            break;
        }
    }
    query = end;
    for (size_t i = 0; i < end; i++) {
        if (target.ptr[i] == '?') {
            query = i;
            break;
        }
    }
    out_path->ptr = target.ptr;
    out_path->len = query;
    if (query < end) {
        out_query->ptr = target.ptr + query + 1;
        out_query->len = end - query - 1;
    } else {
        out_query->ptr = NULL;
        out_query->len = 0;
    }
    return 0;
}

/* 역할: http:// authority를 분리해 Host 후보와 경로/query 부분 뷰를 만든다. */
int uri_parse_absolute(str_t target, str_t *out_authority, str_t *out_rest)
{
    static const char root[] = "/";
    size_t start = sizeof("http://") - 1;
    size_t end;
    if (out_authority == NULL || out_rest == NULL || !has_http_scheme(target)) {
        errno = EINVAL;
        return -1;
    }
    /* scheme 뒤에서 첫 경로·query·fragment 기호까지를 authority로 본다. */
    end = start;
    while (end < target.len && target.ptr[end] != '/' && target.ptr[end] != '?' &&
           target.ptr[end] != '#') {
        unsigned char c = (unsigned char)target.ptr[end];
        if (c <= 0x20 || c == 0x7f || c == '@') {
            errno = EINVAL;
            return -1;
        }
        end++;
    }
    if (end == start) {
        errno = EINVAL;
        return -1;
    }
    out_authority->ptr = target.ptr + start;
    out_authority->len = end - start;
    /* 경로가 없는 absolute-form은 루트 경로로 취급한다. query나 fragment만 붙은 형식은 거부한다. */
    if (end == target.len) {
        out_rest->ptr = root;
        out_rest->len = 1;
    } else if (target.ptr[end] != '/') {
        errno = EINVAL;
        return -1;
    } else {
        out_rest->ptr = target.ptr + end;
        out_rest->len = target.len - end;
    }
    return 0;
}

/* 퍼센트 인코딩 두 자리의 16진 숫자 값을 돌려준다. */
static int hex_value(unsigned char c)
{
    if (c >= '0' && c <= '9') {
        return (int)(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return (int)(c - 'a') + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return (int)(c - 'A') + 10;
    }
    return -1;
}

/* 역할: 경로의 %XX를 한 번 디코딩하고 NUL 종료 문자열로 쓴다. */
int uri_decode_path(str_t in, char *out, size_t cap, size_t *out_len)
{
    size_t written = 0;
    if ((in.len != 0 && in.ptr == NULL) || out == NULL || cap == 0) {
        errno = EINVAL;
        return -1;
    }
    /* 일반 바이트는 복사하고 %XX만 한 바이트로 바꾼다. '+'는 그대로 둔다. */
    for (size_t i = 0; i < in.len; i++) {
        unsigned char c = (unsigned char)in.ptr[i];
        if (c == '%') {
            int hi;
            int lo;
            if (i + 2 >= in.len || (hi = hex_value((unsigned char)in.ptr[i + 1])) < 0 ||
                (lo = hex_value((unsigned char)in.ptr[i + 2])) < 0) {
                errno = EINVAL;
                return -1;
            }
            c = (unsigned char)((hi << 4) | lo);
            i += 2;
            /* NUL은 C 문자열 경로를 끊으므로 디코딩 결과에 허용하지 않는다. */
            if (c == 0) {
                errno = EINVAL;
                return -1;
            }
        }
        if (written + 1 >= cap) {
            errno = ENAMETOOLONG;
            return -1;
        }
        out[written++] = (char)c;
    }
    out[written] = '\0';
    if (out_len != NULL) {
        *out_len = written;
    }
    return 0;
}

/* 역할: 절대 경로의 중복 슬래시와 . / .. 세그먼트를 제자리에서 정리한다. */
int path_normalize(char *path)
{
    size_t len;
    size_t read_pos;
    size_t write_pos = 1;
    bool trailing;
    if (path == NULL || path[0] != '/') {
        errno = EINVAL;
        return -1;
    }
    len = strlen(path);
    /* read_pos는 원본 세그먼트를 읽고 write_pos는 정규화 결과를 같은 배열 앞쪽에 쓴다. */
    trailing = len > 0 && path[len - 1] == '/';
    path[0] = '/';
    read_pos = 1;
    while (read_pos < len) {
        size_t start;
        size_t seg_len;
        while (read_pos < len && path[read_pos] == '/') {
            read_pos++;
        }
        start = read_pos;
        while (read_pos < len && path[read_pos] != '/') {
            read_pos++;
        }
        seg_len = read_pos - start;
        if (seg_len == 0) {
            continue;
        }
        /* 현재 디렉터리(.)는 제거한다. 마지막 점 세그먼트는 끝의 /를 보존한다. */
        if (seg_len == 1 && path[start] == '.') {
            if (read_pos == len) {
                trailing = true;
            }
            continue;
        }
        /* 부모 디렉터리(..)는 출력의 마지막 세그먼트를 지운다. 루트 위 이동은 거부한다. */
        if (seg_len == 2 && path[start] == '.' && path[start + 1] == '.') {
            if (write_pos == 1) {
                errno = EPERM;
                return -1;
            }
            while (write_pos > 1 && path[write_pos - 1] != '/') {
                write_pos--;
            }
            if (read_pos == len) {
                trailing = true;
            }
            continue;
        }
        /* 일반 세그먼트 사이에는 구분자 하나만 두고 연속 슬래시는 합친다. */
        if (write_pos > 1 && path[write_pos - 1] != '/') {
            path[write_pos++] = '/';
        }
        memmove(path + write_pos, path + start, seg_len);
        write_pos += seg_len;
    }
    if (trailing && write_pos > 1 && path[write_pos - 1] != '/') {
        path[write_pos++] = '/';
    } else if (!trailing && write_pos > 1 && path[write_pos - 1] == '/') {
        write_pos--;
    }
    path[write_pos] = '\0';
    return 0;
}
