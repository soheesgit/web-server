/* src/core/config.c
 *
 * 기본값·설정 파일·CLI → config_t. 명세: config.h (문법은 머리 주석)
 */
#include "core/config.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/str.h"

#define CFG_FILE_MAX (1024 * 1024) /* 설정 파일 크기 상한 */
#define CFG_MAX_TOKENS 24 /* 한 줄의 토큰 상한 (upstream 이름 + 백엔드 16개 여유) */

void config_defaults(config_t *out_cfg)
{
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    int workers = 4;

    if (ncpu >= 1) {
        workers = ncpu > 512 ? 1024 : (int)ncpu * 2;
    }

    memset(out_cfg, 0, sizeof(*out_cfg));
    (void)snprintf(out_cfg->listen_host, sizeof(out_cfg->listen_host), "%s", "0.0.0.0");
    out_cfg->port = 8080;
    out_cfg->backlog = 128;
    (void)snprintf(out_cfg->root, sizeof(out_cfg->root), "%s", "./www");
    out_cfg->engine = CFG_ENGINE_POOL;
    out_cfg->workers = workers;
    out_cfg->queue_len = 1024;
    out_cfg->max_head = 8192;
    out_cfg->max_uri = 2048;
    out_cfg->max_headers = 64;
    out_cfg->max_body = 10u * 1024u * 1024u;
    out_cfg->header_timeout_ms = 10000;
    out_cfg->body_timeout_ms = 30000;
    out_cfg->keepalive_timeout_ms = 5000;
    out_cfg->max_keepalive_requests = 100;
    out_cfg->cgi_timeout_ms = 10000;
    out_cfg->cgi_max_head = 8192;
    out_cfg->upstream_connect_timeout_ms = 2000;
    out_cfg->upstream_response_timeout_ms = 15000;
    out_cfg->backend_fail_threshold = 3;
    out_cfg->backend_cooldown_ms = 10000;
    out_cfg->pool_max_idle = 8;
    out_cfg->shutdown_grace_ms = 5000;
    out_cfg->access_log[0] = '\0';
    out_cfg->error_log[0] = '\0';
    out_cfg->n_upstreams = 0;
    out_cfg->n_routes = 0;
}

/* 파싱 상태. path가 NULL이면 CLI·검증 단계의 오류다. */
typedef struct {
    config_t *cfg;
    const char *path;                  /* 설정 파일 경로 (메시지용) */
    size_t line;                       /* 현재 줄 번호 (1부터) */
    size_t route_line[CFG_MAX_ROUTES]; /* route마다 정의된 줄 (검증 메시지용) */
    char *err;
    size_t errcap;
} cfg_ctx_t;

/* err에 메시지를 쓰고 errno를 e로 둔다. 파일 단계면 "<path>:<line>: "를 앞에 붙인다.
 * line이 0이면 줄 번호 없이 "<path>: "를 붙인다. 항상 -1. */
static int cfg_fail_at(cfg_ctx_t *c, size_t line, int e, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static int cfg_fail_at(cfg_ctx_t *c, size_t line, int e, const char *fmt, ...)
{
    va_list ap;
    size_t len = 0;
    int n = 0;

    if (c->err != NULL && c->errcap > 0) {
        c->err[0] = '\0';
        if (c->path != NULL && line > 0) {
            n = snprintf(c->err, c->errcap, "%s:%zu: ", c->path, line);
        } else if (c->path != NULL) {
            n = snprintf(c->err, c->errcap, "%s: ", c->path);
        }
        len = n < 0 ? 0 : (size_t)n;
        if (len < c->errcap) {
            va_start(ap, fmt);
            (void)vsnprintf(c->err + len, c->errcap - len, fmt, ap);
            va_end(ap);
        }
    }
    errno = e;
    return -1;
}

/* str_t를 "%.*s"로 쓰기 위한 길이 (int 범위로 자름) */
static int pf_len(str_t s)
{
    return s.len > INT_MAX ? INT_MAX : (int)s.len;
}

/* 1~65535 포트 */
static int parse_port(str_t s, uint16_t *out)
{
    uint64_t v;

    if (str_to_u64(s, 65535, &v) != 0 || v == 0) {
        return -1;
    }
    *out = (uint16_t)v;
    return 0;
}

/* "host:port" 를 마지막 ':'에서 나눈다. host가 비거나 길면 -1. */
static int parse_hostport(str_t s, char *host, size_t hostcap, uint16_t *port)
{
    size_t k = s.len;

    while (k > 0 && s.ptr[k - 1] != ':') {
        k--;
    }
    if (k <= 1) {
        return -1;
    }
    str_t h = {s.ptr, k - 1};
    str_t p = {s.ptr + k, s.len - k};
    if (parse_port(p, port) != 0 || str_to_cstr(h, host, hostcap) != 0) {
        return -1;
    }
    return 0;
}

/* upstream 이름: 1*(ALPHA / DIGIT / "_" / "-"), CFG_NAME_MAX-1자 이하 */
static bool valid_name(str_t s)
{
    if (s.len == 0 || s.len >= CFG_NAME_MAX) {
        return false;
    }
    for (size_t i = 0; i < s.len; i++) {
        char ch = s.ptr[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
              ch == '_' || ch == '-')) {
            return false;
        }
    }
    return true;
}

