/* tests/unit/contract_min/test_handler.c
 *
 * handler/ 계약 최소셋 (plan·static·cgi·proxy). 기대값의 원래 표: 기획서 10.2~10.6절.
 * ID 목록: src/handler/README.md
 */
#include "unit/testlib.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common/types.h"
#include "core/buf.h"
#include "fakes/fake_handler.h"
#include "handler/handler.h"
#include "handler/plan.h"
#include "upstream/upstream.h"

static const http_limits_t LIM = {8192, 2048, 64, 10485760};

static handler_ctx_t make_ctx(const config_t *cfg)
{
    handler_ctx_t ctx;

    memset(&ctx, 0, sizeof(ctx));
    ctx.cfg = cfg;
    ctx.lim = &LIM;
    ctx.client_ip.ptr = "127.0.0.1";
    ctx.client_ip.len = 9;
    ctx.server_port = 8080;
    ctx.now = 1700000000;
    ctx.now_ms = fake_now_ms();
    return ctx;
}

static int mem_has(const char *p, size_t n, const char *needle)
{
    size_t k = strlen(needle);

    if (p == NULL || k > n) {
        return 0;
    }
    for (size_t i = 0; i + k <= n; i++) {
        if (memcmp(p + i, needle, k) == 0) {
            return 1;
        }
    }
    return 0;
}

static int fields_has(const response_plan_t *p, const char *needle)
{
    return mem_has(p->head.fields.data, p->head.fields.len, needle);
}

static void new_plan(response_plan_t *p)
{
    memset(p, 0, sizeof(*p));
    plan_init(p);
}

static route_t static_route(const char *root)
{
    route_t rt;

    memset(&rt, 0, sizeof(rt));
    rt.kind = ROUTE_STATIC;
    rt.prefix = "/";
    rt.prefix_len = 1;
    rt.root = root;
    return rt;
}

static void t_u_hd_01(void)
{
    response_plan_t p = {0};

    plan_cleanup(NULL);
    plan_cleanup(&p);
    CHECK_EQ_INT(p.kind, BODY_NONE);
    CHECK_EQ_INT(p.head.status, 0);
    CHECK_EQ_INT(p.head.framing.kind, FRAMING_NONE);
    CHECK(p.head.fields.data == NULL);
    CHECK(!p.head.close && !p.head_pending);
    CHECK_EQ_INT(p.body.pipe.out_fd, -1);
    CHECK_EQ_INT(p.body.pipe.in_fd, -1);
    CHECK_EQ_INT(p.body.pipe.pid, 0);
    CHECK_EQ_INT(p.body.pipe.deadline_ms, -1);
}

static void t_u_hd_02(void)
{
    response_plan_t p;
    char body[] = {'a', '\0', 'b'};
    const char header[] = "Content-Type: application/octet-stream\r\n";

    new_plan(&p);
    CHECK_EQ_INT(plan_set_mem(&p, 200, "application/octet-stream", body, sizeof(body)), 0);
    body[0] = 'x';
    CHECK_EQ_MEM(p.body.mem.data.data, p.body.mem.data.len, "a\0b", 3);
    CHECK_EQ_INT(p.head.framing.length, 3);
    CHECK_EQ_INT(plan_add_header(&p, "content-length", "3"), -1);
    CHECK_EQ_INT(errno, EINVAL);
    CHECK_EQ_INT(plan_add_header(&p, "X-Data", "a\r\nUnexpected: b"), -1);
    CHECK_EQ_MEM(p.head.fields.data, p.head.fields.len, header, sizeof(header) - 1);
    CHECK_EQ_INT(plan_set_mem(&p, 200, NULL, p.body.mem.data.data, p.body.mem.data.len), 0);
    CHECK_EQ_MEM(p.body.mem.data.data, p.body.mem.data.len, "a\0b", 3);

    p.head.close = true;
    CHECK_EQ_INT(plan_error(&p, 404), 0);
    CHECK(p.head.close);
    CHECK_EQ_INT(p.kind, BODY_MEM);
    CHECK_EQ_INT(p.head.status, 404);
    CHECK(p.body.mem.data.len > 0);
    CHECK_EQ_INT(p.head.framing.kind, FRAMING_LENGTH);
    CHECK_EQ_INT(p.head.framing.length, p.body.mem.data.len);
    CHECK(mem_has(p.body.mem.data.data, p.body.mem.data.len, "404"));
    CHECK(mem_has(p.body.mem.data.data, p.body.mem.data.len, "<"));
    plan_cleanup(&p);
}

