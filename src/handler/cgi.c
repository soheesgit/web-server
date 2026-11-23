/* src/handler/cgi.c
 *
 * CGI 처리기. 명세: handler.h
 */
#include "handler/handler.h"

#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/http_headers.h"
#include "core/platform.h"
#include "handler/internal_path.h"
#include "handler/plan.h"

/* 요청 헤더별 항목과 최대 13개의 CGI 변수, execve용 마지막 NULL을 담는다. */
typedef struct {
    char *vars[HTTP_MAX_HEADERS + 14];
    size_t count;
} cgi_env_t;

/*
 * pipe 생성과 CLOEXEC 설정이 별도 호출이므로 두 작업 사이에 다른 CGI fork가
 * 끼어들면 새 fd가 그 자식에 상속될 수 있다. 생성부터 fork까지 같은 락으로 보호한다.
 */
static pthread_mutex_t cgi_fork_lock = PTHREAD_MUTEX_INITIALIZER;

static str_t server_name(str_t host)
{
    if (host.len == 0) {
        return host;
    }
    /* SERVER_NAME에는 포트를 빼고 전달한다. IPv6 주소의 내부 콜론은 유지한다. */
    if (host.ptr[0] == '[') {
        const char *end = memchr(host.ptr, ']', host.len);
        if (end != NULL && (size_t)(end - host.ptr + 1) < host.len && end[1] == ':') {
            host.len = (size_t)(end - host.ptr + 1);
        }
    } else {
        const char *port = memchr(host.ptr, ':', host.len);
        if (port != NULL) {
            host.len = (size_t)(port - host.ptr);
        }
    }
    return host;
}

static void env_cleanup(cgi_env_t *env)
{
    for (size_t i = 0; i < env->count; i++) {
        free(env->vars[i]);
    }
}

static int env_add(cgi_env_t *env, str_t name, str_t value, bool header)
{
    /*
     * 요청 버퍼를 빌린 이름과 값을 NUL 종료된 NAME=value 사본으로 만든다.
     * 헤더 이름만 CGI의 HTTP_ 대문자 표기로 변환하고 값은 바이트 그대로 복사한다.
     */
    size_t prefix_len = header ? sizeof("HTTP_") - 1 : 0;
    if (name.len > SIZE_MAX - prefix_len - 2 || value.len > SIZE_MAX - prefix_len - name.len - 2 ||
        env->count >= sizeof(env->vars) / sizeof(env->vars[0]) - 1) {
        return -1;
    }
    char *entry = malloc(prefix_len + name.len + value.len + 2);
    if (entry == NULL) {
        return -1;
    }
    if (header) {
        memcpy(entry, "HTTP_", prefix_len);
        for (size_t i = 0; i < name.len; i++) {
            char c = name.ptr[i];
            if (c >= 'a' && c <= 'z') {
                c = (char)(c - 'a' + 'A');
            } else if (c == '-') {
                c = '_';
            }
            entry[prefix_len + i] = c;
        }
    } else {
        memcpy(entry, name.ptr, name.len);
    }
    size_t pos = prefix_len + name.len;
    entry[pos++] = '=';
    if (value.len != 0) {
        memcpy(entry + pos, value.ptr, value.len);
    }
    entry[pos + value.len] = '\0';
    env->vars[env->count++] = entry;
    return 0;
}

