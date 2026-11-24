/* src/handler/cache.c
 *
 * 포워드 프록시 응답 캐시. 정책과 API: internal_cache.h, 결정: D-042
 */
#include "handler/internal_cache.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/http_headers.h"
#include "core/buf.h"
#include "core/platform.h"
#include "handler/plan.h"

#define CACHE_MAX_TTL_S 86400 /* 신선도 상한: 하루 */

typedef struct entry {
    struct entry *prev, *next;
    char *key;
    size_t key_len;
    int status;
    char *fields;
    size_t fields_len;
    char *body;
    size_t body_len;
    int64_t stored_ms;
    int64_t ttl_ms;
    size_t cost;
} entry_t;

struct cache_fill {
    char *key;
    size_t key_len;
    size_t entry_max, total_max;
    bool active; /* arm이 통과했고 아직 포기하지 않음 */
    int status;
    char *fields;
    size_t fields_len;
    char *body;
    size_t body_len, body_want;
    int64_t ttl_ms;
};

/* shortcut: 항목을 연결 리스트로 훑는다(O(n)). 항목이 수천 개를 넘으면 해시 표로 바꾼다. */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static entry_t *g_head, *g_tail; /* head가 가장 최근에 쓴 항목 */
static size_t g_bytes;

/* 역할: 리스트에서 항목을 뺀다 (뮤텍스를 쥔 채 부른다). */
static void unlink_entry(entry_t *e)
{
    if (e->prev != NULL)
        e->prev->next = e->next;
    else
        g_head = e->next;
    if (e->next != NULL)
        e->next->prev = e->prev;
    else
        g_tail = e->prev;
    e->prev = e->next = NULL;
}

/* 역할: 항목을 리스트 맨 앞(가장 최근)에 둔다. */
static void push_front(entry_t *e)
{
    e->prev = NULL;
    e->next = g_head;
    if (g_head != NULL)
        g_head->prev = e;
    g_head = e;
    if (g_tail == NULL)
        g_tail = e;
}

/* 역할: 항목을 리스트에서 빼고 메모리를 해제하며 총 바이트를 줄인다. */
static void remove_entry(entry_t *e)
{
    unlink_entry(e);
    g_bytes -= e->cost;
    free(e->key);
    free(e->fields);
    free(e->body);
    free(e);
}

static entry_t *find_entry(const char *key, size_t key_len)
{
    entry_t *e;
    for (e = g_head; e != NULL; e = e->next) {
        if (e->key_len == key_len && memcmp(e->key, key, key_len) == 0)
            return e;
    }
    return NULL;
}

