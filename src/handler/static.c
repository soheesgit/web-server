/* src/handler/static.c
 *
 * 정적 파일 처리기. 명세: handler.h
 */
#include "handler/handler.h"

#include "common/http_headers.h"
#include "handler/mime.h"
#include "handler/plan.h"
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int path_inside_root(const char *root, const char *path)
{
    size_t n = strlen(root);
    if (strncmp(root, path, n) != 0) return 0;
    return path[n] == '\0' || path[n] == '/' || (n > 0 && root[n - 1] == '/');
}

static int make_path(const http_request_t *req, const route_t *rt, char *out, size_t cap)
{
    const char *suffix = req->path;
    size_t prefix_len = rt->prefix_len;
    int n;
    if (strncmp(suffix, rt->prefix, prefix_len) == 0) suffix += prefix_len;
    while (*suffix == '/') suffix++;
    n = snprintf(out, cap, "%s/%s", rt->root, suffix);
    return n >= 0 && (size_t)n < cap ? 0 : -1;
}

static int map_file_error(int e)
{
    if (e == ENOENT || e == ENOTDIR) return 404;
    if (e == EACCES || e == EPERM || e == ELOOP) return 403;
    return 500;
}

static int set_date_header(response_plan_t *plan, time_t when)
{
    struct tm tmv;
    char date[64];
    if (gmtime_r(&when, &tmv) == NULL || strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &tmv) == 0)
        return -1;
    return plan_add_header(plan, "Last-Modified", date);
}

/* If-Modified-Since가 IMF-fixdate이고 파일보다 같거나 뒤면 본문을 보내지 않는다. */
static bool is_not_modified_since(const http_request_t *req, time_t modified)
{
    const http_header_t *header;
    char value[64];
    struct tm tmv;
    char *end;
    time_t since;

    header = http_header_get(&req->headers, "If-Modified-Since");
    if (header == NULL || header->value.len == 0 || header->value.len >= sizeof(value)) {
        return false;
    }
    memcpy(value, header->value.ptr, header->value.len);
    value[header->value.len] = '\0';
    memset(&tmv, 0, sizeof(tmv));
    end = strptime(value, "%a, %d %b %Y %H:%M:%S GMT", &tmv);
    if (end == NULL || *end != '\0') {
        return false;
    }
    since = timegm(&tmv);
    return since != (time_t)-1 && modified <= since;
}

handler_result_t static_handle(const http_request_t *req, const route_t *rt,
                               const handler_ctx_t *ctx, response_plan_t *out_plan)
{
    char path[PATH_MAX], canonical_root[PATH_MAX], canonical_file[PATH_MAX];
    struct stat st;
    int fd, status;
    if (req == NULL || rt == NULL || ctx == NULL || out_plan == NULL || rt->root == NULL) {
        if (out_plan != NULL) (void)plan_error(out_plan, 500);
        return HANDLER_DONE;
    }
    if (req->method != HTTP_GET && req->method != HTTP_HEAD) {
        (void)plan_error(out_plan, 405);
        (void)plan_add_header(out_plan, "Allow", "GET, HEAD");
        return HANDLER_DONE;
    }
    if (make_path(req, rt, path, sizeof(path)) != 0 || realpath(rt->root, canonical_root) == NULL) {
        (void)plan_error(out_plan, 403);
        return HANDLER_DONE;
    }
retry_path:
    if (realpath(path, canonical_file) == NULL) {
        (void)plan_error(out_plan, map_file_error(errno));
        return HANDLER_DONE;
    }
    if (!path_inside_root(canonical_root, canonical_file)) {
        (void)plan_error(out_plan, 403);
        return HANDLER_DONE;
    }
    /* 검사한 정규 경로를 연다. 원래 경로를 열면 검사와 열기 사이에 심볼릭 링크가 바뀔 수 있다. */
    fd = open(canonical_file, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        (void)plan_error(out_plan, map_file_error(errno));
        return HANDLER_DONE;
    }
    if (fstat(fd, &st) < 0) {
        status = map_file_error(errno);
        (void)close(fd);
        (void)plan_error(out_plan, status);
        return HANDLER_DONE;
    }
    if (S_ISDIR(st.st_mode)) {
        size_t plen = strlen(req->path);
        if (plen == 0 || req->path[plen - 1] != '/') {
            char location[HTTP_PATH_MAX + 2048];
            char query[2048];
            if (str_to_cstr(req->query, query, sizeof(query)) != 0) query[0] = '\0';
            (void)close(fd);
            status = snprintf(location, sizeof(location), "%s/%s%s%s", req->path,
                              query[0] == '\0' ? "" : "?", query[0] == '\0' ? "" : query, "");
            if (status < 0 || (size_t)status >= sizeof(location) ||
                plan_set_mem(out_plan, 301, NULL, NULL, 0) != 0 ||
                plan_add_header(out_plan, "Location", location) != 0) {
                (void)plan_error(out_plan, 500);
            }
            return HANDLER_DONE;
        }
        (void)close(fd);
        if (strlen(path) + sizeof("index.html") >= sizeof(path)) {
            (void)plan_error(out_plan, 403);
            return HANDLER_DONE;
        }
        {
            size_t path_len = strlen(path);
            memcpy(path + path_len, "index.html", sizeof("index.html"));
        }
        goto retry_path;
    }
    if (!S_ISREG(st.st_mode)) {
        (void)close(fd);
        (void)plan_error(out_plan, 403);
        return HANDLER_DONE;
    }
    if (is_not_modified_since(req, st.st_mtime)) {
        (void)close(fd);
        plan_cleanup(out_plan);
        out_plan->kind = BODY_NONE;
        out_plan->head.status = 304;
        out_plan->head.framing = (http_framing_t){FRAMING_NONE, 0};
        if (set_date_header(out_plan, st.st_mtime) != 0) {
            plan_cleanup(out_plan);
            (void)plan_error(out_plan, 500);
        }
        return HANDLER_DONE;
    }
    plan_cleanup(out_plan);
    out_plan->kind = BODY_FILE;
    out_plan->body.file.fd = fd;
    out_plan->body.file.off = 0;
    out_plan->body.file.len = st.st_size;
    out_plan->head.status = 200;
    out_plan->head.framing = (http_framing_t){FRAMING_LENGTH, (uint64_t)st.st_size};
    if (plan_add_header(out_plan, "Content-Type", mime_for_path(path)) != 0 ||
        set_date_header(out_plan, st.st_mtime) != 0) {
        plan_cleanup(out_plan);
        (void)plan_error(out_plan, 500);
    }
    return HANDLER_DONE;
}