static int build_env(const http_request_t *req, const handler_ctx_t *ctx, str_t script_name,
                     const char *path_info, cgi_env_t *out)
{
    char protocol[32];
    char port[8];
    char length[32];

    (void)snprintf(protocol, sizeof(protocol), "HTTP/1.%d", req->minor);
    (void)snprintf(port, sizeof(port), "%u", (unsigned)ctx->server_port);
    (void)snprintf(length, sizeof(length), "%zu", req->body.len);
    /*
     * 서버 환경에서 변수를 복사하지 않고 execve에 넘길 환경을 요청마다 구성한다.
     * QUERY_STRING은 파싱 전 인코딩을 보존한다.
     */
    const http_header_t values[] = {
        {STR_LIT("GATEWAY_INTERFACE"), STR_LIT("CGI/1.1")},
        {STR_LIT("SERVER_SOFTWARE"), STR_LIT("httpd/0.1")},
        {STR_LIT("SERVER_PROTOCOL"), str_from_cstr(protocol)},
        {STR_LIT("SERVER_NAME"), server_name(req->host)},
        {STR_LIT("SERVER_PORT"), str_from_cstr(port)},
        {STR_LIT("REQUEST_METHOD"), req->method_raw},
        {STR_LIT("SCRIPT_NAME"), script_name},
        {STR_LIT("PATH_INFO"), str_from_cstr(path_info)},
        {STR_LIT("QUERY_STRING"), req->query},
        {STR_LIT("REMOTE_ADDR"), ctx->client_ip},
        {STR_LIT("PATH"), STR_LIT("/usr/bin:/bin")},
    };

    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        if (env_add(out, values[i].name, values[i].value, false) < 0) {
            return -1;
        }
    }
    if (req->body.len != 0 &&
        env_add(out, STR_LIT("CONTENT_LENGTH"), str_from_cstr(length), false) < 0) {
        return -1;
    }
    const http_header_t *type = http_header_get(&req->headers, "Content-Type");
    if (type != NULL && env_add(out, STR_LIT("CONTENT_TYPE"), type->value, false) < 0) {
        return -1;
    }
    /*
     * Content-Type과 Content-Length에는 전용 CGI 변수가 있다.
     * Proxy 요청 헤더는 자식의 HTTP_PROXY 설정을 바꿀 수 있으므로 제외한다.
     */
    for (size_t i = 0; i < req->headers.n; i++) {
        const http_header_t *h = &req->headers.v[i];
        if (str_eq_ci(h->name, "Proxy") || str_eq_ci(h->name, "Content-Type") ||
            str_eq_ci(h->name, "Content-Length")) {
            continue;
        }
        if (env_add(out, h->name, h->value, true) < 0) {
            return -1;
        }
    }
    return 0;
}

static void close_pipe(int ends[2])
{
    for (size_t i = 0; i < 2; i++) {
        if (ends[i] >= 0) {
            (void)close(ends[i]);
        }
    }
}

