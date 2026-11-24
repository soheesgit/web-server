/* src/core/config.c
 *
 * 기본값·설정 파일·CLI → config_t. 명세: config.h
 */
#include "core/config.h"

#include "core/str.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void config_defaults(config_t *out_cfg)
{
    long cpu_count;

    if (out_cfg == NULL) {
        return;
    }

    memset(out_cfg, 0, sizeof(*out_cfg));

    (void)snprintf(out_cfg->listen_host, sizeof(out_cfg->listen_host), "%s", "0.0.0.0");
    (void)snprintf(out_cfg->root, sizeof(out_cfg->root), "%s", "./www");

    out_cfg->port = 8080;
    out_cfg->backlog = 128;
    out_cfg->engine = CFG_ENGINE_POOL;
    out_cfg->queue_len = 1024;
    out_cfg->max_head = 8192;
    out_cfg->max_uri = 2048;
    out_cfg->max_headers = 64;
    out_cfg->max_body = 10485760;
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
    out_cfg->cache_max_bytes = 8388608;
    out_cfg->cache_entry_max = 1048576;

    cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpu_count < 1) {
        out_cfg->workers = 4;
    } else if (cpu_count > 512) {
        out_cfg->workers = 1024;
    } else {
        out_cfg->workers = (int)(cpu_count * 2);
    }
}

static void set_error(char *err, size_t cap, const char *fmt, const char *a, const char *b)
{
    if (err != NULL && cap > 0) {
        (void)snprintf(err, cap, fmt, a == NULL ? "" : a, b == NULL ? "" : b);
    }
}

static int copy_value(char *out, size_t cap, const char *value)
{
    size_t n = strlen(value);
    if (n == 0 || n >= cap) {
        errno = n == 0 ? EINVAL : ENAMETOOLONG;
        return -1;
    }
    memcpy(out, value, n + 1);
    return 0;
}

static int parse_uint(const char *s, uint64_t max, uint64_t *out)
{
    return str_to_u64(str_from_cstr(s), max, out);
}

static int parse_port(const char *s, uint16_t *out)
{
    uint64_t n;
    if (parse_uint(s, 65535, &n) != 0 || n == 0) {
        errno = EINVAL;
        return -1;
    }
    *out = (uint16_t)n;
    return 0;
}

static int parse_listen(config_t *cfg, const char *s)
{
    const char *colon = strrchr(s, ':');
    uint16_t port;
    if (colon == NULL) {
        if (parse_port(s, &port) != 0) {
            return -1;
        }
        cfg->port = port;
        (void)snprintf(cfg->listen_host, sizeof(cfg->listen_host), "%s", "0.0.0.0");
        return 0;
    }
    if (colon == s || (size_t)(colon - s) >= sizeof(cfg->listen_host) ||
        parse_port(colon + 1, &port) != 0) {
        errno = EINVAL;
        return -1;
    }
    memcpy(cfg->listen_host, s, (size_t)(colon - s));
    cfg->listen_host[colon - s] = '\0';
    cfg->port = port;
    return 0;
}

static int parse_hostport(const char *s, backend_conf_t *b)
{
    const char *colon = strrchr(s, ':');
    char portbuf[16];
    size_t hostlen;
    if (colon == NULL || colon == s) {
        errno = EINVAL;
        return -1;
    }
    hostlen = (size_t)(colon - s);
    if (hostlen >= sizeof(b->host) || strlen(colon + 1) >= sizeof(portbuf)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(b->host, s, hostlen);
    b->host[hostlen] = '\0';
    (void)snprintf(portbuf, sizeof(portbuf), "%s", colon + 1);
    return parse_port(portbuf, &b->port);
}

static int valid_name(const char *s)
{
    size_t i, n = strlen(s);
    if (n == 0 || n >= CFG_NAME_MAX) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-')) {
            return 0;
        }
    }
    return 1;
}

