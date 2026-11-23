/* src/core/config.c
 *
 * 기본값·설정 파일·CLI → config_t. 명세: config.h
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

/* core는 common의 상수를 include할 수 없다. config.h의 헤더 개수 한도를 적용한다. */
enum { CONFIG_MAX_HTTP_HEADERS = 64, CONFIG_MAX_TOKENS = CFG_MAX_BACKENDS + 2 };
static const char default_listen_host[] = "0.0.0.0";

void config_defaults(config_t *out_cfg)
{
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    int workers = 4;

    if (ncpu >= 1) {
        workers = ncpu > 512 ? 1024 : (int)ncpu * 2;
    }

    memset(out_cfg, 0, sizeof(*out_cfg));
    memcpy(out_cfg->listen_host, default_listen_host, sizeof(default_listen_host));
    out_cfg->port = 8080;
    out_cfg->backlog = 128;
    (void)snprintf(out_cfg->root, sizeof(out_cfg->root), "%s", "./www");
    out_cfg->engine = CFG_ENGINE_POOL;
    out_cfg->workers = workers;
    out_cfg->queue_len = 1024;
    out_cfg->max_head = 8192;
    out_cfg->max_uri = 2048;
    out_cfg->max_headers = CONFIG_MAX_HTTP_HEADERS;
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

typedef struct {
    const char *path;
    size_t root_line;
    size_t route_lines[CFG_MAX_ROUTES];
} config_source_t;

typedef enum { UINT_INT, UINT_SIZE, UINT_U64 } config_uint_type_t;

typedef struct {
    const char *name;
    size_t offset;
    config_uint_type_t type;
    uint64_t max;
} config_uint_field_t;

#define INT_FIELD(name) {#name, offsetof(config_t, name), UINT_INT, INT_MAX}
#define SIZE_FIELD(name, limit) {#name, offsetof(config_t, name), UINT_SIZE, (limit)}

static const config_uint_field_t uint_fields[] = {
    {"workers", offsetof(config_t, workers), UINT_INT, 1024},
    INT_FIELD(backlog),
    INT_FIELD(queue_len),
    SIZE_FIELD(max_head, SIZE_MAX),
    SIZE_FIELD(max_uri, SIZE_MAX),
    SIZE_FIELD(max_headers, CONFIG_MAX_HTTP_HEADERS),
    {"max_body", offsetof(config_t, max_body), UINT_U64, UINT64_MAX},
    INT_FIELD(header_timeout_ms),
    INT_FIELD(body_timeout_ms),
    INT_FIELD(keepalive_timeout_ms),
    INT_FIELD(max_keepalive_requests),
    INT_FIELD(cgi_timeout_ms),
    SIZE_FIELD(cgi_max_head, SIZE_MAX),
    INT_FIELD(upstream_connect_timeout_ms),
    INT_FIELD(upstream_response_timeout_ms),
    INT_FIELD(backend_fail_threshold),
    INT_FIELD(backend_cooldown_ms),
    INT_FIELD(pool_max_idle),
    INT_FIELD(shutdown_grace_ms),
};

#undef INT_FIELD
#undef SIZE_FIELD

static int fail(char *err, size_t cap, int code, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static int fail(char *err, size_t cap, int code, const char *fmt, ...)
{
    if (cap != 0) {
        va_list ap;
        va_start(ap, fmt);
        (void)vsnprintf(err, cap, fmt, ap);
        va_end(ap);
    }
    errno = code;
    return -1;
}

static bool read_positive(const char *text, uint64_t max, uint64_t *out)
{
    return str_to_u64(str_from_cstr(text), max, out) == 0 && *out != 0;
}

static bool read_engine(const char *text, cfg_engine_t *out)
{
    if (strcmp(text, "pool") == 0) {
        *out = CFG_ENGINE_POOL;
        return true;
    }
    if (strcmp(text, "loop") == 0) {
        *out = CFG_ENGINE_LOOP;
        return true;
    }
    return false;
}

static bool copy_token(const char *text, char *out, size_t cap)
{
    return str_to_cstr(str_from_cstr(text), out, cap) == 0;
}

static bool valid_name(const char *name, bool is_host)
{
    if (*name == '\0') {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)name; *p != '\0'; ++p) {
        unsigned char c = *p;
        if ((c < 'A' || c > 'Z') && (c < 'a' || c > 'z') && (c < '0' || c > '9') && c != '-' &&
            c != '_' && !(is_host && c == '.')) {
            return false;
        }
    }
    return true;
}

static bool read_port(const char *text, uint16_t *out)
{
    uint64_t port;
    if (!read_positive(text, UINT16_MAX, &port)) {
        return false;
    }
    *out = (uint16_t)port;
    return true;
}

static bool read_hostport(const char *text, char *host, uint16_t *port)
{
    const char *colon = strchr(text, ':');
    if (colon == NULL || !read_port(colon + 1, port)) {
        return false;
    }
    str_t name = {text, (size_t)(colon - text)};
    return str_to_cstr(name, host, CFG_HOST_MAX) == 0 && valid_name(host, true);
}

static bool read_listen(const char *text, config_t *cfg)
{
    if (strchr(text, ':') != NULL) {
        return read_hostport(text, cfg->listen_host, &cfg->port);
    }
    if (!read_port(text, &cfg->port)) {
        return false;
    }
    /* listen은 주소 전체를 덮어쓴다. 호스트를 생략하면 기본 바인딩 주소로 돌아간다. */
    memcpy(cfg->listen_host, default_listen_host, sizeof(default_listen_host));
    return true;
}

static const char *read_upstream(config_t *cfg, char *const *token, size_t count)
{
    if (count < 3 || !valid_name(token[1], false) || strlen(token[1]) >= CFG_NAME_MAX) {
        return "invalid upstream name or missing backends";
    }
    if (cfg->n_upstreams >= CFG_MAX_UPSTREAMS || count - 2 > CFG_MAX_BACKENDS) {
        return "too many upstreams or backends";
    }
    for (size_t i = 0; i < cfg->n_upstreams; ++i) {
        if (strcmp(cfg->upstreams[i].name, token[1]) == 0) {
            return "duplicate upstream";
        }
    }

    upstream_conf_t *u = &cfg->upstreams[cfg->n_upstreams];
    (void)copy_token(token[1], u->name, sizeof(u->name));
    for (size_t i = 2; i < count; ++i) {
        backend_conf_t *backend = &u->backends[i - 2];
        if (!read_hostport(token[i], backend->host, &backend->port)) {
            return "invalid upstream host:port";
        }
    }
    u->n_backends = count - 2;
    ++cfg->n_upstreams;
    return NULL;
}

static const char *read_route(config_t *cfg, char *const *token, size_t count, const char *host)
{
    if (count < 4 || strcmp(token[0], "route") != 0 || token[1][0] != '/') {
        return "invalid route syntax or prefix";
    }
    if (cfg->n_routes >= CFG_MAX_ROUTES) {
        return "too many routes";
    }
    route_conf_t *route = &cfg->routes[cfg->n_routes];
    if (!copy_token(token[1], route->prefix, sizeof(route->prefix)) ||
        (host != NULL &&
         (!valid_name(host, true) || !copy_token(host, route->host, sizeof(route->host))))) {
        return "invalid route prefix or host";
    }

    if (strcmp(token[2], "proxy") == 0) {
        route->kind = ROUTE_PROXY;
        if (!valid_name(token[3], false) ||
            !copy_token(token[3], route->upstream, sizeof(route->upstream))) {
            return "invalid upstream name";
        }
        size_t next = 4;
        if (next < count && strcmp(token[next], "strip") == 0) {
            route->strip_prefix = true;
            ++next;
        }
        if (next < count && strcmp(token[next], "keep_host") == 0) {
            route->keep_host = true;
            ++next;
        }
        if (next != count) {
            return "invalid proxy options";
        }
    } else if (count == 4 && (strcmp(token[2], "static") == 0 || strcmp(token[2], "cgi") == 0)) {
        route->kind = strcmp(token[2], "static") == 0 ? ROUTE_STATIC : ROUTE_CGI;
        if (!copy_token(token[3], route->root, sizeof(route->root))) {
            return "route root is too long";
        }
    } else {
        return "invalid route type or argument count";
    }
    ++cfg->n_routes;
    return NULL;
}

static bool set_uint(config_t *cfg, const config_uint_field_t *field, const char *text)
{
    uint64_t value;
    if (!read_positive(text, field->max, &value)) {
        return false;
    }

    /* 테이블은 지시어 이름과 실제 필드 타입을 함께 보관한다. */
    char *dest = (char *)cfg + field->offset;
    switch (field->type) {
    case UINT_INT: {
        int n = (int)value;
        memcpy(dest, &n, sizeof(n));
        break;
    }
    case UINT_SIZE: {
        size_t n = (size_t)value;
        memcpy(dest, &n, sizeof(n));
        break;
    }
    case UINT_U64:
        memcpy(dest, &value, sizeof(value));
        break;
    }
    return true;
}

static int read_directive(config_t *cfg, config_source_t *source, size_t line, char **token,
                          size_t count, char *err, size_t errcap)
{
    const char *name = token[0];
    const char *problem = NULL;

    if (strcmp(name, "route") == 0 || strcmp(name, "host") == 0) {
        size_t index = cfg->n_routes;
        if (strcmp(name, "route") == 0) {
            problem = read_route(cfg, token, count, NULL);
        } else {
            problem =
                count >= 3 ? read_route(cfg, token + 2, count - 2, token[1]) : "invalid host route";
        }
        if (problem == NULL) {
            source->route_lines[index] = line;
        }
    } else if (strcmp(name, "upstream") == 0) {
        problem = read_upstream(cfg, token, count);
    } else if (strcmp(name, "listen") == 0) {
        problem =
            count == 2 && read_listen(token[1], cfg) ? NULL : "invalid listen address or port";
    } else if (strcmp(name, "engine") == 0) {
        problem = count == 2 && read_engine(token[1], &cfg->engine) ? NULL : "invalid engine";
    } else if (strcmp(name, "root") == 0) {
        problem = count == 2 && copy_token(token[1], cfg->root, sizeof(cfg->root))
                      ? NULL
                      : "invalid root path";
        source->root_line = line;
    } else if (strcmp(name, "access_log") == 0 || strcmp(name, "error_log") == 0) {
        char *path = name[0] == 'a' ? cfg->access_log : cfg->error_log;
        problem =
            count == 2 && copy_token(token[1], path, CFG_PATH_MAX) ? NULL : "invalid log path";
    } else {
        const config_uint_field_t *field = NULL;
        for (size_t i = 0; i < sizeof(uint_fields) / sizeof(*uint_fields); ++i) {
            if (strcmp(name, uint_fields[i].name) == 0) {
                field = &uint_fields[i];
                break;
            }
        }
        if (field == NULL) {
            return fail(err, errcap, EINVAL, "%s:%zu: unknown directive '%s'", source->path, line,
                        name);
        }
        problem = count == 2 && set_uint(cfg, field, token[1]) ? NULL : "invalid numeric value";
    }

    if (problem != NULL) {
        return fail(err, errcap, EINVAL, "%s:%zu: %s: %s", source->path, line, name, problem);
    }
    return 0;
}

static size_t split_line(char *text, char **token)
{
    size_t count = 0;
    char *p = text;
    while (*p != '\0') {
        p += strspn(p, " \t");
        if (*p == '\0') {
            break;
        }
        if (count == CONFIG_MAX_TOKENS) {
            return CONFIG_MAX_TOKENS + 1;
        }
        token[count++] = p;
        p += strcspn(p, " \t");
        if (*p != '\0') {
            *p++ = '\0';
        }
    }
    return count;
}

static int read_file(config_t *cfg, config_source_t *source, char *err, size_t errcap)
{
    int fd = open(source->path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return fail(err, errcap, errno, "%s: cannot open configuration", source->path);
    }
    FILE *file = fdopen(fd, "r");
    if (file == NULL) {
        int saved = errno;
        (void)close(fd);
        return fail(err, errcap, saved, "%s: cannot read configuration", source->path);
    }

    char *buffer = NULL;
    size_t cap = 0;
    size_t line = 0;
    int rc = 0;
    for (;;) {
        errno = 0;
        ssize_t n = getline(&buffer, &cap, file);
        if (n < 0) {
            if (!feof(file)) {
                rc = fail(err, errcap, errno != 0 ? errno : EIO, "%s:%zu: cannot read line",
                          source->path, line + 1);
            }
            break;
        }
        ++line;

        /* CRLF와 LF를 먼저 벗기고 주석 앞에 있는 바이트만 설정 문법으로 검사한다. */
        size_t size = (size_t)n;
        if (buffer[size - 1] != '\n') {
            rc = fail(err, errcap, EINVAL, "%s:%zu: missing line ending", source->path, line);
            break;
        }
        --size;
        if (size > 0 && buffer[size - 1] == '\r') {
            --size;
        }
        char *comment = memchr(buffer, '#', size);
        if (comment != NULL) {
            size = (size_t)(comment - buffer);
        }
        bool bad_byte = false;
        for (size_t i = 0; i < size; ++i) {
            unsigned char c = (unsigned char)buffer[i];
            if ((c < 0x20 && c != '\t') || c == 0x7f) {
                bad_byte = true;
                break;
            }
        }
        if (bad_byte) {
            rc =
                fail(err, errcap, EINVAL, "%s:%zu: invalid configuration byte", source->path, line);
            break;
        }
        buffer[size] = '\0';

        char *token[CONFIG_MAX_TOKENS];
        size_t count = split_line(buffer, token);
        if (count > CONFIG_MAX_TOKENS) {
            rc = fail(err, errcap, EINVAL, "%s:%zu: too many arguments", source->path, line);
            break;
        }
        if (count != 0 && read_directive(cfg, source, line, token, count, err, errcap) < 0) {
            rc = -1;
            break;
        }
    }
    int saved = errno;
    free(buffer);
    (void)fclose(file);
    errno = saved;
    return rc;
}

typedef struct {
    const char *path;
    const char *port;
    const char *root;
    const char *workers;
    const char *engine;
} config_cli_t;

static int collect_cli(int argc, char **argv, config_cli_t *cli, char *err, size_t errcap)
{
    for (int i = 1; i < argc; ++i) {
        const char *option = argv[i];
        const char **destination;
        if (strcmp(option, "-c") == 0) {
            destination = &cli->path;
        } else if (strcmp(option, "-p") == 0) {
            destination = &cli->port;
        } else if (strcmp(option, "-r") == 0) {
            destination = &cli->root;
        } else if (strcmp(option, "-w") == 0) {
            destination = &cli->workers;
        } else if (strcmp(option, "-e") == 0) {
            destination = &cli->engine;
        } else {
            return fail(err, errcap, EINVAL, "%s: unknown option or positional argument", option);
        }
        if (i == argc - 1) {
            return fail(err, errcap, EINVAL, "%s: missing value", option);
        }
        *destination = argv[++i];
    }
    return 0;
}

static int apply_cli(config_t *cfg, config_source_t *source, const config_cli_t *cli, char *err,
                     size_t errcap)
{
    if (cli->port != NULL && !read_port(cli->port, &cfg->port)) {
        return fail(err, errcap, EINVAL, "-p: invalid port '%s'", cli->port);
    }
    if (cli->root != NULL) {
        if (cli->root[0] == '\0' || !copy_token(cli->root, cfg->root, sizeof(cfg->root))) {
            return fail(err, errcap, EINVAL, "-r: invalid root '%s'", cli->root);
        }
        source->root_line = 0;
    }
    if (cli->workers != NULL) {
        uint64_t workers;
        if (!read_positive(cli->workers, 1024, &workers)) {
            return fail(err, errcap, EINVAL, "-w: invalid workers '%s'", cli->workers);
        }
        cfg->workers = (int)workers;
    }
    if (cli->engine != NULL && !read_engine(cli->engine, &cfg->engine)) {
        return fail(err, errcap, EINVAL, "-e: invalid engine '%s'", cli->engine);
    }
    return 0;
}

static bool upstream_exists(const config_t *cfg, const char *name)
{
    for (size_t i = 0; i < cfg->n_upstreams; ++i) {
        if (strcmp(cfg->upstreams[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

static int check_directory(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0) {
        return errno;
    }
    return S_ISDIR(st.st_mode) ? 0 : ENOTDIR;
}

static int check_config(const config_t *cfg, const config_source_t *source, char *err, size_t cap)
{
    int code = check_directory(cfg->root);
    if (code != 0) {
        if (source->root_line != 0) {
            return fail(err, cap, code, "%s:%zu: root: not a directory: %s", source->path,
                        source->root_line, cfg->root);
        }
        return fail(err, cap, code, "root: not a directory: %s", cfg->root);
    }

    /* 파일 끝까지 읽은 뒤 참조를 검사하므로 route가 뒤에 정의될 upstream도 가리킬 수 있다. */
    for (size_t i = 0; i < cfg->n_routes; ++i) {
        const route_conf_t *route = &cfg->routes[i];
        if (route->kind == ROUTE_PROXY) {
            if (!upstream_exists(cfg, route->upstream)) {
                return fail(err, cap, EINVAL, "%s:%zu: undefined upstream '%s'", source->path,
                            source->route_lines[i], route->upstream);
            }
        } else {
            code = check_directory(route->root);
            if (code != 0) {
                return fail(err, cap, code, "%s:%zu: route root: not a directory: %s", source->path,
                            source->route_lines[i], route->root);
            }
        }
    }
    return 0;
}

int config_load(int argc, char **argv, config_t *out_cfg, char *err, size_t errcap)
{
    config_defaults(out_cfg);
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            return 1;
        }
    }
    config_cli_t cli = {0};
    if (collect_cli(argc, argv, &cli, err, errcap) < 0) {
        return -1;
    }

    config_source_t source = {.path = cli.path};
    if ((cli.path != NULL && read_file(out_cfg, &source, err, errcap) < 0) ||
        apply_cli(out_cfg, &source, &cli, err, errcap) < 0) {
        return -1;
    }
    return check_config(out_cfg, &source, err, errcap);
}
