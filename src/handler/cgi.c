/* src/handler/cgi.c
 *
 * CGI 처리기. 명세: handler.h
 */
#include "handler/handler.h"
#include "handler/plan.h"

#include "common/http_headers.h"
#include "core/buf.h"
#include "core/platform.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CGI_FIXED_ENV 14
#define CGI_MAX_ENV (CGI_FIXED_ENV + HTTP_MAX_HEADERS + 1)
#define CGI_FIELDS_MAX (64 * 1024)

/* ── 응답 머리 파싱 ─────────────────────────────────────────────────────── */

/* "NNN [사유]" → 100~599, 형식 오류면 -1 */
static int parse_status(str_t v)
{
    int st = 0;

    if (v.len < 3 || (v.len > 3 && v.ptr[3] != ' ')) {
        return -1;
    }
    for (size_t i = 0; i < 3; i++) {
        if (v.ptr[i] < '0' || v.ptr[i] > '9') {
            return -1;
        }
        st = st * 10 + (v.ptr[i] - '0');
    }
    return st >= 100 && st <= 599 ? st : -1;
}

/* 응답에 옮기지 않는 헤더 */
static bool dropped(str_t name)
{
    return http_is_hop_by_hop(name, NULL) || str_eq_ci(name, "Date") || str_eq_ci(name, "Server");
}

static http_parse_result_t head_error(http_head_out_t *h, size_t fields_len, int *out_status)
{
    h->fields.len = fields_len;
    *out_status = 502;
    return HTTP_PARSE_ERROR;
}

http_parse_result_t cgi_parse_head(const char *p, size_t len, size_t max_head,
                                   http_head_out_t *out_head, size_t *out_used, int *out_status)
{
    size_t start_len;
    size_t pos = 0;
    int status = 0;
    bool location = false;
    bool has_length = false;
    uint64_t length = 0;

    if (out_head->fields.max == 0 && buf_init(&out_head->fields, 256, CGI_FIELDS_MAX) != 0) {
        *out_status = 502;
        return HTTP_PARSE_ERROR;
    }
    start_len = out_head->fields.len;
    for (;;) {
        const char *nl = len > pos ? memchr(p + pos, '\n', len - pos) : NULL;
        size_t end;
        str_t line, name, value;
        const char *colon;

        if (nl == NULL) {
            if (len >= max_head) {
                return head_error(out_head, start_len, out_status);
            }
            out_head->fields.len = start_len;
            return HTTP_PARSE_INCOMPLETE;
        }
        end = (size_t)(nl - p);
        if (end + 1 > max_head) {
            return head_error(out_head, start_len, out_status);
        }
        line.ptr = p + pos;
        line.len = end - pos;
        if (line.len > 0 && line.ptr[line.len - 1] == '\r') {
            line.len--;
        }
        pos = end + 1;
        if (line.len == 0) {
            break;
        }
        colon = memchr(line.ptr, ':', line.len);
        if (colon == NULL || memchr(line.ptr, '\r', line.len) != NULL) {
            return head_error(out_head, start_len, out_status);
        }
        name.ptr = line.ptr;
        name.len = (size_t)(colon - line.ptr);
        value.ptr = colon + 1;
        value.len = line.len - name.len - 1;
        value = str_trim_ows(value);
        if (!http_token_valid(name)) {
            return head_error(out_head, start_len, out_status);
        }
        if (str_eq_ci(name, "Status")) {
            status = status == 0 ? parse_status(value) : -1; /* 중복도 오류 */
            if (status < 0) {
                return head_error(out_head, start_len, out_status);
            }
            continue;
        }
        if (str_eq_ci(name, "Content-Length")) {
            /* 유효하지 않으면 없는 것으로 본다 (conn이 CHUNKED·CLOSE로 끝냄) */
            has_length = str_to_u64(value, UINT64_MAX, &length) == 0;
            continue;
        }
        if (str_eq_ci(name, "Location")) {
            location = true;
        }
        if (dropped(name)) {
            continue;
        }
        if (buf_appendf(&out_head->fields, "%.*s: %.*s\r\n", (int)name.len, name.ptr,
                        (int)value.len, value.ptr) != 0) {
            return head_error(out_head, start_len, out_status);
        }
    }
    out_head->status = status != 0 ? status : (location ? 302 : 200);
    if (has_length) {
        out_head->framing.kind = FRAMING_LENGTH;
        out_head->framing.length = length;
    } else {
        out_head->framing.kind = FRAMING_NONE;
    }
    *out_used = pos;
    return HTTP_PARSE_DONE;
}

/* ── 스크립트 시작 ──────────────────────────────────────────────────────── */

static int errno_status(int e)
{
    if (e == ENOENT || e == ENOTDIR) {
        return 404;
    }
    return e == EACCES ? 403 : 500;
}