/* 1 이상 max 이하 정수 */
static int parse_uint(str_t s, uint64_t max, uint64_t *out)
{
    uint64_t v;

    if (str_to_u64(s, max, &v) != 0 || v == 0) {
        return -1;
    }
    *out = v;
    return 0;
}

/* knob 지시어 표. 필드 이름과 지시어 이름이 같다. */
typedef enum { KNOB_INT, KNOB_SIZE, KNOB_U64 } knob_type_t;
typedef struct {
    const char *name;
    knob_type_t type;
    size_t off;
} knob_t;

#define KNOB(field, type) {#field, type, offsetof(config_t, field)}
static const knob_t knobs[] = {
    KNOB(backlog, KNOB_INT),
    KNOB(queue_len, KNOB_INT),
    KNOB(max_head, KNOB_SIZE),
    KNOB(max_uri, KNOB_SIZE),
    KNOB(max_headers, KNOB_SIZE),
    KNOB(max_body, KNOB_U64),
    KNOB(header_timeout_ms, KNOB_INT),
    KNOB(body_timeout_ms, KNOB_INT),
    KNOB(keepalive_timeout_ms, KNOB_INT),
    KNOB(max_keepalive_requests, KNOB_INT),
    KNOB(cgi_timeout_ms, KNOB_INT),
    KNOB(cgi_max_head, KNOB_SIZE),
    KNOB(upstream_connect_timeout_ms, KNOB_INT),
    KNOB(upstream_response_timeout_ms, KNOB_INT),
    KNOB(backend_fail_threshold, KNOB_INT),
    KNOB(backend_cooldown_ms, KNOB_INT),
    KNOB(pool_max_idle, KNOB_INT),
    KNOB(shutdown_grace_ms, KNOB_INT),
};
#undef KNOB

static int apply_knob(cfg_ctx_t *c, const knob_t *k, str_t val)
{
    char *base = (char *)c->cfg;
    uint64_t max = k->type == KNOB_INT    ? (uint64_t)INT_MAX
                   : k->type == KNOB_SIZE ? (uint64_t)SIZE_MAX
                                          : UINT64_MAX;
    uint64_t v;

    if (parse_uint(val, max, &v) != 0) {
        return cfg_fail_at(c, c->line, EINVAL, "%s: invalid value '%.*s'", k->name, pf_len(val),
                           val.ptr);
    }
    switch (k->type) {
    case KNOB_INT: {
        int iv = (int)v;
        memcpy(base + k->off, &iv, sizeof(iv));
        break;
    }
    case KNOB_SIZE: {
        size_t sv = (size_t)v;
        memcpy(base + k->off, &sv, sizeof(sv));
        break;
    }
    case KNOB_U64:
        memcpy(base + k->off, &v, sizeof(v));
        break;
    }
    return 0;
}

static const knob_t *find_knob(str_t d)
{
    for (size_t i = 0; i < sizeof(knobs) / sizeof(knobs[0]); i++) {
        if (str_eq(d, knobs[i].name)) {
            return &knobs[i];
        }
    }
    return NULL;
}

