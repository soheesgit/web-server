/* tests/fakes/fake_handler.c
 *
 * fake_handler.h 구현.
 */
#include "fakes/fake_handler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unit/testlib.h"

static int64_t g_now_ms = 1000000;

void fake_request(http_request_t *out, http_method_t m, const char *path, const char *host)
{
    static const char *names[] = {"GET", "HEAD", "POST", "OTHER"};
    size_t plen = strlen(path);

    memset(out, 0, sizeof(*out));
    out->method = m;
    out->method_raw = str_from_cstr(names[m <= HTTP_OTHER ? m : HTTP_OTHER]);
    if (plen >= sizeof(out->path)) {
        plen = sizeof(out->path) - 1;
    }
    memcpy(out->path, path, plen);
    out->path[plen] = '\0';
    out->target.ptr = out->path;
    out->target.len = plen;
    out->raw_path = out->target;
    out->minor = 1;
    out->keep_alive = true;
    out->framing.kind = FRAMING_NONE;
    if (host != NULL) {
        out->host.ptr = host;
        out->host.len = strlen(host);
        fake_request_add_header(out, "Host", host);
    }
}

void fake_request_add_header(http_request_t *r, const char *name, const char *value)
{
    http_header_t *h;

    if (r->headers.n >= HTTP_MAX_HEADERS) {
        return;
    }
    h = &r->headers.v[r->headers.n++];
    h->name.ptr = name;
    h->name.len = strlen(name);
    h->value.ptr = value;
    h->value.len = strlen(value);
}

config_t *fake_config(void)
{
    config_t *cfg = calloc(1, sizeof(*cfg));
    const char *dir;

    if (cfg == NULL) {
        return NULL;
    }
    config_defaults(cfg);
    dir = test_tmpdir();
    if (dir == NULL ||
        snprintf(cfg->root, sizeof(cfg->root), "%s", dir) >= (int)sizeof(cfg->root)) {
        free(cfg);
        return NULL;
    }
    return cfg;
}

int64_t fake_now_ms(void)
{
    return g_now_ms;
}

void fake_advance_ms(int64_t d)
{
    g_now_ms += d;
}
