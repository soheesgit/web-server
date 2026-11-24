/* tests/robust/props.c
 *
 * 성질(property) 검사 + 무작위 입력. 파서와 경로 함수가 어떤 입력에도 헤더 주석의 계약을 지키는지
 * 본다. 기획서 ID 체계 밖의 보조 스위트다 (make robust, docs/TESTING.md는 ID 스위트만 다룬다).
 * 입력은 길이에 딱 맞는 힙 버퍼에 복사해 넘긴다: 길이 밖을 한 바이트라도 읽으면 ASan이 잡는다.
 * 실패하면 입력을 C 이스케이프로 찍고 1로 끝난다. 사용법: props [시드] [반복 배수]
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/http_framing.h"
#include "common/http_headers.h"
#include "common/types.h"
#include "core/buf.h"
#include "core/str.h"
#include "downstream/request_parser.h"
#include "downstream/uri.h"
#include "handler/handler.h"
#include "upstream/response_parser.h"

static uint64_t g_rng = 0x9e3779b97f4a7c15u;
static long g_checks, g_fails;
static int g_mult = 5; /* 기본 5배: ASan 빌드에서도 1초 안쪽 */

/* 역할: xorshift64 난수. 시드가 같으면 같은 입력열을 만든다. */
static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 32);
}

static size_t below(size_t n)
{
    return n == 0 ? 0 : rnd() % n;
}

/* 역할: 실패 입력을 C 이스케이프로 한 줄에 찍는다 (최대 400바이트). */
static void dump(const char *p, size_t n)
{
    fputs("    input: \"", stdout);
    for (size_t i = 0; i < n && i < 400; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c == '\r') {
            fputs("\\r", stdout);
        } else if (c == '\n') {
            fputs("\\n", stdout);
        } else if (c == '"' || c == '\\') {
            printf("\\%c", c);
        } else if (c < 0x20 || c >= 0x7f) {
            printf("\\x%02x", c);
        } else {
            putchar(c);
        }
    }
    printf("\"%s (len %zu)\n", n > 400 ? "..." : "", n);
}

static void fail_at(int line, const char *cond, const char *prop, const char *p, size_t n)
{
    g_fails++;
    if (g_fails <= 25) {
        printf("FAIL %s (props.c:%d): %s\n", prop, line, cond);
        dump(p, n);
    }
}