static void t_u_hd_04(void)
{
    const char *dir = test_tmpdir();
    char path[4096];
    response_plan_t p;
    int fd;

    CHECK(dir != NULL);
    CHECK_EQ_INT(test_write_file(dir, "f.txt", "x", 1, 0644), 0);
    CHECK(snprintf(path, sizeof(path), "%s/f.txt", dir) < (int)sizeof(path));
    fd = open(path, O_RDONLY);
    CHECK(fd >= 0);
    new_plan(&p);
    p.kind = BODY_FILE;
    p.body.file.fd = fd;
    p.body.file.off = 0;
    p.body.file.len = 1;
    plan_cleanup(&p);
    errno = 0;
    CHECK(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
    CHECK_EQ_INT(p.kind, BODY_NONE);
    plan_cleanup(&p); /* 두 번째 호출도 안전해야 한다 */
    CHECK_EQ_INT(p.kind, BODY_NONE);
}

static void t_u_hd_05(void)
{
    response_plan_t p;
    int fds[2];

    CHECK_EQ_INT(pipe(fds), 0);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        (void)pause();
        _exit(0);
    }
    plan_init(&p);
    p.kind = BODY_PIPE;
    p.body.pipe.out_fd = fds[0];
    p.body.pipe.in_fd = fds[1];
    p.body.pipe.pid = pid;
    errno = EBUSY;
    plan_cleanup(&p);
    int cleanup_errno = errno;
    pid_t result = waitpid(pid, NULL, WNOHANG);
    int wait_errno = errno;
    if (result == 0) {
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
    }
    CHECK_EQ_INT(cleanup_errno, EBUSY);
    CHECK_EQ_INT(p.kind, BODY_NONE);
    CHECK(fcntl(fds[0], F_GETFD) == -1 && errno == EBADF);
    CHECK(fcntl(fds[1], F_GETFD) == -1 && errno == EBADF);
    CHECK(result == -1 && wait_errno == ECHILD);
}

static void t_u_hd_09(void)
{
    const char *html = "<p>hi</p>\n";
    config_t *cfg = fake_config();
    http_request_t req;
    response_plan_t p;
    route_t rt;
    handler_ctx_t ctx;

    CHECK(cfg != NULL);
    CHECK_EQ_INT(test_write_file(cfg->root, "index.html", html, strlen(html), 0644), 0);
    rt = static_route(cfg->root);
    ctx = make_ctx(cfg);
    fake_request(&req, HTTP_GET, "/index.html", "t");
    new_plan(&p);
    CHECK_EQ_INT(static_handle(&req, &rt, &ctx, &p), HANDLER_DONE);
    CHECK_EQ_INT(p.head.status, 200);
    CHECK_EQ_INT(p.kind, BODY_FILE);
    CHECK(p.body.file.fd >= 0);
    CHECK_EQ_INT(p.body.file.len, strlen(html));
    CHECK_EQ_INT(p.head.framing.kind, FRAMING_LENGTH);
    CHECK_EQ_INT(p.head.framing.length, strlen(html));
    CHECK(fields_has(&p, "Content-Type: text/html; charset=utf-8\r\n"));
    CHECK(fields_has(&p, "Last-Modified: "));
    plan_cleanup(&p);
}

static void t_u_hd_11(void)
{
    config_t *cfg = fake_config();
    http_request_t req;
    response_plan_t p;
    route_t rt;
    handler_ctx_t ctx;

    CHECK(cfg != NULL);
    CHECK_EQ_INT(test_write_file(cfg->root, "index.html", "x", 1, 0644), 0);
    rt = static_route(cfg->root);
    ctx = make_ctx(cfg);
    fake_request(&req, HTTP_POST, "/index.html", "t");
    new_plan(&p);
    CHECK_EQ_INT(static_handle(&req, &rt, &ctx, &p), HANDLER_DONE);
    CHECK_EQ_INT(p.head.status, 405);
    CHECK(fields_has(&p, "Allow: GET, HEAD\r\n"));
    plan_cleanup(&p);
}