static bool inside_root(const char *root, const char *real)
{
    size_t n = strlen(root);

    if (n == 1) {
        return real[0] == '/';
    }
    return strncmp(real, root, n) == 0 && (real[n] == '\0' || real[n] == '/');
}

typedef struct {
    char *v[CGI_MAX_ENV];
    size_t n;
    bool failed;
} envp_t;

static void env_add(envp_t *e, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void env_add(envp_t *e, const char *fmt, ...)
{
    va_list ap;
    int n;
    char *s;

    if (e->failed || e->n + 1 >= CGI_MAX_ENV) {
        e->failed = true;
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    s = n < 0 ? NULL : malloc((size_t)n + 1);
    if (s == NULL) {
        e->failed = true;
        return;
    }
    va_start(ap, fmt);
    (void)vsnprintf(s, (size_t)n + 1, fmt, ap);
    va_end(ap);
    e->v[e->n++] = s;
}

static void env_free(envp_t *e)
{
    for (size_t i = 0; i < e->n; i++) {
        free(e->v[i]);
    }
    e->n = 0;
}

/* Host에서 포트를 뗀다 (IPv6 리터럴이면 ']' 뒤의 ':'만) */
static str_t host_without_port(str_t host)
{
    size_t from = 0;

    if (host.len > 0 && host.ptr[0] == '[') {
        const char *rb = memchr(host.ptr, ']', host.len);
        if (rb == NULL) {
            return host;
        }
        from = (size_t)(rb - host.ptr) + 1;
    }
    for (size_t i = from; i < host.len; i++) {
        if (host.ptr[i] == ':') {
            host.len = i;
            break;
        }
    }
    return host;
}

static void env_add_header(envp_t *e, const http_header_t *h)
{
    char name[256];
    size_t n = 0;

    if (str_eq_ci(h->name, "Proxy") || str_eq_ci(h->name, "Content-Length") ||
        str_eq_ci(h->name, "Content-Type")) {
        return;
    }
    if (h->name.len >= sizeof(name)) {
        return;
    }
    for (size_t i = 0; i < h->name.len; i++) {
        char c = h->name.ptr[i];
        name[n++] = c == '-' ? '_' : (char)toupper((unsigned char)c);
    }
    name[n] = '\0';
    env_add(e, "HTTP_%s=%.*s", name, (int)h->value.len, h->value.ptr);
}

static void build_env(envp_t *e, const http_request_t *req, const handler_ctx_t *ctx,
                      const char *script_name, const char *path_info)
{
    str_t server_name = host_without_port(req->host);
    const http_header_t *ct = http_header_get(&req->headers, "Content-Type");

    env_add(e, "GATEWAY_INTERFACE=CGI/1.1");
    env_add(e, "SERVER_SOFTWARE=httpd/0.1");
    env_add(e, "SERVER_PROTOCOL=HTTP/1.%d", req->minor);
    env_add(e, "SERVER_NAME=%.*s", (int)server_name.len, server_name.ptr);
    env_add(e, "SERVER_PORT=%u", (unsigned)ctx->server_port);
    env_add(e, "REQUEST_METHOD=%.*s", (int)req->method_raw.len, req->method_raw.ptr);
    env_add(e, "SCRIPT_NAME=%s", script_name);
    env_add(e, "PATH_INFO=%s", path_info);
    env_add(e, "QUERY_STRING=%.*s", (int)req->query.len, req->query.ptr);
    env_add(e, "REMOTE_ADDR=%.*s", (int)ctx->client_ip.len, ctx->client_ip.ptr);
    if (req->body.len > 0) {
        env_add(e, "CONTENT_LENGTH=%zu", req->body.len);
    }
    if (ct != NULL) {
        env_add(e, "CONTENT_TYPE=%.*s", (int)ct->value.len, ct->value.ptr);
    }
    env_add(e, "PATH=/usr/bin:/bin");
    for (size_t i = 0; i < req->headers.n; i++) {
        env_add_header(e, &req->headers.v[i]);
    }
    e->v[e->n] = NULL;
}

static void close_pair(int fds[2])
{
    for (int i = 0; i < 2; i++) {
        if (fds[i] >= 0) {
            (void)close(fds[i]);
            fds[i] = -1;
        }
    }
}

static int make_pipe(int fds[2])
{
    if (pipe(fds) != 0) {
        fds[0] = fds[1] = -1;
        return -1;
    }
    if (platform_set_cloexec(fds[0]) != 0 || platform_set_cloexec(fds[1]) != 0) {
        close_pair(fds);
        return -1;
    }
    return 0;
}

/* 자식: async-signal-safe 함수만 쓴다 */
static void run_child(int in_r, int out_w, const char *dir, const char *script, char **envp)
{
    char *argv[2];

    argv[0] = (char *)script;
    argv[1] = NULL;
    if (dup2(in_r, STDIN_FILENO) < 0 || dup2(out_w, STDOUT_FILENO) < 0 || chdir(dir) != 0) {
        _exit(127);
    }
    execve(script, argv, envp);
    _exit(127);
}

static handler_result_t fail(response_plan_t *p, int status)
{
    (void)plan_error(p, status);
    return HANDLER_DONE;
}

/* 스크립트를 확인한다 (명세 3단계). 성공하면 0과 real·dir, 실패하면 상태 코드 */
static int check_script(const route_t *rt, const char *seg, char *real, char *dir)
{
    char root[PATH_MAX];
    char path[PATH_MAX];
    struct stat st;
    char *slash;
    int n;

    if (realpath(rt->root, root) == NULL) {
        return 500;
    }
    n = snprintf(path, sizeof(path), "%s/%s", rt->root, seg);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        return 404;
    }
    if (realpath(path, real) == NULL) {
        return errno_status(errno);
    }
    if (!inside_root(root, real)) {
        return 403;
    }
    if (stat(real, &st) != 0) {
        return errno_status(errno);
    }
    if (!S_ISREG(st.st_mode) || access(real, X_OK) != 0) {
        return 403;
    }
    memcpy(dir, real, strlen(real) + 1);
    slash = strrchr(dir, '/');
    slash[slash == dir ? 1 : 0] = '\0';
    return 0;
}

handler_result_t cgi_handle(const http_request_t *req, const route_t *rt, const handler_ctx_t *ctx,
                            response_plan_t *out_plan)
{
    const char *rest = req->path + rt->prefix_len;
    const char *path_info;
    char seg[PATH_MAX];
    char script_name[HTTP_PATH_MAX + PATH_MAX];
    char real[PATH_MAX];
    char dir[PATH_MAX];
    int in_fds[2] = {-1, -1};
    int out_fds[2] = {-1, -1};
    envp_t env = {{NULL}, 0, false};
    buf_t in_data;
    size_t seg_len;
    pid_t pid;
    int n;
    int status;

    if (req->method != HTTP_GET && req->method != HTTP_HEAD && req->method != HTTP_POST) {
        (void)plan_error(out_plan, 405);
        (void)plan_add_header(out_plan, "Allow", "GET, HEAD, POST");
        return HANDLER_DONE;
    }
    while (*rest == '/') {
        rest++;
    }
    path_info = strchr(rest, '/');
    if (path_info == NULL) {
        path_info = rest + strlen(rest);
    }
    seg_len = (size_t)(path_info - rest);
    if (seg_len == 0 || seg_len >= sizeof(seg)) {
        return fail(out_plan, 404);
    }
    memcpy(seg, rest, seg_len);
    seg[seg_len] = '\0';
    status = check_script(rt, seg, real, dir);
    if (status != 0) {
        return fail(out_plan, status);
    }
    n = snprintf(script_name, sizeof(script_name), "%s%s%s", rt->prefix,
                 rt->prefix[rt->prefix_len - 1] == '/' ? "" : "/", seg);
    if (n < 0 || (size_t)n >= sizeof(script_name)) {
        return fail(out_plan, 404);
    }

    build_env(&env, req, ctx, script_name, path_info);
    if (env.failed ||
        buf_init(&in_data, req->body.len, req->body.len > 0 ? req->body.len : 1) != 0) {
        env_free(&env);
        return fail(out_plan, 500);
    }
    if (buf_append(&in_data, req->body.ptr, req->body.len) != 0 || make_pipe(in_fds) != 0 ||
        make_pipe(out_fds) != 0) {
        close_pair(in_fds);
        buf_cleanup(&in_data);
        env_free(&env);
        return fail(out_plan, 500);
    }
    pid = fork();
    if (pid == 0) {
        run_child(in_fds[0], out_fds[1], dir, real, env.v);
    }
    env_free(&env);
    (void)close(in_fds[0]);
    (void)close(out_fds[1]);
    if (pid < 0) {
        (void)close(in_fds[1]);
        (void)close(out_fds[0]);
        buf_cleanup(&in_data);
        return fail(out_plan, 500);
    }
    out_plan->kind = BODY_PIPE;
    out_plan->head_pending = true;
    out_plan->body.pipe.pid = pid;
    out_plan->body.pipe.in_fd = in_fds[1];
    out_plan->body.pipe.out_fd = out_fds[0];
    out_plan->body.pipe.in_data = in_data;
    out_plan->body.pipe.deadline_ms = ctx->now_ms + ctx->cfg->cgi_timeout_ms;
    return HANDLER_DONE;
}