static int parse_knob(config_t *cfg, const char *key, const char *value)
{
    uint64_t n;
    uint64_t max = INT_MAX;
    if (strcmp(key, "max_head") == 0 || strcmp(key, "max_uri") == 0 ||
        strcmp(key, "cgi_max_head") == 0) {
        max = SIZE_MAX;
    } else if (strcmp(key, "max_headers") == 0) {
        max = 256;
    } else if (strcmp(key, "max_body") == 0) {
        max = UINT64_MAX;
    }
    if (parse_uint(value, max, &n) != 0 || (n == 0 && strcmp(key, "cache_max_bytes") != 0)) {
        return -1;
    }
#define SET_INT(k, field)                                                                          \
    if (strcmp(key, (k)) == 0) {                                                                   \
        cfg->field = (int)n;                                                                       \
        return 0;                                                                                  \
    }
    SET_INT("backlog", backlog)
    SET_INT("workers", workers)
    SET_INT("queue_len", queue_len)
    SET_INT("header_timeout_ms", header_timeout_ms)
    SET_INT("body_timeout_ms", body_timeout_ms)
    SET_INT("keepalive_timeout_ms", keepalive_timeout_ms)
    SET_INT("max_keepalive_requests", max_keepalive_requests)
    SET_INT("cgi_timeout_ms", cgi_timeout_ms)
    SET_INT("upstream_connect_timeout_ms", upstream_connect_timeout_ms)
    SET_INT("upstream_response_timeout_ms", upstream_response_timeout_ms)
    SET_INT("backend_fail_threshold", backend_fail_threshold)
    SET_INT("backend_cooldown_ms", backend_cooldown_ms)
    SET_INT("pool_max_idle", pool_max_idle)
    SET_INT("shutdown_grace_ms", shutdown_grace_ms)
    SET_INT("cache_max_bytes", cache_max_bytes)
    SET_INT("cache_entry_max", cache_entry_max)
#undef SET_INT
    if (strcmp(key, "max_head") == 0)
        cfg->max_head = (size_t)n;
    else if (strcmp(key, "max_uri") == 0)
        cfg->max_uri = (size_t)n;
    else if (strcmp(key, "max_headers") == 0)
        cfg->max_headers = (size_t)n;
    else if (strcmp(key, "max_body") == 0)
        cfg->max_body = n;
    else if (strcmp(key, "cgi_max_head") == 0)
        cfg->cgi_max_head = (size_t)n;
    else
        return -1;
    return 0;
}

static int parse_route(config_t *cfg, char **v, size_t n, size_t start, const char *host)
{
    route_conf_t r;
    size_t i;
    memset(&r, 0, sizeof(r));
    if (start + 2 > n || v[start][0] != '/' || strlen(v[start]) >= sizeof(r.prefix)) {
        errno = EINVAL;
        return -1;
    }
    (void)snprintf(r.prefix, sizeof(r.prefix), "%s", v[start]);
    if (host != NULL && copy_value(r.host, sizeof(r.host), host) != 0) {
        return -1;
    }
    if (strcmp(v[start + 1], "static") == 0 || strcmp(v[start + 1], "cgi") == 0) {
        if (start + 3 != n) {
            errno = EINVAL;
            return -1;
        }
        r.kind = strcmp(v[start + 1], "static") == 0 ? ROUTE_STATIC : ROUTE_CGI;
        if (copy_value(r.root, sizeof(r.root), v[start + 2]) != 0) {
            return -1;
        }
    } else if (strcmp(v[start + 1], "forward") == 0) {
        if (start + 2 != n) {
            errno = EINVAL;
            return -1;
        }
        r.kind = ROUTE_FORWARD;
        r.keep_host = true; /* 목적지 서버에는 URI에 적힌 authority를 Host로 보낸다 */
        (void)snprintf(r.upstream, sizeof(r.upstream), "%s", CFG_FORWARD_UPSTREAM);
    } else if (strcmp(v[start + 1], "proxy") == 0) {
        if (start + 3 > n || start + 5 < n) {
            errno = EINVAL;
            return -1;
        }
        r.kind = ROUTE_PROXY;
        if (!valid_name(v[start + 2]) ||
            copy_value(r.upstream, sizeof(r.upstream), v[start + 2]) != 0) {
            errno = EINVAL;
            return -1;
        }
        for (i = start + 3; i < n; i++) {
            if (strcmp(v[i], "strip") == 0 && !r.strip_prefix)
                r.strip_prefix = true;
            else if (strcmp(v[i], "keep_host") == 0 && !r.keep_host)
                r.keep_host = true;
            else {
                errno = EINVAL;
                return -1;
            }
        }
    } else {
        errno = EINVAL;
        return -1;
    }
    if (cfg->n_routes >= CFG_MAX_ROUTES) {
        errno = E2BIG;
        return -1;
    }
    cfg->routes[cfg->n_routes++] = r;
    return 0;
}