static void t_u_hd_14(void)
{
    config_t *cfg = fake_config();
    const char *outside = test_tmpdir();
    char target[4096], link[4096];
    http_request_t req;
    response_plan_t p;
    route_t rt;
    handler_ctx_t ctx;

    CHECK(cfg != NULL && outside != NULL);
    CHECK_EQ_INT(test_write_file(outside, "secret", "s", 1, 0644), 0);
    CHECK(snprintf(target, sizeof(target), "%s/secret", outside) < (int)sizeof(target));
    CHECK(snprintf(link, sizeof(link), "%s/escape", cfg->root) < (int)sizeof(link));
    CHECK_EQ_INT(symlink(target, link), 0);
    rt = static_route(cfg->root);
    ctx = make_ctx(cfg);
    fake_request(&req, HTTP_GET, "/escape", "t");
    new_plan(&p);
    CHECK_EQ_INT(static_handle(&req, &rt, &ctx, &p), HANDLER_DONE);
    CHECK_EQ_INT(p.head.status, 403);
    plan_cleanup(&p);
}

static void t_u_hd_21(void)
{
    const char *script = "#!/bin/sh\nprintf 'Content-Type: text/plain\\n\\n'\nenv\n";
    const char *cgidir = test_tmpdir();
    config_t *cfg = fake_config();
    http_request_t req;
    response_plan_t p;
    route_t rt;
    handler_ctx_t ctx;
    static char out[65536];
    size_t n = 0;

    CHECK(cgidir != NULL && cfg != NULL);
    CHECK_EQ_INT(test_write_file(cgidir, "echo.sh", script, strlen(script), 0755), 0);
    memset(&rt, 0, sizeof(rt));
    rt.kind = ROUTE_CGI;
    rt.prefix = "/cgi-bin/";
    rt.prefix_len = 9;
    rt.root = cgidir;
    ctx = make_ctx(cfg);
    fake_request(&req, HTTP_GET, "/cgi-bin/echo.sh", "t");
    req.query.ptr = "a=1";
    req.query.len = 3;
    new_plan(&p);
    CHECK_EQ_INT(cgi_handle(&req, &rt, &ctx, &p), HANDLER_DONE);
    CHECK_EQ_INT(p.kind, BODY_PIPE);
    CHECK(p.head_pending == true);
    CHECK(p.body.pipe.pid > 0);
    CHECK(p.body.pipe.out_fd >= 0);
    if (p.body.pipe.in_fd >= 0) {
        close(p.body.pipe.in_fd); /* GET: 쓸 본문 없음 */
        p.body.pipe.in_fd = -1;
    }
    for (;;) {
        ssize_t r = read(p.body.pipe.out_fd, out + n, sizeof(out) - 1 - n);
        if (r <= 0) {
            break;
        }
        n += (size_t)r;
    }
    out[n] = '\0';
    CHECK(strstr(out, "REQUEST_METHOD=GET") != NULL);
    CHECK(strstr(out, "QUERY_STRING=a=1") != NULL);
    plan_cleanup(&p);
}

static void t_u_hd_26(void)
{
    const char *p = "Content-Type: text/plain\n\nbody";
    http_head_out_t h;
    size_t used = 0;
    int st = 0;

    memset(&h, 0, sizeof(h));
    CHECK_EQ_INT(cgi_parse_head(p, strlen(p), 8192, &h, &used, &st), HTTP_PARSE_DONE);
    CHECK_EQ_INT(used, 26);
    CHECK_EQ_INT(h.status, 200);
    CHECK(mem_has(h.fields.data, h.fields.len, "Content-Type: text/plain\r\n"));
    buf_cleanup(&h.fields);
}

/* ── proxy ─────────────────────────────────────────────────────────────── */

typedef struct {
    config_t *cfg;
    upstream_registry_t *reg;
    route_t rt;
    handler_ctx_t ctx;
} px_fixture_t;