/* 값 하나를 받는 지시어인가 (knob 포함) */
static bool is_single_directive(str_t d)
{
    return str_eq(d, "listen") || str_eq(d, "root") || str_eq(d, "engine") ||
           str_eq(d, "workers") || str_eq(d, "access_log") || str_eq(d, "error_log") ||
           find_knob(d) != NULL;
}

/* route 지시어 (tok[0]은 "route"). host가 NULL이면 모든 Host. */
static int parse_route(cfg_ctx_t *c, const str_t *tok, size_t n, str_t host)
{
    route_conf_t *r;
    size_t i = 3;

    if (n < 4) {
        return cfg_fail_at(c, c->line, EINVAL, "route: expected 'route <prefix> <kind> ...'");
    }
    if (c->cfg->n_routes >= CFG_MAX_ROUTES) {
        return cfg_fail_at(c, c->line, EINVAL, "too many routes (max %d)", CFG_MAX_ROUTES);
    }
    r = &c->cfg->routes[c->cfg->n_routes];
    memset(r, 0, sizeof(*r));
    if (tok[1].ptr[0] != '/' || str_to_cstr(tok[1], r->prefix, sizeof(r->prefix)) != 0) {
        return cfg_fail_at(c, c->line, EINVAL, "route: invalid prefix '%.*s'", pf_len(tok[1]),
                           tok[1].ptr);
    }
    if (host.len > 0 && str_to_cstr(host, r->host, sizeof(r->host)) != 0) {
        return cfg_fail_at(c, c->line, EINVAL, "host: name too long");
    }
    if (str_eq(tok[2], "static") || str_eq(tok[2], "cgi")) {
        r->kind = str_eq(tok[2], "static") ? ROUTE_STATIC : ROUTE_CGI;
        if (str_to_cstr(tok[3], r->root, sizeof(r->root)) != 0) {
            return cfg_fail_at(c, c->line, EINVAL, "route: path too long");
        }
        i = 4;
    } else if (str_eq(tok[2], "proxy")) {
        r->kind = ROUTE_PROXY;
        if (!valid_name(tok[3])) {
            return cfg_fail_at(c, c->line, EINVAL, "route: invalid upstream name '%.*s'",
                               pf_len(tok[3]), tok[3].ptr);
        }
        (void)str_to_cstr(tok[3], r->upstream, sizeof(r->upstream));
        i = 4;
        if (i < n && str_eq(tok[i], "strip")) {
            r->strip_prefix = true;
            i++;
        }
        if (i < n && str_eq(tok[i], "keep_host")) {
            r->keep_host = true;
            i++;
        }
    } else {
        return cfg_fail_at(c, c->line, EINVAL, "route: unknown kind '%.*s'", pf_len(tok[2]),
                           tok[2].ptr);
    }
    if (i < n) {
        return cfg_fail_at(c, c->line, EINVAL, "route: unexpected '%.*s'", pf_len(tok[i]),
                           tok[i].ptr);
    }
    c->route_line[c->cfg->n_routes] = c->line;
    c->cfg->n_routes++;
    return 0;
}

static int parse_upstream(cfg_ctx_t *c, const str_t *tok, size_t n)
{
    upstream_conf_t *u;

    if (n < 3) {
        return cfg_fail_at(c, c->line, EINVAL, "upstream: expected 'upstream <name> host:port...'");
    }
    if (!valid_name(tok[1])) {
        return cfg_fail_at(c, c->line, EINVAL, "upstream: invalid name '%.*s'", pf_len(tok[1]),
                           tok[1].ptr);
    }
    for (size_t i = 0; i < c->cfg->n_upstreams; i++) {
        if (str_eq(tok[1], c->cfg->upstreams[i].name)) {
            return cfg_fail_at(c, c->line, EINVAL, "upstream: duplicate name '%.*s'",
                               pf_len(tok[1]), tok[1].ptr);
        }
    }
    if (c->cfg->n_upstreams >= CFG_MAX_UPSTREAMS) {
        return cfg_fail_at(c, c->line, EINVAL, "too many upstreams (max %d)", CFG_MAX_UPSTREAMS);
    }
    if (n - 2 > CFG_MAX_BACKENDS) {
        return cfg_fail_at(c, c->line, EINVAL, "upstream: too many backends (max %d)",
                           CFG_MAX_BACKENDS);
    }
    u = &c->cfg->upstreams[c->cfg->n_upstreams];
    memset(u, 0, sizeof(*u));
    (void)str_to_cstr(tok[1], u->name, sizeof(u->name));
    for (size_t i = 2; i < n; i++) {
        backend_conf_t *b = &u->backends[u->n_backends];
        if (parse_hostport(tok[i], b->host, sizeof(b->host), &b->port) != 0) {
            return cfg_fail_at(c, c->line, EINVAL, "upstream: invalid backend '%.*s'",
                               pf_len(tok[i]), tok[i].ptr);
        }
        u->n_backends++;
    }
    c->cfg->n_upstreams++;
    return 0;
}

