/* src/handler/cgi.c
 *
 * CGI 시작과 출력 머리 파싱. 명세: handler.h
 */
#include "handler/handler.h"

#include "common/http_framing.h"
#include "common/http_headers.h"
#include "core/platform.h"
#include "handler/plan.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int env_add(char **envp, size_t *count, const char *name, const char *value)
{
    size_t a = strlen(name), b = strlen(value);
    char *item;
    if (a > SIZE_MAX - b - 2) { errno = ENOMEM; return -1; }
    item = malloc(a + b + 2);
    if (item == NULL) return -1;
    memcpy(item, name, a);
    item[a] = '=';
    memcpy(item + a + 1, value, b + 1);
    envp[(*count)++] = item;
    return 0;
}

static void env_free(char **envp, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) free(envp[i]);
}

static int cgi_env(const http_request_t *req, const route_t *rt, const handler_ctx_t *ctx,
                   const char *script_name, const char *path_info, char ***out_env,
                   size_t *out_n)
{
    char **envp = calloc(req->headers.n + 16, sizeof(*envp));
    char port[16], content_length[32], host[CFG_HOST_MAX], method[32];
    size_t n = 0, i, host_len = req->host.len;
    const char *colon;
    if (envp == NULL) return -1;
    if (host_len >= sizeof(host)) host_len = sizeof(host) - 1;
    if (req->host.ptr != NULL) memcpy(host, req->host.ptr, host_len);
    host[host_len] = '\0';
    if (str_to_cstr(req->method_raw, method, sizeof(method)) != 0) (void)snprintf(method, sizeof(method), "GET");
    colon = strrchr(host, ':');
    if (colon != NULL) {
        char *close_bracket = strrchr(host, ']');
        if (close_bracket == NULL || colon > close_bracket) host[(size_t)(colon - host)] = '\0';
    }
    (void)snprintf(port, sizeof(port), "%u", (unsigned)ctx->server_port);
    (void)snprintf(content_length, sizeof(content_length), "%llu", (unsigned long long)req->body.len);
#define ADD(k, v) do { if (env_add(envp, &n, (k), (v)) != 0) goto fail; } while (0)
    ADD("GATEWAY_INTERFACE", "CGI/1.1");
    ADD("SERVER_SOFTWARE", "httpd/0.1");
    ADD("SERVER_PROTOCOL", req->minor == 0 ? "HTTP/1.0" : "HTTP/1.1");
    ADD("SERVER_NAME", host[0] == '\0' ? "localhost" : host);
    ADD("SERVER_PORT", port);
    ADD("REQUEST_METHOD", method);
    ADD("SCRIPT_NAME", script_name);
    ADD("PATH_INFO", path_info);
    if (req->query.len > 0) {
        char *q = malloc(req->query.len + 1);
        if (q == NULL) goto fail;
        memcpy(q, req->query.ptr, req->query.len); q[req->query.len] = '\0';
        if (env_add(envp, &n, "QUERY_STRING", q) != 0) { free(q); goto fail; }
        free(q);
    } else ADD("QUERY_STRING", "");
    if (ctx->client_ip.ptr != NULL && ctx->client_ip.len > 0) {
        char ip[CFG_HOST_MAX];
        size_t ipn = ctx->client_ip.len < sizeof(ip) - 1 ? ctx->client_ip.len : sizeof(ip) - 1;
        memcpy(ip, ctx->client_ip.ptr, ipn); ip[ipn] = '\0';
        ADD("REMOTE_ADDR", ip);
    } else ADD("REMOTE_ADDR", "-");
    if (req->body.len > 0) ADD("CONTENT_LENGTH", content_length);
    {
        const http_header_t *ct = http_header_get(&req->headers, "Content-Type");
        if (ct != NULL) {
            char type[2048];
            size_t len = ct->value.len < sizeof(type) - 1 ? ct->value.len : sizeof(type) - 1;
            memcpy(type, ct->value.ptr, len); type[len] = '\0';
            ADD("CONTENT_TYPE", type);
        }
    }
    ADD("PATH", "/usr/bin:/bin");
    for (i = 0; i < req->headers.n; i++) {
        const http_header_t *h = &req->headers.v[i];
        char name[256], value[4096];
        size_t k;
        if (str_eq_ci(h->name, "Proxy") || str_eq_ci(h->name, "Content-Length") ||
            str_eq_ci(h->name, "Content-Type") || h->name.len + 6 >= sizeof(name) ||
            h->value.len >= sizeof(value)) continue;
        memcpy(name, "HTTP_", 5);
        for (k = 0; k < h->name.len; k++) {
            unsigned char c = (unsigned char)h->name.ptr[k];
            name[5 + k] = c == '-' ? '_' :
                (c >= 'a' && c <= 'z' ? (char)(c - ('a' - 'A')) : (char)c);
        }
        name[5 + h->name.len] = '\0';
        memcpy(value, h->value.ptr, h->value.len); value[h->value.len] = '\0';
        ADD(name, value);
    }
#undef ADD
    envp[n] = NULL;
    *out_env = envp;
    *out_n = n;
    (void)rt;
    return 0;
fail:
#undef ADD
    env_free(envp, n);
    free(envp);
    return -1;
}

