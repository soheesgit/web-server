#ifndef HTTPD_HANDLER_INTERNAL_PATH_H
#define HTTPD_HANDLER_INTERNAL_PATH_H

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/str.h"

/** 파일 경로 조회 실패를 처리기의 HTTP 상태로 바꾼다. */
static inline int handler_path_status(int error)
{
    switch (error) {
    case ENOENT:
    case ENOTDIR:
        return 404;
    case EACCES:
    case EPERM:
    case ELOOP:
        return 403;
    default:
        return 500;
    }
}

/**
 * 정규화된 루트 아래 상대 경로를 확인한다.
 *
 * 성공하면 out_path(PATH_MAX 바이트)에 실경로를 쓰고 0을 돌려준다.
 * 루트 밖 또는 경로 조회 실패면 HTTP 상태를 돌려준다.
 */
static inline int handler_resolve_path(const char *root, str_t relative, char *out_path)
{
    char candidate[PATH_MAX];
    size_t root_len = strlen(root);
    const char *sep = root_len == 1 ? "" : "/";
    int n = snprintf(candidate, sizeof(candidate), "%s%s%.*s", root, sep, (int)relative.len,
                     relative.len == 0 ? "" : relative.ptr);

    if (n < 0 || (size_t)n >= sizeof(candidate)) {
        return 500;
    }
    if (realpath(candidate, out_path) == NULL) {
        return handler_path_status(errno);
    }
    if (root_len != 1 && (strncmp(root, out_path, root_len) != 0 ||
                          (out_path[root_len] != '\0' && out_path[root_len] != '/'))) {
        return 403;
    }
    return 0;
}

#endif