/* 한 줄의 토큰들을 지시어로 적용한다. n ≥ 1. */
static int apply_directive(cfg_ctx_t *c, const str_t *tok, size_t n)
{
    config_t *cfg = c->cfg;
    str_t d = tok[0];
    str_t no_host = {NULL, 0};

    if (str_eq(d, "upstream")) {
        return parse_upstream(c, tok, n);
    }
    if (str_eq(d, "route")) {
        return parse_route(c, tok, n, no_host);
    }
    if (str_eq(d, "host")) {
        if (n < 3 || !str_eq(tok[2], "route")) {
            return cfg_fail_at(c, c->line, EINVAL, "host: expected 'host <name> route ...'");
        }
        return parse_route(c, tok + 2, n - 2, tok[1]);
    }
    /* 이하 단일 값 지시어. 이름을 먼저 확인해 오타는 "unknown directive"로 보고한다 */
    if (!is_single_directive(d)) {
        return cfg_fail_at(c, c->line, EINVAL, "unknown directive '%.*s'", pf_len(d), d.ptr);
    }
    if (n != 2) {
        return cfg_fail_at(c, c->line, EINVAL, "%.*s: expected exactly one value", pf_len(d),
                           d.ptr);
    }
    if (str_eq(d, "listen")) {
        bool has_colon = memchr(tok[1].ptr, ':', tok[1].len) != NULL;
        if (has_colon ? parse_hostport(tok[1], cfg->listen_host, sizeof(cfg->listen_host),
                                       &cfg->port) != 0
                      : parse_port(tok[1], &cfg->port) != 0) {
            return cfg_fail_at(c, c->line, EINVAL, "listen: invalid address '%.*s'", pf_len(tok[1]),
                               tok[1].ptr);
        }
        return 0;
    }
    if (str_eq(d, "root") || str_eq(d, "access_log") || str_eq(d, "error_log")) {
        char *dst = str_eq(d, "root")         ? cfg->root
                    : str_eq(d, "access_log") ? cfg->access_log
                                              : cfg->error_log;
        if (str_to_cstr(tok[1], dst, CFG_PATH_MAX) != 0) {
            return cfg_fail_at(c, c->line, EINVAL, "%.*s: path too long", pf_len(d), d.ptr);
        }
        return 0;
    }
    if (str_eq(d, "engine")) {
        if (str_eq(tok[1], "pool")) {
            cfg->engine = CFG_ENGINE_POOL;
        } else if (str_eq(tok[1], "loop")) {
            cfg->engine = CFG_ENGINE_LOOP;
        } else {
            return cfg_fail_at(c, c->line, EINVAL, "engine: expected pool or loop, got '%.*s'",
                               pf_len(tok[1]), tok[1].ptr);
        }
        return 0;
    }
    if (str_eq(d, "workers")) {
        uint64_t v;
        if (parse_uint(tok[1], 1024, &v) != 0) {
            return cfg_fail_at(c, c->line, EINVAL, "workers: invalid value '%.*s' (1-1024)",
                               pf_len(tok[1]), tok[1].ptr);
        }
        cfg->workers = (int)v;
        return 0;
    }
    return apply_knob(c, find_knob(d), tok[1]);
}

