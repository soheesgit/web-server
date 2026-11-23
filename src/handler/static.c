/* src/handler/static.c
 *
 * 정적 파일 처리기. 명세: handler.h
 */
#include "handler/handler.h"
#include "handler/mime.h"
#include "handler/plan.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* realpath·open·fstat 결과를 상태 코드로 바꾼다 */
static int errno_status(int e)
{
    if (e == ENOENT || e == ENOTDIR) {
        return 404;
    }
    return e == EACCES ? 403 : 500;
}

/* real이 root 안인가 ('/' 경계) */
static bool inside_root(const char *root, const char *real)
{
    size_t n = strlen(root);

    if (n == 1) { /* root가 "/" */
        return real[0] == '/';
    }
    return strncmp(real, root, n) == 0 && (real[n] == '\0' || real[n] == '/');
}

/*
 * path를 루트 안에서 확인하고 연다 (명세 3~4단계). 성공하면 0과 *out_fd·*out_st,
 * 실패하면 상태 코드. fd는 O_NONBLOCK이 켜진 채다.
 */
static int open_checked(const char *root, const char *path, int *out_fd, struct stat *out_st)
{
    char real[PATH_MAX];
    int fd;

    if (realpath(path, real) == NULL) {
        return errno_status(errno);
    }
    if (!inside_root(root, real)) {
        return 403;
    }
    fd = open(real, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        return errno_status(errno);
    }
    if (fstat(fd, out_st) != 0) {
        int e = errno;
        (void)close(fd);
        return errno_status(e);
    }
    *out_fd = fd;
    return 0;
}

static handler_result_t fail(response_plan_t *p, int fd, int status)
{
    if (fd >= 0) {
        (void)close(fd);
    }
    (void)plan_error(p, status);
    return HANDLER_DONE;
}

static handler_result_t redirect_dir(const http_request_t *req, response_plan_t *p)
{
    char loc[HTTP_PATH_MAX + 2 + 8192];
    int n;

    if (req->query.len > 0) {
        n = snprintf(loc, sizeof(loc), "%s/?%.*s", req->path, (int)req->query.len, req->query.ptr);
    } else {
        n = snprintf(loc, sizeof(loc), "%s/", req->path);
    }
    if (n < 0 || (size_t)n >= sizeof(loc)) {
        return fail(p, -1, 500);
    }
    p->head.status = 301;
    p->kind = BODY_NONE;
    /* 본문은 없지만 keep-alive에서 끝을 알 수 있게 Content-Length: 0 */
    p->head.framing.kind = FRAMING_LENGTH;
    p->head.framing.length = 0;
    if (plan_add_header(p, "Location", loc) != 0) {
        return fail(p, -1, 500);
    }
    return HANDLER_DONE;
}

handler_result_t static_handle(const http_request_t *req, const route_t *rt,
                               const handler_ctx_t *ctx, response_plan_t *out_plan)
{
    char root[PATH_MAX];
    char path[PATH_MAX];
    char lm[64];
    const char *rest = req->path + rt->prefix_len;
    struct stat st;
    struct tm tm;
    int fd = -1;
    int flags;
    int n;
    int status;

    (void)ctx;
    if (req->method != HTTP_GET && req->method != HTTP_HEAD) {
        (void)plan_error(out_plan, 405);
        (void)plan_add_header(out_plan, "Allow", "GET, HEAD");
        return HANDLER_DONE;
    }
    if (realpath(rt->root, root) == NULL) {
        return fail(out_plan, -1, 500);
    }
    n = snprintf(path, sizeof(path), "%s%s%s", rt->root, rest[0] == '/' ? "" : "/", rest);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        return fail(out_plan, -1, 404);
    }
    status = open_checked(root, path, &fd, &st);
    if (status != 0) {
        return fail(out_plan, -1, status);
    }
    if (S_ISDIR(st.st_mode)) {
        size_t len = strlen(req->path);
        size_t plen = (size_t)n;

        (void)close(fd);
        fd = -1;
        if (len == 0 || req->path[len - 1] != '/') {
            return redirect_dir(req, out_plan);
        }
        n = snprintf(path + plen, sizeof(path) - plen, "%sindex.html",
                     path[plen - 1] == '/' ? "" : "/");
        if (n < 0 || (size_t)n >= sizeof(path) - plen || open_checked(root, path, &fd, &st) != 0) {
            return fail(out_plan, -1, 403);
        }
    }
    if (!S_ISREG(st.st_mode)) {
        return fail(out_plan, fd, 403);
    }
    flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) != 0) {
        return fail(out_plan, fd, 500);
    }
    if (gmtime_r(&st.st_mtime, &tm) == NULL ||
        strftime(lm, sizeof(lm), "%a, %d %b %Y %H:%M:%S GMT", &tm) == 0) {
        return fail(out_plan, fd, 500);
    }
    if (plan_add_header(out_plan, "Content-Type", mime_for_path(path)) != 0 ||
        plan_add_header(out_plan, "Last-Modified", lm) != 0) {
        return fail(out_plan, fd, 500);
    }
    out_plan->head.status = 200;
    out_plan->kind = BODY_FILE;
    out_plan->body.file.fd = fd;
    out_plan->body.file.off = 0;
    out_plan->body.file.len = st.st_size;
    out_plan->head.framing.kind = FRAMING_LENGTH;
    out_plan->head.framing.length = (uint64_t)st.st_size;
    return HANDLER_DONE;
}