static int parse_line(config_t *cfg, char **v, size_t n)
{
    if (n == 0)
        return 0;
    if (strcmp(v[0], "listen") == 0 && n == 2)
        return parse_listen(cfg, v[1]);
    if (strcmp(v[0], "root") == 0 && n == 2)
        return copy_value(cfg->root, sizeof(cfg->root), v[1]);
    if (strcmp(v[0], "engine") == 0 && n == 2) {
        if (strcmp(v[1], "pool") == 0)
            cfg->engine = CFG_ENGINE_POOL;
        else if (strcmp(v[1], "loop") == 0)
            cfg->engine = CFG_ENGINE_LOOP;
        else {
            errno = EINVAL;
            return -1;
        }
        return 0;
    }
    if (strcmp(v[0], "upstream") == 0 && n >= 3) {
        upstream_conf_t *u;
        size_t i;
        if (!valid_name(v[1]) || cfg->n_upstreams >= CFG_MAX_UPSTREAMS) {
            errno = EINVAL;
            return -1;
        }
        for (i = 0; i < cfg->n_upstreams; i++) {
            if (strcmp(cfg->upstreams[i].name, v[1]) == 0) {
                errno = EEXIST;
                return -1;
            }
        }
        u = &cfg->upstreams[cfg->n_upstreams];
        if (copy_value(u->name, sizeof(u->name), v[1]) != 0)
            return -1;
        if (n - 2 > CFG_MAX_BACKENDS) {
            errno = E2BIG;
            return -1;
        }
        for (i = 2; i < n; i++) {
            if (parse_hostport(v[i], &u->backends[i - 2]) != 0)
                return -1;
        }
        u->n_backends = n - 2;
        cfg->n_upstreams++;
        return 0;
    }
    if (strcmp(v[0], "route") == 0 && n >= 3)
        return parse_route(cfg, v, n, 1, NULL);
    if (strcmp(v[0], "host") == 0 && n >= 4 && strcmp(v[2], "route") == 0)
        return parse_route(cfg, v, n, 3, v[1]);
    if ((strcmp(v[0], "access_log") == 0 || strcmp(v[0], "error_log") == 0) && n == 2)
        return copy_value(strcmp(v[0], "access_log") == 0 ? cfg->access_log : cfg->error_log,
                          CFG_PATH_MAX, v[1]);
    if (n == 2) {
        if (parse_knob(cfg, v[0], v[1]) == 0)
            return 0;
        if (errno == EINVAL || errno == ERANGE)
            return -1;
    }
    errno = EINVAL;
    return -1;
}

static int read_config(config_t *cfg, const char *path, char *err, size_t errcap)
{
    FILE *f = fopen(path, "r");
    char *line = NULL;
    size_t cap = 0;
    ssize_t got;
    size_t lineno = 0;
    int result = 0;
    if (f == NULL)
        return -1;
    while ((got = getline(&line, &cap, f)) >= 0) {
        char *v[CFG_MAX_BACKENDS + 8];
        size_t n = 0;
        char *p, *hash;
        lineno++;
        if ((size_t)got > 0 && line[got - 1] == '\n')
            line[--got] = '\0';
        if (got > 0 && line[got - 1] == '\r')
            line[--got] = '\0';
        hash = strchr(line, '#');
        if (hash != NULL)
            *hash = '\0';
        p = line;
        while (*p != '\0') {
            while (*p == ' ' || *p == '\t')
                p++;
            if (*p == '\0')
                break;
            if (n == sizeof(v) / sizeof(v[0])) {
                errno = E2BIG;
                break;
            }
            v[n++] = p;
            while (*p != '\0' && *p != ' ' && *p != '\t')
                p++;
            if (*p != '\0')
                *p++ = '\0';
        }
        if (n > 0 && parse_line(cfg, v, n) != 0) {
            char detail[160];
            (void)snprintf(detail, sizeof(detail), "%s", v[0]);
            if (err != NULL && errcap > 0)
                (void)snprintf(err, errcap, "%s:%zu: invalid directive '%s'", path, lineno, detail);
            errno = EINVAL;
            result = -1;
            break;
        }
    }
    if (ferror(f) && result == 0)
        result = -1;
    free(line);
    (void)fclose(f);
    return result;
}