handler_result_t cgi_handle(const http_request_t *req, const route_t *rt, const handler_ctx_t *ctx,
                            response_plan_t *out_plan)
{
    char relative[HTTP_PATH_MAX], script_name[HTTP_PATH_MAX], path_info[HTTP_PATH_MAX];
    char candidate[PATH_MAX], canonical_root[PATH_MAX], canonical_script[PATH_MAX];
    char **child_env = NULL;
    size_t env_count = 0, rel_len, first_len;
    const char *suffix;
    struct stat st;
    int to_child[2] = {-1, -1}, from_child[2] = {-1, -1};
    pid_t pid;
    buf_t input = {0};
    if (req == NULL || rt == NULL || ctx == NULL || out_plan == NULL || rt->root == NULL) {
        if (out_plan != NULL) (void)plan_error(out_plan, 500);
        return HANDLER_DONE;
    }
    if (req->method != HTTP_GET && req->method != HTTP_HEAD && req->method != HTTP_POST) {
        (void)plan_error(out_plan, 405);
        (void)plan_add_header(out_plan, "Allow", "GET, HEAD, POST");
        return HANDLER_DONE;
    }
    suffix = req->path;
    if (strncmp(suffix, rt->prefix, rt->prefix_len) == 0) suffix += rt->prefix_len;
    while (*suffix == '/') suffix++;
    rel_len = strlen(suffix);
    first_len = 0;
    while (first_len < rel_len && suffix[first_len] != '/') first_len++;
    if (first_len == 0 || first_len >= sizeof(relative) || rel_len - first_len >= sizeof(path_info)) {
        (void)plan_error(out_plan, 404); return HANDLER_DONE;
    }
    memcpy(relative, suffix, first_len); relative[first_len] = '\0';
    {
    size_t prefix_len = rt->prefix_len;
    int n;

    while (prefix_len > 1 && rt->prefix[prefix_len - 1] == '/') {
        prefix_len--;
    }

    n = snprintf(script_name, sizeof(script_name), "%.*s/%s",
                 (int)prefix_len, rt->prefix, relative);
    if (n < 0 || (size_t)n >= sizeof(script_name)) {
        (void)plan_error(out_plan, 414);
        return HANDLER_DONE;
        }
    }
    (void)snprintf(path_info, sizeof(path_info), "%s", suffix + first_len);
    {
        int n = snprintf(candidate, sizeof(candidate), "%s/%s", rt->root, relative);
        if (n < 0 || (size_t)n >= sizeof(candidate) || realpath(rt->root, canonical_root) == NULL ||
            realpath(candidate, canonical_script) == NULL) {
        (void)plan_error(out_plan, errno == EACCES ? 403 : 404); return HANDLER_DONE;
        }
    }
    if (strncmp(canonical_script, canonical_root, strlen(canonical_root)) != 0 ||
        (canonical_root[strlen(canonical_root) - 1] != '/' &&
         canonical_script[strlen(canonical_root)] != '\0' &&
         canonical_script[strlen(canonical_root)] != '/')) {
        (void)plan_error(out_plan, 403); return HANDLER_DONE;
    }
    if (stat(canonical_script, &st) != 0) {
        (void)plan_error(out_plan, errno == ENOENT ? 404 : 403); return HANDLER_DONE;
    }
    if (!S_ISREG(st.st_mode) || access(canonical_script, X_OK) != 0) {
        (void)plan_error(out_plan, 403); return HANDLER_DONE;
    }
    if (cgi_env(req, rt, ctx, script_name, path_info, &child_env, &env_count) != 0) {
        (void)plan_error(out_plan, 500); return HANDLER_DONE;
    }
    if (pipe(to_child) != 0 || pipe(from_child) != 0 || platform_set_cloexec(to_child[0]) != 0 ||
        platform_set_cloexec(to_child[1]) != 0 || platform_set_cloexec(from_child[0]) != 0 ||
        platform_set_cloexec(from_child[1]) != 0) goto fail;
    pid = fork();
    if (pid < 0) goto fail;
    if (pid == 0) {
        char *args[2];
        char dir[PATH_MAX];
        char *slash;
        (void)close(to_child[1]); (void)close(from_child[0]);
        if (dup2(to_child[0], STDIN_FILENO) < 0 || dup2(from_child[1], STDOUT_FILENO) < 0) _exit(127);
        (void)close(to_child[0]); (void)close(from_child[1]);
        (void)snprintf(dir, sizeof(dir), "%s", canonical_script);
        slash = strrchr(dir, '/');
        if (slash != NULL) {
        if (slash == dir) {
            slash[1] = '\0';
        } else {
            *slash = '\0';
            }

        if (chdir(dir) != 0) {
           _exit(127);
            }
        }
        args[0] = canonical_script; args[1] = NULL;
        execve(canonical_script, args, child_env);
        _exit(127);
    }
    (void)close(to_child[0]); to_child[0] = -1;
    (void)close(from_child[1]); from_child[1] = -1;
    if (buf_init(&input, req->body.len, req->body.len == 0 ? 1 : req->body.len) != 0 ||
        buf_append(&input, req->body.ptr, req->body.len) != 0) {
        (void)kill(pid, SIGKILL); (void)waitpid(pid, NULL, 0); goto fail;
    }
    plan_cleanup(out_plan);
    out_plan->kind = BODY_PIPE;
    out_plan->head_pending = true;
    out_plan->body.pipe.pid = pid;
    out_plan->body.pipe.in_fd = to_child[1]; to_child[1] = -1;
    out_plan->body.pipe.out_fd = from_child[0]; from_child[0] = -1;
    out_plan->body.pipe.in_data = input;
    memset(&input, 0, sizeof(input));
    out_plan->body.pipe.deadline_ms = ctx->now_ms + ctx->cfg->cgi_timeout_ms;
    env_free(child_env, env_count); free(child_env);
    return HANDLER_DONE;
fail:
    {
        int saved = errno;
        if (to_child[0] >= 0) close(to_child[0]);
        if (to_child[1] >= 0) close(to_child[1]);
        if (from_child[0] >= 0) close(from_child[0]);
        if (from_child[1] >= 0) close(from_child[1]);
        if (child_env != NULL) { env_free(child_env, env_count); free(child_env); }
        buf_cleanup(&input);
        errno = saved;
    }
    (void)plan_error(out_plan, 500);
    return HANDLER_DONE;
}