static int px_setup(px_fixture_t *fx)
{
    char err[256];
    upstream_conf_t *u;

    memset(fx, 0, sizeof(*fx));
    fx->cfg = fake_config();
    if (fx->cfg == NULL) {
        return -1;
    }
    u = &fx->cfg->upstreams[fx->cfg->n_upstreams++];
    (void)snprintf(u->name, sizeof(u->name), "%s", "g");
    for (int i = 0; i < 3; i++) {
        (void)snprintf(u->backends[i].host, sizeof(u->backends[i].host), "%s", "127.0.0.1");
        u->backends[i].port = (uint16_t)(18001 + i);
    }
    u->n_backends = 3;
    fx->reg = upstream_registry_new(fx->cfg, err, sizeof(err));
    if (fx->reg == NULL) {
        return -1;
    }
    memset(&fx->rt, 0, sizeof(fx->rt));
    fx->rt.kind = ROUTE_PROXY;
    fx->rt.prefix = "/";
    fx->rt.prefix_len = 1;
    fx->rt.upstream = upstream_registry_get(fx->reg, "g");
    fx->ctx = make_ctx(fx->cfg);
    return fx->rt.upstream == NULL ? -1 : 0;
}

static int begin(px_fixture_t *fx, http_request_t *req, proxy_job_t *job, response_plan_t *p)
{
    memset(job, 0, sizeof(*job));
    new_plan(p);
    return proxy_begin(req, &fx->rt, &fx->ctx, job, p) == HANDLER_NEED_UPSTREAM ? 0 : -1;
}

static void t_u_px_01(void)
{
    px_fixture_t fx;
    http_request_t req;
    proxy_job_t job;
    response_plan_t p;
    const char *line = "GET /x HTTP/1.1\r\n";

    CHECK_EQ_INT(px_setup(&fx), 0);
    fake_request(&req, HTTP_GET, "/x", "a.com");
    CHECK_EQ_INT(begin(&fx, &req, &job, &p), 0);
    CHECK(job.backend != NULL);
    CHECK(job.request.len > strlen(line));
    CHECK_EQ_MEM(job.request.data, strlen(line), line, strlen(line));
    proxy_job_cleanup(&job);
    plan_cleanup(&p);
    upstream_registry_free(fx.reg);
}

static void t_u_px_05(void)
{
    px_fixture_t fx;
    http_request_t req;
    proxy_job_t job;
    response_plan_t p;
    struct backend *b0;
    int a0;

    CHECK_EQ_INT(px_setup(&fx), 0);
    fake_request(&req, HTTP_GET, "/x", "a.com");
    CHECK_EQ_INT(begin(&fx, &req, &job, &p), 0);
    b0 = job.backend;
    a0 = job.attempts;
    CHECK(a0 < job.max_attempts);
    CHECK_EQ_INT(proxy_on_fail(&job, PFAIL_CONNECT, &p), PROXY_RETRY);
    CHECK(job.backend != NULL);
    CHECK(job.backend != b0);
    CHECK_EQ_INT(job.attempts, a0 + 1);
    proxy_job_cleanup(&job);
    plan_cleanup(&p);
    upstream_registry_free(fx.reg);
}

static void t_u_px_07(void)
{
    px_fixture_t fx;
    http_request_t req;
    proxy_job_t job;
    response_plan_t p;

    CHECK_EQ_INT(px_setup(&fx), 0);

    fake_request(&req, HTTP_GET, "/x", "a.com");
    CHECK_EQ_INT(begin(&fx, &req, &job, &p), 0);
    job.sent_any = true;
    CHECK_EQ_INT(proxy_on_fail(&job, PFAIL_SEND, &p), PROXY_RETRY);
    proxy_job_cleanup(&job);
    plan_cleanup(&p);

    fake_request(&req, HTTP_POST, "/x", "a.com");
    fake_request_add_header(&req, "Content-Length", "3");
    req.framing.kind = FRAMING_LENGTH;
    req.framing.length = 3;
    req.body.ptr = "abc";
    req.body.len = 3;
    CHECK_EQ_INT(begin(&fx, &req, &job, &p), 0);
    job.sent_any = true;
    CHECK_EQ_INT(proxy_on_fail(&job, PFAIL_SEND, &p), PROXY_GIVE_UP);
    CHECK_EQ_INT(p.head.status, 502);
    proxy_job_cleanup(&job);
    plan_cleanup(&p);
    upstream_registry_free(fx.reg);
}