/* 한 줄(LF 제외)을 토큰으로 나눠 적용한다. */
static int parse_line(cfg_ctx_t *c, const char *p, size_t len)
{
    str_t tok[CFG_MAX_TOKENS];
    size_t n = 0;
    size_t i = 0;

    if (len > 0 && p[len - 1] == '\r') {
        len--;
    }
    while (i < len) {
        size_t start;

        while (i < len && (p[i] == ' ' || p[i] == '\t')) {
            i++;
        }
        if (i == len || p[i] == '#') {
            break;
        }
        start = i;
        while (i < len && p[i] != ' ' && p[i] != '\t') {
            if (p[i] == '\0' || p[i] == '\r') {
                return cfg_fail_at(c, c->line, EINVAL, "invalid control byte");
            }
            i++;
        }
        if (n == CFG_MAX_TOKENS) {
            return cfg_fail_at(c, c->line, EINVAL, "too many tokens");
        }
        tok[n].ptr = p + start;
        tok[n].len = i - start;
        n++;
    }
    return n == 0 ? 0 : apply_directive(c, tok, n);
}

/* 파일 전체를 읽어 *out에 malloc 버퍼로 돌려준다. */
static int read_file(cfg_ctx_t *c, char **out, size_t *out_len)
{
    struct stat st;
    char *data = NULL;
    size_t len = 0;
    int fd;
    int e;

    fd = open(c->path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        e = errno;
        return cfg_fail_at(c, 0, e, "cannot open: %s",
                           e == ENOENT   ? "no such file"
                           : e == EACCES ? "permission denied"
                                         : "open failed");
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        (void)close(fd);
        return cfg_fail_at(c, 0, EINVAL, "not a regular file");
    }
    if (st.st_size > CFG_FILE_MAX) {
        (void)close(fd);
        return cfg_fail_at(c, 0, EFBIG, "file too large (max %d bytes)", CFG_FILE_MAX);
    }
    data = malloc((size_t)st.st_size + 1);
    if (data == NULL) {
        (void)close(fd);
        return cfg_fail_at(c, 0, ENOMEM, "out of memory");
    }
    while (len < (size_t)st.st_size) {
        ssize_t r = read(fd, data + len, (size_t)st.st_size - len);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            e = r < 0 ? errno : EIO;
            free(data);
            (void)close(fd);
            return cfg_fail_at(c, 0, e, "read failed");
        }
        len += (size_t)r;
    }
    (void)close(fd);
    *out = data;
    *out_len = len;
    return 0;
}

static int parse_file(cfg_ctx_t *c)
{
    char *data = NULL;
    size_t len = 0;
    size_t pos = 0;
    int rc = 0;

    if (read_file(c, &data, &len) != 0) {
        return -1;
    }
    c->line = 0;
    while (pos < len && rc == 0) {
        const char *nl = memchr(data + pos, '\n', len - pos);
        size_t llen = nl != NULL ? (size_t)(nl - (data + pos)) : len - pos;

        c->line++;
        rc = parse_line(c, data + pos, llen);
        pos += llen + 1;
    }
    free(data);
    return rc;
}

/* 존재하는 디렉토리인가. 아니면 errno(ENOENT·ENOTDIR 등)를 두고 false. */
static bool is_dir(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0) {
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return false;
    }
    return true;
}