static int validate_config(config_t *cfg, char *err, size_t errcap)
{
    struct stat st;
    size_t i, j;
    if (stat(cfg->root, &st) != 0 || !S_ISDIR(st.st_mode)) {
        set_error(err, errcap, "root: not a directory: %s", cfg->root, NULL);
        errno = ENOTDIR;
        return -1;
    }
    for (i = 0; i < cfg->n_routes; i++) {
        route_conf_t *r = &cfg->routes[i];
        if (r->kind == ROUTE_STATIC || r->kind == ROUTE_CGI) {
            if (stat(r->root, &st) != 0 || !S_ISDIR(st.st_mode)) {
                set_error(err, errcap, "route: not a directory: %s", r->root, NULL);
                errno = ENOTDIR;
                return -1;
            }
        } else if (r->kind == ROUTE_PROXY) {
            for (j = 0; j < cfg->n_upstreams; j++) {
                if (strcmp(r->upstream, cfg->upstreams[j].name) == 0)
                    break;
            }
            if (j == cfg->n_upstreams) {
                set_error(err, errcap, "route: undefined upstream: %s", r->upstream, NULL);
                errno = EINVAL;
                return -1;
            }
        }
    }
    if (cfg->max_headers > 64) {
        set_error(err, errcap, "max_headers: exceeds HTTP_MAX_HEADERS", NULL, NULL);
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int apply_cli(config_t *cfg, int argc, char **argv, char *err, size_t errcap)
{
    int i;
    for (i = 1; i < argc; i++) {
        uint64_t n;
        const char *arg = argv[i];
        if (strcmp(arg, "-c") == 0) {
            i++;
            continue;
        }
        if (strcmp(arg, "-p") == 0 || strcmp(arg, "-r") == 0 || strcmp(arg, "-w") == 0 ||
            strcmp(arg, "-e") == 0) {
            const char *val;
            if (i + 1 >= argc) {
                set_error(err, errcap, "%s: missing value", arg, NULL);
                errno = EINVAL;
                return -1;
            }
            val = argv[++i];
            if (strcmp(arg, "-p") == 0) {
                if (parse_port(val, &cfg->port) != 0) {
                    set_error(err, errcap, "-p: invalid port '%s'", val, NULL);
                    errno = EINVAL;
                    return -1;
                }
            } else if (strcmp(arg, "-r") == 0) {
                if (copy_value(cfg->root, sizeof(cfg->root), val) != 0) {
                    set_error(err, errcap, "-r: invalid root '%s'", val, NULL);
                    errno = EINVAL;
                    return -1;
                }
            } else if (strcmp(arg, "-w") == 0) {
                if (parse_uint(val, 1024, &n) != 0 || n == 0) {
                    set_error(err, errcap, "-w: invalid workers '%s'", val, NULL);
                    errno = EINVAL;
                    return -1;
                }
                cfg->workers = (int)n;
            } else {
                if (strcmp(val, "pool") == 0)
                    cfg->engine = CFG_ENGINE_POOL;
                else if (strcmp(val, "loop") == 0)
                    cfg->engine = CFG_ENGINE_LOOP;
                else {
                    set_error(err, errcap, "-e: invalid engine '%s'", val, NULL);
                    errno = EINVAL;
                    return -1;
                }
            }
        } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            continue;
        } else {
            set_error(err, errcap, "option: unknown argument '%s'", arg, NULL);
            errno = EINVAL;
            return -1;
        }
    }
    return 0;
}

int config_load(int argc, char **argv, config_t *out_cfg, char *err, size_t errcap)
{
    const char *config_path = NULL;
    int i;
    config_t cfg;
    if (out_cfg == NULL || argv == NULL || argc < 1) {
        errno = EINVAL;
        return -1;
    }
    if (err != NULL && errcap > 0)
        err[0] = '\0';
    for (i = 1; i < argc; i++) {
        if (argv[i] != NULL && (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0))
            return 1;
        if (argv[i] != NULL && strcmp(argv[i], "-c") == 0) {
            if (i + 1 >= argc) {
                set_error(err, errcap, "-c: missing file", NULL, NULL);
                errno = EINVAL;
                return -1;
            }
            config_path = argv[++i];
        }
    }
    config_defaults(&cfg);
    if (config_path != NULL && read_config(&cfg, config_path, err, errcap) != 0)
        return -1;
    if (apply_cli(&cfg, argc, argv, err, errcap) != 0)
        return -1;
    if (validate_config(&cfg, err, errcap) != 0)
        return -1;
    *out_cfg = cfg;
    return 0;
}

void config_print_usage(const char *prog)
{
    if (prog == NULL)
        prog = "httpd";
    printf("Usage: %s [-p port] [-r root] [-c file] [-w workers] [-e pool|loop] [-h]\n", prog);
    printf("  -p <port>     listen port (1-65535, default 8080)\n");
    printf("  -r <root>     static file root directory (default ./www)\n");
    printf("  -c <file>     configuration file (grammar: src/core/config.h)\n");
    printf("  -w <workers>  worker threads for the pool engine (default: CPU cores x 2)\n");
    printf("  -e pool|loop  concurrency engine (default pool)\n");
    printf("  -h, --help    show this help and exit\n");
}