#define EXPECT(cond, prop, p, n)                                                                   \
    do {                                                                                           \
        g_checks++;                                                                                \
        if (!(cond)) {                                                                             \
            fail_at(__LINE__, #cond, prop, p, n);                                                  \
        }                                                                                          \
    } while (0)

/* 역할: 딱 n바이트짜리 힙 복사본. 파서가 n 밖을 읽으면 ASan heap-buffer-overflow가 난다. */
static char *dup_exact(const char *p, size_t n)
{
    char *c = malloc(n ? n : 1);
    if (c == NULL) {
        abort();
    }
    if (n) {
        memcpy(c, p, n);
    }
    return c;
}

static bool inside(str_t s, const char *base, size_t n)
{
    return s.len == 0 || (s.ptr >= base && s.ptr + s.len <= base + n);
}

static bool has_ctl(str_t s)
{
    for (size_t i = 0; i < s.len; i++) {
        unsigned char c = (unsigned char)s.ptr[i];
        if (c == '\r' || c == '\n' || c == '\0') {
            return true;
        }
    }
    return false;
}

/* 역할: 정규화가 끝난 경로인지 본다: '/' 시작, 빈·"."·".." 세그먼트 없음 (끝의 '/'는 허용). */
static bool path_clean(const char *path)
{
    if (path[0] != '/') {
        return false;
    }
    const char *s = path + 1;
    while (*s) {
        const char *e = strchr(s, '/');
        size_t L = e ? (size_t)(e - s) : strlen(s);
        if ((L == 0 && e) || (L == 1 && s[0] == '.') || (L == 2 && s[0] == '.' && s[1] == '.')) {
            return false;
        }
        if (!e) {
            break;
        }
        s = e + 1;
    }
    return true;
}

/* 역할: 모든 Content-Length 헤더의 모든 항목이 want인지 본다. "5, 5"처럼 같은 값의 목록은
 * 허용한다 (RFC 9110 8.6). 값이 하나라도 다르면 밀반입 경계가 둘이 된다. */
static bool cl_all_equal(const http_headers_t *h, uint64_t want)
{
    for (size_t i = 0; i < h->n; i++) {
        if (!str_eq_ci(h->v[i].name, "content-length")) {
            continue;
        }
        str_t rest = h->v[i].value;
        while (rest.len) {
            const char *comma = memchr(rest.ptr, ',', rest.len);
            size_t L = comma ? (size_t)(comma - rest.ptr) : rest.len;
            uint64_t v;
            if (str_to_u64(str_trim_ows((str_t){rest.ptr, L}), UINT64_MAX, &v) != 0 || v != want) {
                return false;
            }
            rest = comma ? (str_t){comma + 1, rest.len - L - 1} : (str_t){NULL, 0};
        }
    }
    return true;
}

static bool headers_sane(const http_headers_t *h, const char *base, size_t n)
{
    for (size_t i = 0; i < h->n; i++) {
        str_t v = h->v[i].value;
        if (!http_token_valid(h->v[i].name) || !inside(h->v[i].name, base, n) ||
            !inside(v, base, n) || has_ctl(v)) {
            return false;
        }
        if (v.len && (v.ptr[0] == ' ' || v.ptr[0] == '\t' || v.ptr[v.len - 1] == ' ' ||
                      v.ptr[v.len - 1] == '\t')) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ 요청 파서 */

static const http_limits_t LIM = {
    .max_head = 512, .max_uri = 96, .max_headers = 8, .max_body = 1000};

typedef struct {
    http_parse_result_t r;
    int status;
    char sig[HTTP_PATH_MAX + 512]; /* DONE 결과 요약. 다른 버퍼의 결과끼리 내용으로 비교한다 */
} outcome_t;

/* 역할: 요청 하나를 딱 맞는 크기의 버퍼로 파싱하고, DONE이면 불변식을 검사한 뒤 요약을 만든다. */
static void parse_req(const char *p, size_t n, outcome_t *o)
{
    char *c = dup_exact(p, n);
    http_request_t req;
    memset(&req, 0xA5, sizeof req); /* 파서가 채우지 않은 필드를 쓰레기로 드러낸다 */
    o->status = 0;
    o->sig[0] = '\0';
    o->r = request_parse_head(n ? c : NULL, n, &LIM, &req, &o->status);
    if (o->r == HTTP_PARSE_ERROR) {
        int s = o->status;
        EXPECT(s == 400 || s == 413 || s == 414 || s == 431 || s == 501 || s == 505,
               "request: ERROR status in contract list", p, n);
    } else if (o->r == HTTP_PARSE_DONE) {
        const char *prop = "request: DONE invariants";
        size_t hl = req.head_len;
        EXPECT(hl >= 1 && hl <= n, prop, p, n);
        if (hl > n) {
            hl = n;
        }
        EXPECT(req.method == HTTP_GET || req.method == HTTP_HEAD || req.method == HTTP_POST, prop,
               p, n);
        EXPECT(str_eq(req.method_raw, "GET") || str_eq(req.method_raw, "HEAD") ||
                   str_eq(req.method_raw, "POST"),
               prop, p, n);
        EXPECT(req.minor == 0 || req.minor == 1, prop, p, n);
        EXPECT(memchr(req.path, '\0', HTTP_PATH_MAX) != NULL && path_clean(req.path),
               "request: path normalized", p, n);
        EXPECT(inside(req.method_raw, c, hl) && inside(req.target, c, hl) &&
                   inside(req.raw_path, c, hl) && inside(req.query, c, hl) &&
                   inside(req.host, c, hl),
               "request: views inside head", p, n);
        EXPECT(req.headers.n <= LIM.max_headers && headers_sane(&req.headers, c, hl),
               "request: headers well-formed", p, n);
        /* 밀반입 방지: Transfer-Encoding이 있는 요청은 DONE이 되면 안 된다 (D-003: 501). */
        EXPECT(http_header_count(&req.headers, "Transfer-Encoding") == 0,
               "request: no DONE with Transfer-Encoding", p, n);
        EXPECT(req.framing.kind == FRAMING_NONE || req.framing.kind == FRAMING_LENGTH, prop, p, n);
        if (req.framing.kind == FRAMING_LENGTH) {
            EXPECT(req.framing.length <= LIM.max_body, "request: body limit", p, n);
            EXPECT(cl_all_equal(&req.headers, req.framing.length),
                   "request: every Content-Length agrees with framing", p, n);
        } else {
            EXPECT(http_header_count(&req.headers, "Content-Length") == 0,
                   "request: Content-Length ignored", p, n);
        }
        if (req.minor == 1 && req.target.len && req.target.ptr[0] == '/') {
            EXPECT(http_header_count(&req.headers, "Host") == 1, "request: Host exactly once", p,
                   n);
        }
        EXPECT(req.keep_alive == http_keep_alive(req.minor, &req.headers),
               "request: keep_alive matches Connection", p, n);
        snprintf(o->sig, sizeof o->sig, "%d %d %zu|%.*s|%s|%.*s|%.*s|%d %llu %d %zu",
                 (int)req.method, req.minor, req.head_len, (int)req.target.len, req.target.ptr,
                 req.path, (int)req.query.len, req.query.ptr ? req.query.ptr : "",
                 (int)req.host.len, req.host.ptr ? req.host.ptr : "", (int)req.framing.kind,
                 (unsigned long long)req.framing.length, (int)req.keep_alive, req.headers.n);
    }
    free(c);
}

/* 역할: 접두사를 1바이트씩 늘려 가며 파싱해도 결과가 한 번 정해지면 바뀌지 않는지 본다.
 *  - DONE이 나온 뒤에는 모든 더 긴 접두사가 같은 DONE (요약·head_len 같음).
 *  - ERROR가 나온 뒤에는 DONE이 될 수 없다. 상태 코드는 크기 한도(414·431)로 일찍 끊은 경우만 다를
 * 수 있다. */
static void prefix_req(const char *p, size_t n)
{
    static outcome_t first, cur;
    size_t k = 0;
    for (; k <= n; k++) {
        parse_req(p, k, &first);
        if (first.r != HTTP_PARSE_INCOMPLETE) {
            break;
        }
    }
    if (k > n) {
        return;
    }
    for (size_t j = k + 1; j <= n; j++) {
        parse_req(p, j, &cur);
        EXPECT(cur.r == first.r, "request prefix: result never changes once decided", p, j);
        if (cur.r == HTTP_PARSE_DONE && first.r == HTTP_PARSE_DONE) {
            EXPECT(strcmp(cur.sig, first.sig) == 0, "request prefix: same DONE result", p, j);
        }
        if (cur.r == HTTP_PARSE_ERROR && first.status != 414 && first.status != 431) {
            EXPECT(cur.status == first.status, "request prefix: same ERROR status", p, j);
        }
        if (cur.r != first.r) {
            break;
        }
    }
}

static const char *REQ_SEEDS[] = {
    "GET / HTTP/1.1\r\nHost: a\r\n\r\n",
    "GET /a/b/../c?x=1&y=%41 HTTP/1.1\r\nHost: ex.com:8080\r\nConnection: close\r\n\r\n",
    "POST /cgi-bin/echo.py HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\nhello",
    "\r\n\r\nGET /x HTTP/1.0\r\n\r\n",
    "GET http://ex.com:8080/p?q HTTP/1.1\r\nHost: other\r\n\r\n",
    "HEAD /%41%2e%2e/x HTTP/1.1\nHost: a\n\n",
    "GET / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n",
    "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n",
    "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n",
    "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 2\r\ncontent-length: 2\r\n\r\nab",
    "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 1, 1\r\n\r\nx",
    "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: +5\r\n\r\n",
    "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 99999999999999999999999\r\n\r\n",
    "GET / HTTP/1.1\r\nHost: a\r\nX: 1\r\n folded\r\n\r\n",
    "GET / HTTP/1.1\r\nHost : a\r\n\r\n",
    "GET / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding : chunked\r\n\r\n",
    "GET / HTTP/1.1\r\nHost: a\r\nX: a\rb\r\n\r\n",
    "GET /../etc/passwd HTTP/1.1\r\nHost: a\r\n\r\n",
    "GET /%2e%2e/%2e%2e/etc HTTP/1.1\r\nHost: a\r\n\r\n",
    "GET /a%2f..%2f..%2fb HTTP/1.1\r\nHost: a\r\n\r\n",
    "GET /%252e%252e/x HTTP/1.1\r\nHost: a\r\n\r\n",
    "GET /a%00b HTTP/1.1\r\nHost: a\r\n\r\n",
    "GET /a%zz HTTP/1.1\r\nHost: a\r\n\r\n",
    "GET "
    "/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaa"
    "aaaa HTTP/1.1\r\nHost: a\r\n\r\n",
    "PUT / HTTP/1.1\r\nHost: a\r\n\r\n",
    "get / HTTP/1.1\r\nHost: a\r\n\r\n",
    "GET / HTTP/2.0\r\nHost: a\r\n\r\n",
    "GET / HTTP/1.1 \r\nHost: a\r\n\r\n",
    "GET  / HTTP/1.1\r\nHost: a\r\n\r\n",
    "GET * HTTP/1.1\r\nHost: a\r\n\r\n",
    "CONNECT a.com:443 HTTP/1.1\r\nHost: a\r\n\r\n",
    "GET / HTTP/1.1\r\n\r\n",
    "GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n",
    "GET / HTTP/1.1\r\nA: 1\r\nB: 2\r\nC: 3\r\nD: 4\r\nE: 5\r\nF: 6\r\nG: 7\r\nHost: a\r\n\r\n",
    "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\nGET /2 HTTP/1.1\r\nHost: a\r\n\r\n",
    "GET /\xea\xb0\x80?%ZZ HTTP/1.1\r\nHost: a\r\n\r\n",
    "GET /a\\..\\b HTTP/1.1\r\nHost: a\r\n\r\n",
};

static const char INTERESTING[] = " \t\r\n:%/.?#;,\0\x7f\x80\xff"
                                  "0aF\"";

/* 역할: 시드 하나를 무작위로 1~4번 고친다 (바꾸기, 끼우기, 지우기, 조각 복제, 자르기). */
static size_t mutate(const char *seed, size_t n, char *out, size_t cap)
{
    size_t len = n < cap ? n : cap;
    memcpy(out, seed, len);
    int edits = 1 + (int)below(4);
    for (int e = 0; e < edits && len > 0; e++) {
        size_t at = below(len);
        char ch = INTERESTING[below(sizeof INTERESTING - 1)];
        switch (below(5)) {
        case 0:
            out[at] = ch;
            break;
        case 1:
            if (len < cap) {
                memmove(out + at + 1, out + at, len - at);
                out[at] = ch;
                len++;
            }
            break;
        case 2:
            memmove(out + at, out + at + 1, len - at - 1);
            len--;
            break;
        case 3: {
            size_t from = below(len), span = 1 + below(12);
            if (from + span > len) {
                span = len - from;
            }
            if (len + span <= cap) {
                memmove(out + at + span, out + at, len - at);
                memmove(out + at, out + (from >= at ? from + span : from), span);
                len += span;
            }
            break;
        }
        default:
            len = at;
            break;
        }
    }
    return len;
}

static void prop_request(void)
{
    static outcome_t o;
    static char buf[2048];
    size_t nseeds = sizeof REQ_SEEDS / sizeof REQ_SEEDS[0];
    for (size_t i = 0; i < nseeds; i++) {
        prefix_req(REQ_SEEDS[i], strlen(REQ_SEEDS[i]));
    }
    /* "/a%00b"처럼 NUL이 든 시드는 strlen으로 길이를 못 재므로 직접 넣는다. */
    static const char nul_req[] = "GET /a\0b HTTP/1.1\r\nHost: a\r\n\r\n";
    prefix_req(nul_req, sizeof nul_req - 1);
    for (int it = 0; it < 4000 * g_mult; it++) {
        const char *seed = REQ_SEEDS[below(nseeds)];
        size_t n = mutate(seed, strlen(seed), buf, sizeof buf);
        if (it % 8 == 0) {
            prefix_req(buf, n);
        } else {
            parse_req(buf, n, &o);
        }
    }
}

/* ------------------------------------------------------------------ 응답 파서 */

static void parse_resp(const char *p, size_t n, http_method_t m, outcome_t *o)
{
    char *c = dup_exact(p, n);
    http_response_t resp;
    memset(&resp, 0xA5, sizeof resp);
    o->status = 0;
    o->sig[0] = '\0';
    o->r = response_parse_head(n ? c : NULL, n, m, &LIM, &resp, &o->status);
    if (o->r == HTTP_PARSE_ERROR) {
        EXPECT(o->status == 502, "response: ERROR is 502", p, n);
    } else if (o->r == HTTP_PARSE_DONE) {
        size_t hl = resp.head_len <= n ? resp.head_len : n;
        EXPECT(resp.head_len >= 1 && resp.head_len <= n, "response: head_len", p, n);
        EXPECT(resp.status >= 100 && resp.status <= 599, "response: status range", p, n);
        EXPECT(resp.minor == 0 || resp.minor == 1, "response: minor", p, n);
        /* reason 안의 CR·NUL은 검사하지 않는다: 클라이언트 쪽 상태 줄은 response_writer가
         * http_reason(status)으로 새로 만들어 업스트림 reason은 어디에도 전달되지 않는다. */
        EXPECT(inside(resp.reason, c, hl), "response: reason inside head", p, n);
        EXPECT(headers_sane(&resp.headers, c, hl), "response: headers well-formed", p, n);
        bool bodyless =
            m == HTTP_HEAD || resp.status < 200 || resp.status == 204 || resp.status == 304;
        if (bodyless) {
            EXPECT(resp.framing.kind == FRAMING_NONE, "response: bodyless framing NONE", p, n);
        } else {
            /* 밀반입 방지: TE가 있으면 Content-Length로 경계를 잡으면 안 된다. */
            if (http_header_count(&resp.headers, "Transfer-Encoding") > 0) {
                EXPECT(resp.framing.kind != FRAMING_LENGTH, "response: TE beats Content-Length", p,
                       n);
            }
            if (resp.framing.kind == FRAMING_LENGTH) {
                EXPECT(cl_all_equal(&resp.headers, resp.framing.length),
                       "response: every Content-Length agrees", p, n);
            }
        }
        if (resp.framing.kind == FRAMING_CLOSE) {
            EXPECT(!resp.keep_alive, "response: CLOSE framing is not reusable", p, n);
        }
        snprintf(o->sig, sizeof o->sig, "%d %d %zu %d %llu %d %zu", resp.status, resp.minor,
                 resp.head_len, (int)resp.framing.kind, (unsigned long long)resp.framing.length,
                 (int)resp.keep_alive, resp.headers.n);
    }
    free(c);
}

static void prefix_resp(const char *p, size_t n, http_method_t m)
{
    static outcome_t first, cur;
    size_t k = 0;
    for (; k <= n; k++) {
        parse_resp(p, k, m, &first);
        if (first.r != HTTP_PARSE_INCOMPLETE) {
            break;
        }
    }
    for (size_t j = k + 1; j <= n; j++) {
        parse_resp(p, j, m, &cur);
        EXPECT(cur.r == first.r && strcmp(cur.sig, first.sig) == 0,
               "response prefix: result never changes once decided", p, j);
        if (cur.r != first.r) {
            break;
        }
    }
}

static const char *RESP_SEEDS[] = {
    "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello",
    "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n",
    "HTTP/1.0 204 No Content\r\nContent-Length: 7\r\n\r\n",
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 3\r\n\r\n",
    "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\n",
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n",
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked, gzip\r\n\r\n",
    "HTTP/1.1 304 Not Modified\r\nContent-Length: 10\r\n\r\n",
    "HTTP/1.1 200\r\nConnection: close\r\n\r\nbody",
    "HTTP/1.0 200 OK\r\n\r\nuntil-eof",
    "HTTP/1.1 200 OK\nX: y\n\n",
    "HTTP/1.1 999 Weird\r\n\r\n",
    "HTTP/1.1 20 Short\r\n\r\n",
    "HTTP/1.1 200 OK\r\nX: a\r\n b\r\n\r\n",
    "HTTP/1.1 200 OK\r\nBad Name: v\r\n\r\n",
    "ICY 200 OK\r\n\r\n",
    /* 앞 빈 줄 + 머리 뒤 바이트: 접두사가 머리 끝에서 끊길 때와 결과가 같아야 한다.
     * 무작위 변형으로는 드물게만 닿아 시드로 고정한다 (뒤 바이트가 없으면 비교할 접두사가 없다). */
    "\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok",
    "\nHTTP/1.1 204 No Content\r\n\r\nHTTP/1.1 200 OK\r\n",
};

static void prop_response(void)
{
    static outcome_t o;
    static char buf[2048];
    size_t nseeds = sizeof RESP_SEEDS / sizeof RESP_SEEDS[0];
    for (size_t i = 0; i < nseeds; i++) {
        prefix_resp(RESP_SEEDS[i], strlen(RESP_SEEDS[i]), HTTP_GET);
        prefix_resp(RESP_SEEDS[i], strlen(RESP_SEEDS[i]), HTTP_HEAD);
    }
    for (int it = 0; it < 3000 * g_mult; it++) {
        const char *seed = RESP_SEEDS[below(nseeds)];
        size_t n = mutate(seed, strlen(seed), buf, sizeof buf);
        http_method_t m = below(4) == 0 ? HTTP_HEAD : HTTP_GET;
        if (it % 8 == 0) {
            prefix_resp(buf, n, m);
        } else {
            parse_resp(buf, n, m, &o);
        }
    }
}

/* ------------------------------------------------------------------ CGI 응답 머리 */

static const char *const CGI_DROPPED[] = {
    "connection", "transfer-encoding", "keep-alive", "upgrade", "date",
    "server",     "content-length",    "status"};

/* 역할: fields가 "Name: value\r\n" 줄들로만 되어 있고 버려야 할 헤더가 없는지 본다. */
static bool cgi_fields_sane(const buf_t *b)
{
    size_t i = 0;
    while (i < b->len) {
        const char *line = b->data + i;
        const char *end = memchr(line, '\n', b->len - i);
        if (end == NULL || end == line || end[-1] != '\r') {
            return false;
        }
        const char *colon = memchr(line, ':', (size_t)(end - line));
        if (colon == NULL || colon[1] != ' ') {
            return false;
        }
        str_t name = {line, (size_t)(colon - line)};
        if (!http_token_valid(name)) {
            return false;
        }
        for (const char *q = colon; q < end - 1; q++) {
            if (*q == '\r' || *q == '\0') {
                return false; /* 값 안의 CR은 응답 분할로 이어진다 */
            }
        }
        for (size_t d = 0; d < sizeof CGI_DROPPED / sizeof CGI_DROPPED[0]; d++) {
            if (str_eq_ci(name, CGI_DROPPED[d])) {
                return false;
            }
        }
        i = (size_t)(end - b->data) + 1;
    }
    return true;
}

static void parse_cgi(const char *p, size_t n, outcome_t *o)
{
    char *c = dup_exact(p, n);
    http_head_out_t h;
    memset(&h, 0, sizeof h);
    size_t used = 0;
    o->status = 0;
    o->sig[0] = '\0';
    o->r = cgi_parse_head(n ? c : NULL, n, 256, &h, &used, &o->status);
    if (o->r == HTTP_PARSE_ERROR) {
        EXPECT(o->status == 502, "cgi: ERROR is 502", p, n);
    } else if (o->r == HTTP_PARSE_DONE) {
        EXPECT(used >= 1 && used <= n, "cgi: used", p, n);
        EXPECT(h.status >= 100 && h.status <= 599, "cgi: status range", p, n);
        EXPECT(cgi_fields_sane(&h.fields), "cgi: fields are clean header lines", p, n);
        snprintf(o->sig, sizeof o->sig, "%d %zu %d %llu %.*s", h.status, used, (int)h.framing.kind,
                 (unsigned long long)h.framing.length, (int)h.fields.len,
                 h.fields.data ? h.fields.data : "");
    }
    buf_cleanup(&h.fields);
    free(c);
}

static void prefix_cgi(const char *p, size_t n)
{
    static outcome_t first, cur;
    size_t k = 0;
    for (; k <= n; k++) {
        parse_cgi(p, k, &first);
        if (first.r != HTTP_PARSE_INCOMPLETE) {
            break;
        }
    }
    for (size_t j = k + 1; j <= n; j++) {
        parse_cgi(p, j, &cur);
        EXPECT(cur.r == first.r && strcmp(cur.sig, first.sig) == 0,
               "cgi prefix: result never changes once decided", p, j);
        if (cur.r != first.r) {
            break;
        }
    }
}

static const char *CGI_SEEDS[] = {
    "Content-Type: text/plain\n\nbody",
    "Status: 404 Not Found\r\nContent-Type: text/html\r\n\r\n",
    "Location: /x\n\n",
    "Content-Length: 3\nX-A: b\n\nabc",
    "Status: 200\nConnection: close\nTransfer-Encoding: chunked\nDate: x\nServer: y\nX-Ok: 1\n\n",
    "X-A: one\rX-B: two\n\n",
    "Status: 2000\n\n",
    "Status: abc\n\n",
    "NoColon\n\n",
    "Bad Name: v\n\n",
    "X-Empty:\n\n",
    "Content-Length: -1\n\n",
    "Content-Length: 1\nContent-Length: 2\n\n",
};

static void prop_cgi(void)
{
    static outcome_t o;
    static char buf[1024];
    size_t nseeds = sizeof CGI_SEEDS / sizeof CGI_SEEDS[0];
    for (size_t i = 0; i < nseeds; i++) {
        prefix_cgi(CGI_SEEDS[i], strlen(CGI_SEEDS[i]));
    }
    for (int it = 0; it < 3000 * g_mult; it++) {
        const char *seed = CGI_SEEDS[below(nseeds)];
        size_t n = mutate(seed, strlen(seed), buf, sizeof buf);
        if (it % 8 == 0) {
            prefix_cgi(buf, n);
        } else {
            parse_cgi(buf, n, &o);
        }
    }
}

/* ------------------------------------------------------------------ path_normalize */

/* 역할: 세그먼트 스택으로 짠 참조 구현. path_normalize 헤더 주석의 규칙을 그대로 옮겼다. */
static int ref_normalize(const char *in, char *out)
{
    const char *seg[HTTP_PATH_MAX];
    size_t segl[HTTP_PATH_MAX];
    size_t n = 0;
    bool trailing = false;
    if (in[0] != '/') {
        return -1;
    }
    const char *s = in;
    while (*s) {
        while (*s == '/') {
            s++;
        }
        if (*s == '\0') {
            trailing = true;
            break;
        }
        const char *e = s;
        while (*e && *e != '/') {
            e++;
        }
        size_t L = (size_t)(e - s);
        if (L == 1 && s[0] == '.') {
            trailing = trailing || *e == '\0';
        } else if (L == 2 && s[0] == '.' && s[1] == '.') {
            if (n == 0) {
                return -1;
            }
            n--;
            trailing = trailing || *e == '\0';
        } else {
            seg[n] = s;
            segl[n++] = L;
        }
        s = e;
    }
    char *o = out;
    *o++ = '/';
    for (size_t i = 0; i < n; i++) {
        memcpy(o, seg[i], segl[i]);
        o += segl[i];
        if (i + 1 < n || trailing) {
            *o++ = '/';
        }
    }
    *o = '\0';
    return 0;
}

static const char *const PATH_TOK[] = {"a", "b", "..", ".", "/", "//", "...", ".a", "a.", "%2e"};

static void check_normalize(const char *in)
{
    static char want[HTTP_PATH_MAX];
    size_t n = strlen(in);
    char *got = dup_exact(in, n + 1); /* 제자리 수정: 크기가 딱 맞아야 넘쳐 쓰기를 잡는다 */
    int wr = ref_normalize(in, want);
    int gr = path_normalize(got);
    EXPECT(gr == wr, "path_normalize: return code matches reference", in, n);
    if (gr == 0 && wr == 0) {
        EXPECT(strcmp(got, want) == 0, "path_normalize: result matches reference", in, n);
        EXPECT(strlen(got) <= n, "path_normalize: never grows", in, n);
        char *again = dup_exact(got, strlen(got) + 1);
        EXPECT(path_normalize(again) == 0 && strcmp(again, got) == 0, "path_normalize: idempotent",
               in, n);
        free(again);
    }
    free(got);
}

static void prop_normalize(void)
{
    static char in[HTTP_PATH_MAX];
    for (int it = 0; it < 60000 * g_mult; it++) {
        size_t len = 0;
        if (below(16) != 0) {
            in[len++] = '/';
        }
        int toks = (int)below(10);
        for (int t = 0; t < toks; t++) {
            const char *tok = PATH_TOK[below(sizeof PATH_TOK / sizeof PATH_TOK[0])];
            size_t tl = strlen(tok);
            memcpy(in + len, tok, tl);
            len += tl;
            if (below(3) != 0) {
                in[len++] = '/';
            }
        }
        in[len] = '\0';
        check_normalize(in);
    }
    /* 버퍼 끝 근처: HTTP_PATH_MAX - 1 바이트짜리 경로 */
    size_t len = 0;
    while (len + 2 < HTTP_PATH_MAX) {
        in[len++] = '/';
        in[len++] = below(2) ? 'a' : '.';
    }
    in[len] = '\0';
    check_normalize(in);
}

/* ------------------------------------------------------------------ uri_decode_path */

static int hexv(int c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* 역할: 참조 디코더. 잘못된 %XX나 %00이면 -1. */
static int ref_decode(const char *in, size_t n, char *out, size_t *out_len)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '%') {
            if (i + 2 >= n) {
                return -1;
            }
            int hi = hexv(in[i + 1]), lo = hexv(in[i + 2]);
            if (hi < 0 || lo < 0 || (hi == 0 && lo == 0)) {
                return -1;
            }
            out[o++] = (char)(hi * 16 + lo);
            i += 2;
        } else {
            out[o++] = (char)c;
        }
    }
    *out_len = o;
    return 0;
}

static void prop_decode(void)
{
    static char raw[256], enc[1024], got[1024], want[1024];
    static const char HEX_U[] = "0123456789ABCDEF", HEX_L[] = "0123456789abcdef";
    for (int it = 0; it < 30000 * g_mult; it++) {
        /* 1) 왕복: 임의 바이트(1~255)를 일부만 %XX로 바꿔도 디코딩하면 원래대로 돌아온다. */
        size_t m = below(64), e = 0;
        for (size_t i = 0; i < m; i++) {
            unsigned char c = (unsigned char)(1 + below(255));
            raw[i] = (char)c;
            bool plain = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' || c == '.';
            if (plain && below(2)) {
                enc[e++] = (char)c;
            } else {
                const char *hx = below(2) ? HEX_U : HEX_L;
                enc[e++] = '%';
                enc[e++] = hx[c >> 4];
                enc[e++] = hx[c & 15];
            }
        }
        char *ce = dup_exact(enc, e); /* str_t는 NUL 종료가 아니다: 끝 너머를 읽으면 잡힌다 */
        size_t olen = 0;
        int r = uri_decode_path((str_t){ce, e}, got, m + 1, &olen);
        EXPECT(r == 0 && olen == m && memcmp(got, raw, m) == 0 && got[m] == '\0',
               "uri_decode_path: roundtrip", enc, e);
        EXPECT(uri_decode_path((str_t){ce, e}, got, m, &olen) == -1,
               "uri_decode_path: cap without room for NUL fails", enc, e);
        free(ce);

        /* 2) 쓰레기 입력: 참조 디코더와 결과가 같다. */
        static const char ALPH[] = "%%%0aFg2/.";
        size_t n = below(24);
        for (size_t i = 0; i < n; i++) {
            enc[i] = ALPH[below(sizeof ALPH - 1)];
        }
        ce = dup_exact(enc, n);
        size_t wl = 0;
        int wr = ref_decode(enc, n, want, &wl);
        r = uri_decode_path((str_t){ce, n}, got, sizeof got, &olen);
        EXPECT(r == wr, "uri_decode_path: accepts exactly what the reference accepts", enc, n);
        if (r == 0 && wr == 0) {
            EXPECT(olen == wl && memcmp(got, want, wl) == 0, "uri_decode_path: same bytes", enc, n);
        }
        free(ce);
    }
}

/* ------------------------------------------------------------------ chunked 본문 추적 */

/* 역할: 유효한 chunked 본문을 만든다 (16진 대소문자, 앞자리 0, 확장, 트레일러, LF 단독 섞음). */
static size_t gen_chunked(char *out)
{
    size_t n = 0;
    int chunks = (int)below(5);
    for (int c = 0; c <= chunks; c++) {
        size_t sz = c == chunks ? 0 : 1 + below(40);
        int zeros = (int)below(3);
        for (int z = 0; z < zeros; z++) {
            out[n++] = '0';
        }
        n += (size_t)sprintf(out + n, below(2) ? "%zx" : "%zX", sz);
        if (below(3) == 0) {
            n += (size_t)sprintf(out + n, ";ext=\"v %d\"", (int)below(100));
        }
        n += (size_t)sprintf(out + n, below(4) ? "\r\n" : "\n");
        for (size_t i = 0; i < sz; i++) {
            out[n++] = (char)below(256);
        }
        if (sz) {
            n += (size_t)sprintf(out + n, below(4) ? "\r\n" : "\n");
        }
    }
    int trailers = (int)below(3);
    for (int t = 0; t < trailers; t++) {
        n += (size_t)sprintf(out + n, "X-T%d: v\tw%s", t, below(4) ? "\r\n" : "\n");
    }
    n += (size_t)sprintf(out + n, below(4) ? "\r\n" : "\n");
    return n;
}

/* 역할: 본문을 pieces 지점에서 나눠 넣고 (결과, 쓴 바이트 합)을 돌려준다. MORE면 used == 조각 길이.
 */
static http_body_result_t feed(const char *p, size_t n, const size_t *cuts, size_t ncuts,
                               size_t *total, bool *more_ok)
{
    http_body_tracker_t t;
    http_body_tracker_init(&t, (http_framing_t){FRAMING_CHUNKED, 0});
    size_t at = 0;
    http_body_result_t r = BODY_MORE;
    *total = 0;
    *more_ok = true;
    for (size_t k = 0; k <= ncuts; k++) {
        size_t end = k < ncuts ? cuts[k] : n;
        if (end < at) {
            continue;
        }
        size_t used = 0, len = end - at;
        char *c = dup_exact(p + at, len);
        r = http_body_track(&t, len ? c : NULL, len, &used);
        free(c);
        *total += used;
        if (r == BODY_MORE && used != len) {
            *more_ok = false;
        }
        if (r != BODY_MORE) {
            break;
        }
        at = end;
    }
    return r;
}

static int cmp_size(const void *a, const void *b)
{
    size_t x = *(const size_t *)a, y = *(const size_t *)b;
    return x < y ? -1 : x > y;
}

static void prop_chunked(void)
{
    static char body[4096];
    static size_t cuts[4096];
    for (int it = 0; it < 6000 * g_mult; it++) {
        size_t blen = gen_chunked(body);
        bool mutated = below(3) == 0;
        if (mutated) {
            body[below(blen)] = INTERESTING[below(sizeof INTERESTING - 1)];
        }
        static const char NEXT[] = "GET /next HTTP/1.1\r\n\r\n"; /* 파이프라인 다음 메시지 */
        memcpy(body + blen, NEXT, sizeof NEXT - 1);
        size_t n = blen + sizeof NEXT - 1;
        if (below(5) == 0) {
            n = below(n + 1); /* 아직 덜 온 본문 */
        }

        size_t whole = 0;
        bool ok = true;
        http_body_result_t rw = feed(body, n, NULL, 0, &whole, &ok);
        if (!mutated && n >= blen) {
            EXPECT(rw == BODY_DONE && whole == blen,
                   "chunked: valid body ends exactly at its last CRLF", body, n);
        }
        size_t nc = below(8);
        for (size_t k = 0; k < nc; k++) {
            cuts[k] = below(n + 1);
        }
        qsort(cuts, nc, sizeof cuts[0], cmp_size);
        size_t split = 0;
        http_body_result_t rs = feed(body, n, cuts, nc, &split, &ok);
        EXPECT(rs == rw && split == whole && ok, "chunked: random split gives same result", body,
               n);
        if (it % 16 == 0) {
            for (size_t k = 0; k < n; k++) {
                cuts[k] = k + 1;
            }
            rs = feed(body, n, cuts, n, &split, &ok);
            EXPECT(rs == rw && split == whole && ok, "chunked: byte-by-byte gives same result",
                   body, n);
        }
    }
}

/* ------------------------------------------------------------------ buf (append·consume·shrink) */

/* 역할: 무작위 append·consume·shrink 열을 단순 모형(배열)과 견준다. 내용·len이 모형과 같고,
 * shrink가 내용을 해치지 않으며 용량을 max(len, keep)까지만 줄이는지 본다 (D-043). */
static void prop_buf(void)
{
    static char model[70000];
    static char chunk[3000];
    for (int it = 0; it < 300 * g_mult; it++) {
        buf_t b;
        size_t max = 1 + below(65536), mlen = 0;
        EXPECT(buf_init(&b, below(64), max) == 0, "buf: init", "", 0);
        for (int op = 0; op < 60; op++) {
            size_t n, cap_before = b.cap;
            switch (below(3)) {
            case 0:
                n = below(sizeof chunk);
                for (size_t i = 0; i < n; i++) {
                    chunk[i] = (char)below(256);
                }
                EXPECT((buf_append(&b, chunk, n) == 0) == (mlen + n <= max),
                       "buf: append succeeds exactly when it fits max", "", 0);
                if (mlen + n <= max) {
                    memcpy(model + mlen, chunk, n);
                    mlen += n;
                }
                break;
            case 1:
                n = below(mlen + 5);
                buf_consume(&b, n);
                if (n >= mlen) {
                    mlen = 0;
                } else {
                    memmove(model, model + n, mlen - n);
                    mlen -= n;
                }
                break;
            default: {
                size_t keep = below(5000), want = mlen > keep ? mlen : keep;
                buf_shrink(&b, keep);
                if (b.data == NULL) {
                    break;
                }
                if (want == 0) {
                    want = 1;
                }
                EXPECT(b.cap == (cap_before > want ? want : cap_before),
                       "buf: shrink lowers cap to max(len, keep) and never raises it", "", 0);
                break;
            }
            }
            EXPECT(b.len == mlen && b.max == max && b.len <= b.cap && (b.cap <= max || b.cap == 0),
                   "buf: len, max and cap stay consistent", "", 0);
            EXPECT(mlen == 0 || memcmp(b.data, model, mlen) == 0,
                   "buf: contents equal the model after every operation", "", 0);
        }
        buf_cleanup(&b);
    }
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        g_rng = strtoull(argv[1], NULL, 0) | 1;
    }
    if (argc > 2) {
        g_mult = atoi(argv[2]) > 0 ? atoi(argv[2]) : 1;
    }
    printf("props: seed 0x%llx, x%d\n", (unsigned long long)g_rng, g_mult);
    struct {
        const char *name;
        void (*fn)(void);
    } props[] = {
        {"request_parse_head", prop_request},
        {"response_parse_head", prop_response},
        {"cgi_parse_head", prop_cgi},
        {"path_normalize", prop_normalize},
        {"uri_decode_path", prop_decode},
        {"http_body_track(chunked)", prop_chunked},
        {"buf (append·consume·shrink)", prop_buf},
    };
    for (size_t i = 0; i < sizeof props / sizeof props[0]; i++) {
        long before_c = g_checks, before_f = g_fails;
        props[i].fn();
        printf("%-26s %8ld checks  %s\n", props[i].name, g_checks - before_c,
               g_fails == before_f ? "ok" : "FAIL");
    }
    printf("props: %ld checks, %ld failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