http_parse_result_t cgi_parse_head(const char *p, size_t len, size_t max_head,
                                   http_head_out_t *out_head, size_t *out_used, int *out_status)
{
    size_t skip, head_len, header_len, i;
    int status = 502;
    http_headers_t headers;
    http_framing_t framing = {FRAMING_NONE, 0};
    int response_status = 200;
    const http_header_t *h;
    http_parse_result_t pr;
    if (out_head == NULL || out_used == NULL || out_status == NULL || (p == NULL && len != 0)) {
        if (out_status != NULL) *out_status = 502;
        errno = EINVAL; return HTTP_PARSE_ERROR;
    }
    pr = http_find_head_end(p, len, max_head, &skip, &head_len, &status);
    if (pr == HTTP_PARSE_INCOMPLETE) return pr;
    if (pr != HTTP_PARSE_DONE) { *out_status = 502; return HTTP_PARSE_ERROR; }
    header_len = head_len - skip;
    if (header_len >= 4 && memcmp(p + head_len - 4, "\r\n\r\n", 4) == 0) header_len -= 2;
    else if (header_len >= 2 && memcmp(p + head_len - 2, "\n\n", 2) == 0) header_len -= 1;
    else { *out_status = 502; return HTTP_PARSE_ERROR; }
    if (http_parse_headers(p + skip, header_len, HTTP_MAX_HEADERS, &headers, &status) != 0) {
        *out_status = 502; return HTTP_PARSE_ERROR;
    }
    h = http_header_get(&headers, "Status");
    if (h != NULL) {
        uint64_t code;
        if (h->value.len < 3 || str_to_u64((str_t){h->value.ptr, 3}, 599, &code) != 0 || code < 100) {
            *out_status = 502; return HTTP_PARSE_ERROR;
        }
        if (h->value.len > 3 && h->value.ptr[3] != ' ') { *out_status = 502; return HTTP_PARSE_ERROR; }
        response_status = (int)code;
    } else if (http_header_get(&headers, "Location") != NULL) response_status = 302;
    h = http_header_get(&headers, "Content-Length");
    if (h != NULL) {
        uint64_t n;
        if (str_to_u64(h->value, UINT64_MAX, &n) != 0) { *out_status = 502; return HTTP_PARSE_ERROR; }
        framing = (http_framing_t){FRAMING_LENGTH, n};
    }
    if (out_head->fields.max == 0 && buf_init(&out_head->fields, 256, 64u * 1024u) != 0) {
        *out_status = 502; return HTTP_PARSE_ERROR;
    }
    for (i = 0; i < headers.n; i++) {
        const http_header_t *field = &headers.v[i];
        if (str_eq_ci(field->name, "Status") || str_eq_ci(field->name, "Content-Length") ||
            str_eq_ci(field->name, "Date") || str_eq_ci(field->name, "Server") ||
            http_is_hop_by_hop(field->name, &headers)) continue;
        if (buf_appendf(&out_head->fields, "%.*s: %.*s\r\n", (int)field->name.len,
                        field->name.ptr, (int)field->value.len, field->value.ptr) != 0) {
            *out_status = 502; return HTTP_PARSE_ERROR;
        }
    }
    out_head->status = response_status;
    out_head->framing = framing;
    *out_used = head_len;
    return HTTP_PARSE_DONE;
}