static int make_pipe(int ends[2])
{
    int opened[2];

    if (pipe(opened) < 0) {
        return -1;
    }
    ends[0] = opened[0];
    ends[1] = opened[1];
    for (size_t i = 0; i < 2; i++) {
        /*
         * 서버의 표준 fd가 닫혀 있으면 pipe가 같은 번호를 받을 수 있다.
         * 그 경우 3 이상으로 옮겨 자식의 dup2와 원본 pipe 정리가 충돌하지 않게 한다.
         */
        if (ends[i] <= STDERR_FILENO) {
            int fd = fcntl(ends[i], F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
            if (fd < 0) {
                return -1;
            }
            (void)close(ends[i]);
            ends[i] = fd;
        } else if (platform_set_cloexec(ends[i]) < 0) {
            return -1;
        }
    }
    return 0;
}

static pid_t start_cgi(char *script, char *const envp[], int in[2], int out[2])
{
    char workdir[PATH_MAX];
    size_t n = (size_t)(strrchr(script, '/') - script);
    pid_t pid = -1;

    if (n == 0) {
        n = 1;
    }
    /* fork 전에 작업 디렉토리를 계산해 자식에서는 준비된 값으로 시스템 호출만 한다. */
    memcpy(workdir, script, n);
    workdir[n] = '\0';
    if (pthread_mutex_lock(&cgi_fork_lock) != 0) {
        return -1;
    }
    if (make_pipe(in) == 0 && make_pipe(out) == 0) {
        pid = fork();
        if (pid == 0) {
            char *const args[] = {script, NULL};

            /* 자식의 stdin은 요청 본문을 읽고 stdout은 CGI 응답을 부모에 돌려준다. */
            if (dup2(in[0], STDIN_FILENO) < 0 || dup2(out[1], STDOUT_FILENO) < 0) {
                _exit(127);
            }
            close_pipe(in);
            close_pipe(out);
            if (chdir(workdir) < 0) {
                _exit(127);
            }
            execve(script, args, envp);
            _exit(127);
        }
    }
    if (pid < 0) {
        close_pipe(in);
        close_pipe(out);
    } else {
        /* 부모는 쓰기용 stdin과 읽기용 stdout만 보유한다. 사용하지 않는 쓰기 끝은 EOF를 막는다. */
        (void)close(in[0]);
        (void)close(out[1]);
    }
    (void)pthread_mutex_unlock(&cgi_fork_lock);
    return pid;
}

static int launch_cgi(char *script, const http_request_t *req, const handler_ctx_t *ctx,
                      str_t script_name, const char *path_info, response_plan_t *plan)
{
    cgi_env_t env = {0};
    buf_t input = {0};
    int in[2] = {-1, -1};
    int out[2] = {-1, -1};
    pid_t pid = -1;

    /*
     * 연결 처리기가 CGI 실행 중에도 수신 버퍼를 관리할 수 있도록 요청 본문을 복사한다.
     * 사본을 준비한 뒤 자식을 실행해 할당 실패 시 자식이나 파이프를 남기지 않는다.
     */
    if (build_env(req, ctx, script_name, path_info, &env) == 0 &&
        (req->body.len == 0 || (buf_init(&input, req->body.len, req->body.len) == 0 &&
                                buf_append(&input, req->body.ptr, req->body.len) == 0))) {
        pid = start_cgi(script, env.vars, in, out);
    }
    env_cleanup(&env);
    if (pid < 0) {
        buf_cleanup(&input);
        return 500;
    }

    /*
     * 성공하면 부모 쪽 파이프, 본문 사본, 자식 회수 책임을 계획에 넘긴다.
     * 연결 처리기는 절대 마감까지 CGI의 stdin과 stdout을 다루고 출력에서 응답 머리를 확정한다.
     */
    int64_t timeout = ctx->cfg->cgi_timeout_ms;
    plan->kind = BODY_PIPE;
    plan->head_pending = true;
    plan->body.pipe.in_fd = in[1];
    plan->body.pipe.out_fd = out[0];
    plan->body.pipe.pid = pid;
    plan->body.pipe.in_data = input;
    plan->body.pipe.deadline_ms =
        ctx->now_ms > INT64_MAX - timeout ? INT64_MAX : ctx->now_ms + timeout;
    return 0;
}

static int serve_cgi(const http_request_t *req, const route_t *rt, const handler_ctx_t *ctx,
                     response_plan_t *plan)
{
    const char *name = req->path + rt->prefix_len;
    char root[PATH_MAX];
    char script[PATH_MAX];
    struct stat st;

    if (*name == '/') {
        name++;
    }
    /*
     * /cgi/echo.sh/a는 루트의 echo.sh를 실행한다. SCRIPT_NAME은 /cgi/echo.sh이고
     * 그 뒤 /a는 PATH_INFO로만 전달되어 디스크 경로를 결정하는 데 쓰이지 않는다.
     */
    const char *path_info = name + strcspn(name, "/");
    str_t segment = {name, (size_t)(path_info - name)};
    if (segment.len == 0) {
        return 404;
    }
    if (realpath(rt->root, root) == NULL) {
        return handler_path_status(errno);
    }
    int status = handler_resolve_path(root, segment, script);
    if (status != 0) {
        return status;
    }
    if (stat(script, &st) < 0) {
        return handler_path_status(errno);
    }
    if (!S_ISREG(st.st_mode)) {
        return 403;
    }
    if (access(script, X_OK) < 0) {
        return handler_path_status(errno);
    }
    str_t script_name = {req->path, (size_t)(path_info - req->path)};
    return launch_cgi(script, req, ctx, script_name, path_info, plan);
}

handler_result_t cgi_handle(const http_request_t *req, const route_t *rt, const handler_ctx_t *ctx,
                            response_plan_t *out_plan)
{
    if (req->method != HTTP_GET && req->method != HTTP_HEAD && req->method != HTTP_POST) {
        if (plan_error(out_plan, 405) == 0 &&
            plan_add_header(out_plan, "Allow", "GET, HEAD, POST") < 0) {
            (void)plan_error(out_plan, 500);
        }
        return HANDLER_DONE;
    }

    int status = serve_cgi(req, rt, ctx, out_plan);
    if (status != 0) {
        (void)plan_error(out_plan, status);
    }
    return HANDLER_DONE;
}

static bool next_cgi_line(const char *p, size_t limit, size_t *pos, str_t *out)
{
    if (*pos == limit) {
        return false;
    }
    const char *start = p + *pos;
    const char *end = memchr(start, '\n', limit - *pos);
    if (end == NULL) {
        return false;
    }
    /* 위치는 LF 다음으로 옮기고, CRLF의 CR은 호출자가 검사할 줄에서 제외한다. */
    *pos = (size_t)(end - p) + 1;
    if (end != start && end[-1] == '\r') {
        end--;
    }
    *out = (str_t){start, (size_t)(end - start)};
    return true;
}

static bool split_cgi_header(str_t line, http_header_t *out)
{
    const char *colon = memchr(line.ptr, ':', line.len);
    if (colon == NULL) {
        return false;
    }
    out->name = (str_t){line.ptr, (size_t)(colon - line.ptr)};
    if (!http_token_valid(out->name)) {
        return false;
    }
    /* HTAB은 헤더 값의 공백으로 허용하되 NUL·CR 등 다른 제어 문자는 거부한다. */
    str_t value = {colon + 1, line.len - out->name.len - 1};
    for (size_t i = 0; i < value.len; i++) {
        unsigned char c = (unsigned char)value.ptr[i];
        if ((c < 0x20 && c != '\t') || c == 0x7f) {
            return false;
        }
    }
    out->value = str_trim_ows(value);
    return true;
}

static bool cgi_status_code(str_t value, int *status)
{
    uint64_t parsed;

    /* 상태는 한 번만 정하며, 코드 뒤에 사유가 붙으면 공백으로 구분되어야 한다(D-033). */
    if (*status != 0 || value.len < 3 || (value.len > 3 && value.ptr[3] != ' ') ||
        str_to_u64((str_t){value.ptr, 3}, 599, &parsed) < 0 || parsed < 100) {
        return false;
    }
    *status = (int)parsed;
    return true;
}

/*
 * 첫 번째 패스에서 모든 줄을 검증하고 상태, 본문 길이, Connection 옵션을 모은다.
 * 뒤쪽 Connection 줄도 앞쪽 헤더의 전달 여부에 영향을 줄 수 있다.
 */
static int collect_cgi_headers(const char *p, size_t head_len, int *status, http_framing_t *framing,
                               buf_t *connection)
{
    size_t pos = 0;
    str_t line;

    while (next_cgi_line(p, head_len, &pos, &line) && line.len != 0) {
        http_header_t h;
        if (!split_cgi_header(line, &h)) {
            return -1;
        }
        if (str_eq_ci(h.name, "Status")) {
            if (!cgi_status_code(h.value, status)) {
                return -1;
            }
        } else if (str_eq_ci(h.name, "Content-Length")) {
            uint64_t length;
            if (str_to_u64(h.value, UINT64_MAX, &length) < 0 ||
                (framing->kind == FRAMING_LENGTH && framing->length != length)) {
                return -1;
            }
            *framing = (http_framing_t){FRAMING_LENGTH, length};
        } else if (str_eq_ci(h.name, "Connection")) {
            if ((connection->max == 0 && buf_init(connection, 0, head_len) < 0) ||
                (connection->len != 0 && buf_append(connection, ",", 1) < 0) ||
                buf_append(connection, h.value.ptr, h.value.len) < 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int write_cgi_headers(const char *p, size_t head_len, const buf_t *connection, int *status,
                             buf_t *out)
{
    /* 모은 Connection 옵션으로 고정 목록과 연결별 제외 항목을 함께 판정한다. */
    http_headers_t options = {
        .v = {{STR_LIT("Connection"), {connection->data, connection->len}}},
        .n = connection->len == 0 ? 0 : 1,
    };
    size_t pos = 0;
    str_t line;

    while (next_cgi_line(p, head_len, &pos, &line) && line.len != 0) {
        http_header_t h;
        if (!split_cgi_header(line, &h)) {
            return -1;
        }
        if (str_eq_ci(h.name, "Status") || str_eq_ci(h.name, "Content-Length") ||
            str_eq_ci(h.name, "Date") || str_eq_ci(h.name, "Server") ||
            http_is_hop_by_hop(h.name, &options)) {
            continue;
        }
        if (buf_appendf(out, "%.*s: %.*s\r\n", (int)h.name.len, h.name.ptr, (int)h.value.len,
                        h.value.len == 0 ? "" : h.value.ptr) < 0) {
            return -1;
        }
        /* 명시적 Status가 없고 전달할 Location이 있으면 기본 상태를 302로 정한다. */
        if (*status == 0 && str_eq_ci(h.name, "Location")) {
            *status = 302;
        }
    }
    return 0;
}

static int parse_cgi_headers(const char *p, size_t head_len, http_head_out_t *head)
{
    int status = 0;
    http_framing_t framing = {FRAMING_NONE, 0};
    buf_t connection = {0};
    buf_t fields = {0};
    size_t cap = head->fields.max == 0 ? SIZE_MAX : head->fields.max;
    int result = -1;

    /*
     * 출력 헤더는 임시 버퍼에 만든다. 파싱과 복사가 모두 성공한 시점에만 호출자의
     * 헤더를 갱신해, 중간에 오류가 났을 때 완성되기 전의 응답이 남지 않게 한다.
     */
    if (collect_cgi_headers(p, head_len, &status, &framing, &connection) == 0 &&
        buf_init(&fields, 0, cap) == 0 &&
        write_cgi_headers(p, head_len, &connection, &status, &fields) == 0) {
        if (head->fields.max == 0) {
            head->fields = fields;
            fields = (buf_t){0};
            result = 0;
        } else {
            result = buf_append(&head->fields, fields.data, fields.len);
        }
    }

    buf_cleanup(&fields);
    buf_cleanup(&connection);
    if (result == 0) {
        head->status = status == 0 ? 200 : status;
        head->framing = framing;
    }
    return result;
}

http_parse_result_t cgi_parse_head(const char *p, size_t len, size_t max_head,
                                   http_head_out_t *out_head, size_t *out_used, int *out_status)
{
    /*
     * 빈 줄까지만 길이 제한에 포함한다. 같은 읽기에서 이미 본문까지 도착했어도
     * 구분자까지의 길이만 검사하고 본문 시작 위치를 out_used로 돌려준다.
     */
    size_t limit = len < max_head ? len : max_head;
    size_t pos = 0;
    str_t line;

    while (next_cgi_line(p, limit, &pos, &line)) {
        if (line.len == 0) {
            if (parse_cgi_headers(p, pos, out_head) == 0) {
                *out_used = pos;
                return HTTP_PARSE_DONE;
            }
            *out_status = 502;
            return HTTP_PARSE_ERROR;
        }
    }
    /* 빈 줄이 없으면 상한까지 여유가 있을 때만 다음 입력을 기다린다. */
    if (len < max_head) {
        return HTTP_PARSE_INCOMPLETE;
    }
    *out_status = 502;
    return HTTP_PARSE_ERROR;
}