size_t cache_make_key(str_t target, char *out, size_t cap)
{
    static const size_t scheme_len = 7; /* "http://" */
    size_t i, host_end;
    if (target.len <= scheme_len || target.len >= cap ||
        !str_eq_ci((str_t){target.ptr, scheme_len}, "http://"))
        return 0;
    host_end = scheme_len;
    while (host_end < target.len && target.ptr[host_end] != '/' && target.ptr[host_end] != '?')
        host_end++;
    for (i = 0; i < target.len; i++) {
        char c = target.ptr[i];
        out[i] = (i < host_end && c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
    }
    out[target.len] = '\0';
    return target.len;
}

/* Cache-Control 지시어 모음. max-age 계열은 -1이면 없음. */
typedef struct {
    bool no_store, no_cache, private_dir;
    int64_t max_age, s_maxage;
} cc_t;

/* 역할: 접두사(소문자, '=' 포함)로 시작하면 그 뒤 숫자를 읽는다. 숫자가 아니면 0을 돌려준다. */
static bool read_seconds(str_t token, const char *prefix, int64_t *out)
{
    size_t n = strlen(prefix);
    uint64_t v;
    str_t rest;
    if (token.len <= n || !str_eq_ci((str_t){token.ptr, n}, prefix))
        return false;
    rest = (str_t){token.ptr + n, token.len - n};
    if (rest.len >= 2 && rest.ptr[0] == '"' && rest.ptr[rest.len - 1] == '"')
        rest = (str_t){rest.ptr + 1, rest.len - 2};
    if (str_to_u64(rest, 1u << 30, &v) != 0)
        return false;
    *out = (int64_t)v;
    return true;
}

/* 역할: Cache-Control 값 하나를 쉼표로 나눠 지시어를 읽는다. */
static void parse_cc(str_t v, cc_t *cc)
{
    while (v.len > 0) {
        const char *comma = memchr(v.ptr, ',', v.len);
        size_t n = comma != NULL ? (size_t)(comma - v.ptr) : v.len;
        str_t token = str_trim_ows((str_t){v.ptr, n});
        int64_t sec;
        if (str_eq_ci(token, "no-store"))
            cc->no_store = true;
        else if (str_eq_ci(token, "no-cache"))
            cc->no_cache = true;
        else if (str_eq_ci(token, "private"))
            cc->private_dir = true;
        else if (read_seconds(token, "s-maxage=", &sec))
            cc->s_maxage = sec;
        else if (read_seconds(token, "max-age=", &sec))
            cc->max_age = sec;
        v = comma != NULL ? (str_t){comma + 1, v.len - n - 1} : (str_t){NULL, 0};
    }
}

/* 역할: 헤더 목록의 모든 Cache-Control 줄을 합쳐 읽는다. */
static void collect_cc(const http_headers_t *h, cc_t *cc)
{
    size_t i;
    cc->no_store = cc->no_cache = cc->private_dir = false;
    cc->max_age = cc->s_maxage = -1;
    for (i = 0; i < h->n; i++) {
        if (str_eq_ci(h->v[i].name, "Cache-Control"))
            parse_cc(h->v[i].value, cc);
    }
}

void cache_request_policy(const http_request_t *req, bool *out_bypass, bool *out_store)
{
    cc_t cc;
    const http_header_t *pragma;
    bool restricted = http_header_get(&req->headers, "Authorization") != NULL ||
                      http_header_get(&req->headers, "Range") != NULL;
    collect_cc(&req->headers, &cc);
    pragma = http_header_get(&req->headers, "Pragma");
    *out_bypass = restricted || cc.no_cache || cc.no_store ||
                  (pragma != NULL && str_eq_ci(str_trim_ows(pragma->value), "no-cache"));
    *out_store = !restricted && !cc.no_store;
}

int cache_serve(const char *key, size_t key_len, bool keep_alive, response_plan_t *out_plan)
{
    int64_t now = platform_now_ms();
    entry_t *e;
    int rc = -1;
    char age[32];
    pthread_mutex_lock(&g_mu);
    e = find_entry(key, key_len);
    if (e != NULL && now - e->stored_ms >= e->ttl_ms) {
        remove_entry(e);
        e = NULL;
    }
    if (e != NULL) {
        unlink_entry(e);
        push_front(e);
        if (plan_set_mem(out_plan, e->status, NULL, e->body, e->body_len) == 0 &&
            (out_plan->head.fields.max != 0 ||
             buf_init(&out_plan->head.fields, 256, 64u * 1024u) == 0) &&
            buf_append(&out_plan->head.fields, e->fields, e->fields_len) == 0) {
            (void)snprintf(age, sizeof(age), "%lld", (long long)((now - e->stored_ms) / 1000));
            if (plan_add_header(out_plan, "Age", age) == 0 &&
                plan_add_header(out_plan, "X-Cache", "HIT") == 0) {
                out_plan->head.close = !keep_alive;
                rc = 0;
            }
        }
        if (rc != 0)
            plan_cleanup(out_plan);
    }
    pthread_mutex_unlock(&g_mu);
    return rc;
}

void cache_invalidate(const char *key, size_t key_len)
{
    entry_t *e;
    pthread_mutex_lock(&g_mu);
    e = find_entry(key, key_len);
    if (e != NULL)
        remove_entry(e);
    pthread_mutex_unlock(&g_mu);
}

cache_fill_t *cache_fill_new(const char *key, size_t key_len, size_t entry_max, size_t total_max)
{
    cache_fill_t *f = calloc(1, sizeof(*f));
    if (f == NULL)
        return NULL;
    f->key = malloc(key_len + 1);
    if (f->key == NULL) {
        free(f);
        return NULL;
    }
    memcpy(f->key, key, key_len);
    f->key[key_len] = '\0';
    f->key_len = key_len;
    f->entry_max = entry_max;
    f->total_max = total_max;
    return f;
}

int cache_fill_arm(cache_fill_t *f, const http_response_t *resp, const buf_t *fields)
{
    cc_t cc;
    size_t i;
    uint64_t age = 0, v;
    int64_t ttl_s;
    if (f == NULL || resp == NULL || fields == NULL)
        return -1;
    if (resp->status != 200 || resp->framing.kind != FRAMING_LENGTH ||
        resp->framing.length > f->entry_max || resp->framing.length > f->total_max)
        return -1;
    collect_cc(&resp->headers, &cc);
    for (i = 0; i < resp->headers.n; i++) {
        str_t name = resp->headers.v[i].name;
        if (str_eq_ci(name, "Vary") || str_eq_ci(name, "Set-Cookie"))
            return -1;
        if (str_eq_ci(name, "Age") && str_to_u64(resp->headers.v[i].value, 1u << 30, &v) == 0)
            age = v;
    }
    if (cc.no_store || cc.no_cache || cc.private_dir)
        return -1;
    ttl_s = cc.s_maxage >= 0 ? cc.s_maxage : cc.max_age;
    if (ttl_s < 0 || (uint64_t)ttl_s <= age)
        return -1;
    ttl_s -= (int64_t)age;
    if (ttl_s > CACHE_MAX_TTL_S)
        ttl_s = CACHE_MAX_TTL_S;
    f->fields = malloc(fields->len + 1);
    f->body = malloc(resp->framing.length + 1);
    if (f->fields == NULL || f->body == NULL) {
        free(f->fields);
        free(f->body);
        f->fields = f->body = NULL;
        return -1;
    }
    if (fields->len > 0)
        memcpy(f->fields, fields->data, fields->len);
    f->fields_len = fields->len;
    f->body_want = (size_t)resp->framing.length;
    f->body_len = 0;
    f->status = resp->status;
    f->ttl_ms = ttl_s * 1000;
    f->active = true;
    return 0;
}

void cache_fill_data(cache_fill_t *f, const char *p, size_t n)
{
    if (f == NULL || !f->active || n == 0)
        return;
    if (n > f->body_want - f->body_len) { /* 선언한 길이보다 많이 옴: 저장을 포기한다 */
        f->active = false;
        return;
    }
    memcpy(f->body + f->body_len, p, n);
    f->body_len += n;
}

void cache_fill_commit(cache_fill_t *f)
{
    entry_t *e, *old;
    if (f == NULL || !f->active || f->body_len != f->body_want)
        return;
    f->active = false;
    e = calloc(1, sizeof(*e));
    if (e == NULL)
        return;
    e->key = f->key;
    e->key_len = f->key_len;
    e->status = f->status;
    e->fields = f->fields;
    e->fields_len = f->fields_len;
    e->body = f->body;
    e->body_len = f->body_len;
    e->ttl_ms = f->ttl_ms;
    e->cost = e->key_len + e->fields_len + e->body_len + sizeof(*e);
    f->key = f->fields = f->body = NULL; /* 소유권이 항목으로 넘어감 */
    if (e->cost > f->total_max) {
        free(e->key);
        free(e->fields);
        free(e->body);
        free(e);
        return;
    }
    e->stored_ms = platform_now_ms();
    pthread_mutex_lock(&g_mu);
    old = find_entry(e->key, e->key_len);
    if (old != NULL)
        remove_entry(old);
    while (g_tail != NULL && g_bytes + e->cost > f->total_max)
        remove_entry(g_tail);
    push_front(e);
    g_bytes += e->cost;
    pthread_mutex_unlock(&g_mu);
}

void cache_fill_free(cache_fill_t *f)
{
    if (f == NULL)
        return;
    free(f->key);
    free(f->fields);
    free(f->body);
    free(f);
}

void cache_reset(void)
{
    pthread_mutex_lock(&g_mu);
    while (g_head != NULL)
        remove_entry(g_head);
    pthread_mutex_unlock(&g_mu);
}