static void t_u_px_10(void)
{
    px_fixture_t fx;
    http_request_t req;
    proxy_job_t job;
    response_plan_t p;
    upstream_conn_t uc;
    http_response_t resp;

    CHECK_EQ_INT(px_setup(&fx), 0);
    fake_request(&req, HTTP_GET, "/x", "a.com");
    CHECK_EQ_INT(begin(&fx, &req, &job, &p), 0);

    memset(&uc, 0, sizeof(uc));
    uc.fd = -1;
    uc.backend = job.backend;
    memset(&resp, 0, sizeof(resp));
    resp.status = 200;
    resp.reason.ptr = "OK";
    resp.reason.len = 2;
    resp.minor = 1;
    resp.headers.v[0].name.ptr = "Content-Length";
    resp.headers.v[0].name.len = 14;
    resp.headers.v[0].value.ptr = "100";
    resp.headers.v[0].value.len = 3;
    resp.headers.v[1].name.ptr = "Keep-Alive";
    resp.headers.v[1].name.len = 10;
    resp.headers.v[1].value.ptr = "timeout=5";
    resp.headers.v[1].value.len = 9;
    resp.headers.v[2].name.ptr = "X-App";
    resp.headers.v[2].name.len = 5;
    resp.headers.v[2].value.ptr = "1";
    resp.headers.v[2].value.len = 1;
    resp.headers.n = 3;
    resp.framing.kind = FRAMING_LENGTH;
    resp.framing.length = 100;
    resp.keep_alive = true;

    CHECK_EQ_INT(proxy_on_head(&job, &uc, &resp, &p), 0);
    CHECK_EQ_INT(p.kind, BODY_UPSTREAM);
    CHECK(p.body.upstream.conn == &uc);
    CHECK_EQ_INT(p.head.status, 200);
    CHECK_EQ_INT(p.head.framing.kind, FRAMING_LENGTH);
    CHECK_EQ_INT(p.head.framing.length, 100);
    CHECK(fields_has(&p, "X-App: 1\r\n"));
    CHECK(!fields_has(&p, "Keep-Alive"));
    CHECK(!fields_has(&p, "Content-Length"));
    p.body.upstream.conn = NULL; /* 스택의 가짜 연결을 release하지 않도록 */
    proxy_job_cleanup(&job);
    plan_cleanup(&p);
    upstream_registry_free(fx.reg);
}

void register_handler_tests(void)
{
    test_register("U-HD-01", "plan_init: empty plan and fd sentinels", t_u_hd_01);
    test_register("U-HD-02", "plan_error(404) builds an HTML BODY_MEM plan", t_u_hd_02);
    test_register("U-HD-04", "plan_cleanup closes the file fd, twice is safe", t_u_hd_04);
    test_register("U-HD-05", "plan_cleanup closes CGI pipes and reaps child", t_u_hd_05);
    test_register("U-HD-09", "static_handle: GET /index.html", t_u_hd_09);
    test_register("U-HD-11", "static_handle: POST is 405 with Allow", t_u_hd_11);
    test_register("U-HD-14", "static_handle: symlink escaping root is 403", t_u_hd_14);
    test_register("U-HD-21", "cgi_handle: GET with query starts the script", t_u_hd_21);
    test_register("U-HD-26", "cgi_parse_head: LF-only head", t_u_hd_26);
    test_register("U-PX-01", "proxy_begin: origin-form upstream request", t_u_px_01);
    test_register("U-PX-05", "proxy_on_fail: CONNECT retries another backend", t_u_px_05);
    test_register("U-PX-07", "proxy_on_fail: SEND after bytes, GET retry / POST 502", t_u_px_07);
    test_register("U-PX-10", "proxy_on_head: end-to-end headers, LENGTH 100", t_u_px_10);
}