static bool upstream_defined(const config_t *cfg, const char *name)
{
    for (size_t i = 0; i < cfg->n_upstreams; i++) {
        if (strcmp(cfg->upstreams[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

static int validate(cfg_ctx_t *c, const char *file_path)
{
    config_t *cfg = c->cfg;

    c->path = NULL;
    if (!is_dir(cfg->root)) {
        return cfg_fail_at(c, 0, errno, "root: not a directory: %s", cfg->root);
    }
    c->path = file_path; /* route는 파일에서만 나온다 */
    for (size_t i = 0; i < cfg->n_routes; i++) {
        const route_conf_t *r = &cfg->routes[i];

        if (r->kind == ROUTE_PROXY) {
            if (!upstream_defined(cfg, r->upstream)) {
                return cfg_fail_at(c, c->route_line[i], EINVAL, "route %s: undefined upstream '%s'",
                                   r->prefix, r->upstream);
            }
        } else if (!is_dir(r->root)) {
            return cfg_fail_at(c, c->route_line[i], errno, "route %s: not a directory: %s",
                               r->prefix, r->root);
        }
    }
    return 0;
}

/* CLI 옵션 중 값을 받는 것 */
static bool opt_takes_value(const char *a)
{
    return strcmp(a, "-p") == 0 || strcmp(a, "-r") == 0 || strcmp(a, "-c") == 0 ||
           strcmp(a, "-w") == 0 || strcmp(a, "-e") == 0;
}

static int apply_cli(cfg_ctx_t *c, const char *opt, const char *val)
{
    config_t *cfg = c->cfg;
    str_t v = str_from_cstr(val);
    uint64_t n;

    if (strcmp(opt, "-p") == 0) {
        if (parse_port(v, &cfg->port) != 0) {
            return cfg_fail_at(c, 0, EINVAL, "-p: invalid port '%s'", val);
        }
    } else if (strcmp(opt, "-r") == 0) {
        if (v.len == 0 || str_to_cstr(v, cfg->root, sizeof(cfg->root)) != 0) {
            return cfg_fail_at(c, 0, EINVAL, "-r: invalid path '%s'", val);
        }
    } else if (strcmp(opt, "-w") == 0) {
        if (parse_uint(v, 1024, &n) != 0) {
            return cfg_fail_at(c, 0, EINVAL, "-w: invalid worker count '%s' (1-1024)", val);
        }
        cfg->workers = (int)n;
    } else if (strcmp(opt, "-e") == 0) {
        if (strcmp(val, "pool") == 0) {
            cfg->engine = CFG_ENGINE_POOL;
        } else if (strcmp(val, "loop") == 0) {
            cfg->engine = CFG_ENGINE_LOOP;
        } else {
            return cfg_fail_at(c, 0, EINVAL, "-e: expected pool or loop, got '%s'", val);
        }
    }
    return 0; /* -c는 이미 처리됨 */
}

int config_load(int argc, char **argv, config_t *out_cfg, char *err, size_t errcap)
{
    cfg_ctx_t c;
    const char *file = NULL;

    memset(&c, 0, sizeof(c));
    c.cfg = out_cfg;
    c.err = err;
    c.errcap = errcap;
    if (err != NULL && errcap > 0) {
        err[0] = '\0';
    }
    config_defaults(out_cfg);

    /* 1. --help 우선. 값 자리의 "-h"는 옵션으로 보지 않는다 */
    for (int i = 1; i < argc; i += opt_takes_value(argv[i]) ? 2 : 1) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            return 1;
        }
    }
    /* 2. 옵션 모양 검사와 -c 찾기 (반복되면 마지막) */
    for (int i = 1; i < argc; i += 2) {
        if (!opt_takes_value(argv[i])) {
            return cfg_fail_at(&c, 0, EINVAL, "%s: unknown option", argv[i]);
        }
        if (i + 1 >= argc) {
            return cfg_fail_at(&c, 0, EINVAL, "%s: missing value", argv[i]);
        }
        if (strcmp(argv[i], "-c") == 0) {
            file = argv[i + 1];
        }
    }
    /* 3. 설정 파일 */
    if (file != NULL) {
        c.path = file;
        if (parse_file(&c) != 0) {
            return -1;
        }
        c.path = NULL;
    }
    /* 4. CLI가 파일보다 우선 */
    for (int i = 1; i + 1 < argc; i += 2) {
        if (apply_cli(&c, argv[i], argv[i + 1]) != 0) {
            return -1;
        }
    }
    /* 5. 검증 */
    return validate(&c, file);
}

void config_print_usage(const char *prog)
{
    if (prog == NULL) {
        prog = "httpd";
    }
    printf("Usage: %s [-p port] [-r root] [-c file] [-w workers] [-e pool|loop] [-h]\n", prog);
    printf("  -p <port>     listen port (1-65535, default 8080)\n");
    printf("  -r <root>     static file root directory (default ./www)\n");
    printf("  -c <file>     configuration file (grammar: src/core/config.h)\n");
    printf("  -w <workers>  worker threads for the pool engine (default: CPU cores x 2)\n");
    printf("  -e pool|loop  concurrency engine (default pool)\n");
    printf("  -h, --help    show this help and exit\n");
}
