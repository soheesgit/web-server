/* tests/unit/contract_min/test_common.c
 *
 * common/ 계약 최소셋. 기대값의 원래 표: 기획서 8장. ID 목록: src/common/README.md
 */
#include "unit/testlib.h"

#include <string.h>

#include "common/http_framing.h"
#include "common/http_headers.h"
#include "common/types.h"

/* str_from_cstr(스텁일 수 있음)에 기대지 않고 헤더를 직접 채운다. */
static void add_hdr(http_headers_t *h, const char *name, const char *value)
{
    h->v[h->n].name.ptr = name;
    h->v[h->n].name.len = strlen(name);
    h->v[h->n].value.ptr = value;
    h->v[h->n].value.len = strlen(value);
    h->n++;
}

static str_t S(const char *s)
{
    str_t r = {s, strlen(s)};
    return r;
}

static const char REQ1[] = "GET / HTTP/1.1\r\nHost: a\r\n\r\n";

static void t_u_com_01(void)
{
    size_t skip = 99, head_len = 0;
    int st = 0;

    CHECK_EQ_INT(http_find_head_end(REQ1, strlen(REQ1), 8192, &skip, &head_len, &st),
                 HTTP_PARSE_DONE);
    CHECK_EQ_INT(skip, 0);
    CHECK_EQ_INT(head_len, strlen(REQ1));

    const char mixed[] = "\r\n\nGET / HTTP/1.0\n\r\nBODY";
    const size_t end = sizeof(mixed) - sizeof("BODY");
    CHECK_EQ_INT(http_find_head_end(mixed, sizeof(mixed) - 1, end, &skip, &head_len, &st),
                 HTTP_PARSE_DONE);
    CHECK_EQ_INT(skip, 3);
    CHECK_EQ_INT(head_len, end);
    CHECK_EQ_INT(http_find_head_end(mixed, sizeof(mixed) - 1, end - 1, &skip, &head_len, &st),
                 HTTP_PARSE_ERROR);
    CHECK_EQ_INT(st, 431);
}

static void t_u_com_02(void)
{
    size_t len = strlen(REQ1);
    size_t skip = 0, head_len = 0;
    int st = 0;

    for (size_t k = 1; k < len; k++) {
        http_parse_result_t r = http_find_head_end(REQ1, k, 8192, &skip, &head_len, &st);
        if (r != HTTP_PARSE_INCOMPLETE) {
            test_fail(__FILE__, __LINE__, "prefix of %zu bytes: expected INCOMPLETE, got %d", k,
                      (int)r);
            return;
        }
    }
    CHECK_EQ_INT(http_find_head_end(REQ1, len, 8192, &skip, &head_len, &st), HTTP_PARSE_DONE);
    CHECK_EQ_INT(head_len, len);
}

static void t_u_com_04(void)
{
    static char big[9000];
    size_t n = 0;
    size_t skip = 0, head_len = 0;
    int st = 0;
    const char *first = "GET / HTTP/1.1\r\n";

    memcpy(big, first, strlen(first));
    n = strlen(first);
    while (n + 64 < sizeof(big)) {
        memcpy(big + n, "X-Pad: ", 7);
        n += 7;
        memset(big + n, 'a', 50);
        n += 50;
        memcpy(big + n, "\r\n", 2);
        n += 2;
    }
    CHECK(n > 8192);
    CHECK_EQ_INT(http_find_head_end(big, n, 8192, &skip, &head_len, &st), HTTP_PARSE_ERROR);
    CHECK_EQ_INT(st, 431);
}

static void t_u_com_07(void)
{
    const char *p = "A: 1\r\nB: two\r\nC: 3\r\n";
    http_headers_t h;
    int st = 0;

    memset(&h, 0, sizeof(h));
    CHECK_EQ_INT(http_parse_headers(p, strlen(p), 64, &h, &st), 0);
    CHECK_EQ_INT(h.n, 3);
    CHECK_STR_EQ(h.v[0].name, "A");
    CHECK_STR_EQ(h.v[0].value, "1");
    CHECK_STR_EQ(h.v[1].name, "B");
    CHECK_STR_EQ(h.v[1].value, "two");
    CHECK_STR_EQ(h.v[2].name, "C");
    CHECK_STR_EQ(h.v[2].value, "3");

    const char invalid[] = "X-Value: a\0b\r\n";
    CHECK_EQ_INT(http_parse_headers(invalid, sizeof(invalid) - 1, 64, &h, &st), -1);
    CHECK_EQ_INT(st, 400);
    CHECK_EQ_INT(http_parse_headers(p, strlen(p), 2, &h, &st), -1);
    CHECK_EQ_INT(st, 431);
}

static void t_u_com_10(void)
{
    const char *p = "Host : a\r\n";
    http_headers_t h;
    int st = 0;

    CHECK_EQ_INT(http_parse_headers(p, strlen(p), 64, &h, &st), -1);
    CHECK_EQ_INT(st, 400);
}

static void t_u_com_18(void)
{
    http_headers_t h = {0};
    http_framing_t f = {FRAMING_CLOSE, 0};
    int st = 0;

    add_hdr(&h, "Content-Length", "15");
    CHECK_EQ_INT(http_resolve_framing(&h, false, 0, HTTP_POST, &f, &st), 0);
    CHECK_EQ_INT(f.kind, FRAMING_LENGTH);
    CHECK_EQ_INT(f.length, 15);
    add_hdr(&h, "content-length", "15, 015");
    CHECK_EQ_INT(http_resolve_framing(&h, false, 0, HTTP_POST, &f, &st), 0);
    CHECK_EQ_INT(f.length, 15);
    add_hdr(&h, "Content-Length", "16");
    CHECK_EQ_INT(http_resolve_framing(&h, false, 0, HTTP_POST, &f, &st), -1);
    CHECK_EQ_INT(st, 400);
    h.v[2].value = S("18446744073709551616");
    CHECK_EQ_INT(http_resolve_framing(&h, false, 0, HTTP_POST, &f, &st), -1);
}

static void t_u_com_21(void)
{
    http_headers_t h = {0};
    http_framing_t f = {FRAMING_NONE, 0};
    int st = 0;

    add_hdr(&h, "Transfer-Encoding", "chunked");
    add_hdr(&h, "Content-Length", "5");
    CHECK_EQ_INT(http_resolve_framing(&h, false, 0, HTTP_POST, &f, &st), -1);
    CHECK_EQ_INT(st, 400);
    CHECK_EQ_INT(http_resolve_framing(&h, true, 200, HTTP_GET, &f, &st), -1);
    CHECK_EQ_INT(st, 502);

    h.n = 1;
    CHECK_EQ_INT(http_resolve_framing(&h, false, 0, HTTP_POST, &f, &st), 0);
    CHECK_EQ_INT(f.kind, FRAMING_CHUNKED);
    add_hdr(&h, "Transfer-Encoding", "chunked");
    CHECK_EQ_INT(http_resolve_framing(&h, true, 200, HTTP_GET, &f, &st), -1);
    CHECK_EQ_INT(st, 502);
    h.n = 1;
    h.v[0].value = S("gzip, chunked");
    CHECK_EQ_INT(http_resolve_framing(&h, true, 200, HTTP_GET, &f, &st), 0);
    CHECK_EQ_INT(f.kind, FRAMING_CHUNKED);
    h.v[0].value = S("gzip,,chunked");
    CHECK_EQ_INT(http_resolve_framing(&h, true, 200, HTTP_GET, &f, &st), -1);
}

static void t_u_com_26(void)
{
    http_headers_t h = {0};
    http_framing_t f = {FRAMING_NONE, 0};
    int st = 0;

    add_hdr(&h, "Content-Length", "100");
    CHECK_EQ_INT(http_resolve_framing(&h, true, 200, HTTP_GET, &f, &st), 0);
    CHECK_EQ_INT(f.kind, FRAMING_LENGTH);
    CHECK_EQ_INT(f.length, 100);
}

static void t_u_com_29(void)
{
    const char data[] = "abc\0defghiNEXT";
    http_body_tracker_t t;
    http_framing_t f = {FRAMING_LENGTH, 10};
    size_t used = 0;

    memset(&t, 0, sizeof(t));
    http_body_tracker_init(&t, f);
    CHECK_EQ_INT(http_body_track(&t, data, 4, &used), BODY_MORE);
    CHECK_EQ_INT(used, 4);
    CHECK_EQ_INT(http_body_track(&t, NULL, 0, &used), BODY_MORE);
    CHECK_EQ_INT(t.remaining, 6);
    CHECK_EQ_INT(http_body_track(&t, data + 4, sizeof(data) - 5, &used), BODY_DONE);
    CHECK_EQ_INT(used, 6);
}

static void t_u_com_32(void)
{
    const char *p = "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
    http_body_tracker_t t;
    http_framing_t f = {FRAMING_CHUNKED, 0};
    size_t used = 0;

    memset(&t, 0, sizeof(t));
    http_body_tracker_init(&t, f);
    CHECK_EQ_INT(http_body_track(&t, p, strlen(p), &used), BODY_DONE);
    CHECK_EQ_INT(used, strlen(p));
}

static void t_u_com_33(void)
{
    static const char msg[] = "3;tag=x\r\na\0b\r\n0\r\nX-Trace: ok\r\n\r\nNEXT";
    static const char *const bad[] = {
        "10000000000000000\r\n",
        "1\r\na\rX",
        "1 junk\r\n",
        "0\r\nNo-Colon\r\n\r\n",
    };
    const size_t body_len = sizeof(msg) - sizeof("NEXT");
    http_body_tracker_t t;
    http_framing_t f = {FRAMING_CHUNKED, 0};
    size_t used;

    for (size_t split = 0; split < body_len; split++) {
        http_body_tracker_init(&t, f);
        CHECK_EQ_INT(http_body_track(&t, msg, split, &used), BODY_MORE);
        CHECK_EQ_INT(used, split);
        CHECK_EQ_INT(http_body_track(&t, NULL, 0, &used), BODY_MORE);
        CHECK_EQ_INT(used, 0);
        CHECK_EQ_INT(http_body_track(&t, msg + split, sizeof(msg) - 1 - split, &used), BODY_DONE);
        CHECK_EQ_INT(used, body_len - split);
    }
    http_body_tracker_init(&t, f);
    for (size_t i = 0; i < body_len; i++) {
        CHECK_EQ_INT(http_body_track(&t, msg + i, 1, &used),
                     i == body_len - 1 ? BODY_DONE : BODY_MORE);
        CHECK_EQ_INT(used, 1);
    }
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        http_body_tracker_init(&t, f);
        CHECK_EQ_INT(http_body_track(&t, bad[i], strlen(bad[i]), &used), BODY_ERROR);
    }
}

static void t_u_com_38(void)
{
    http_headers_t none = {0}, c1 = {0}, c2 = {0}, c3 = {0};

    add_hdr(&c1, "Connection", "close");
    add_hdr(&c2, "Connection", "Close");
    add_hdr(&c3, "Connection", "keep-alive, close");
    CHECK(http_keep_alive(1, &none) == true);
    CHECK(http_keep_alive(1, &c1) == false);
    CHECK(http_keep_alive(1, &c2) == false);
    CHECK(http_keep_alive(1, &c3) == false);
}

static void t_u_com_40(void)
{
    static const char *hop[] = {"connection",
                                "KEEP-ALIVE",
                                "Proxy-Connection",
                                "te",
                                "Trailer",
                                "transfer-Encoding",
                                "UPGRADE",
                                "proxy-authenticate",
                                "Proxy-Authorization"};

    for (size_t i = 0; i < sizeof(hop) / sizeof(hop[0]); i++) {
        if (!http_is_hop_by_hop(S(hop[i]), NULL)) {
            test_fail(__FILE__, __LINE__, "\"%s\" should be hop-by-hop", hop[i]);
            return;
        }
    }
    CHECK(http_is_hop_by_hop(S("Content-Type"), NULL) == false);

    http_headers_t h = {0};
    buf_t out;
    const char expected[] = "Content-Type: text/plain\r\n";
    add_hdr(&h, "X-Intra", "a");
    add_hdr(&h, "Connection", " X-Other , x-intra ");
    add_hdr(&h, "Content-Type", "text/plain");
    add_hdr(&h, "Connection", "close");
    add_hdr(&h, "X-Other", "b");
    CHECK(http_is_hop_by_hop(S("X-Intra"), &h));
    CHECK_EQ_INT(buf_init(&out, 32, 128), 0);
    CHECK_EQ_INT(http_headers_write(&h, true, &out), 0);
    CHECK_EQ_MEM(out.data, out.len, expected, sizeof(expected) - 1);
    buf_cleanup(&out);
}

void register_common_tests(void)
{
    test_register("U-COM-01", "http_find_head_end: whole request at once", t_u_com_01);
    test_register("U-COM-02", "http_find_head_end: byte by byte", t_u_com_02);
    test_register("U-COM-04", "http_find_head_end: over max_head is 431", t_u_com_04);
    test_register("U-COM-07", "http_parse_headers: three headers in order", t_u_com_07);
    test_register("U-COM-10", "http_parse_headers: space before colon is 400", t_u_com_10);
    test_register("U-COM-18", "http_resolve_framing: request Content-Length 15", t_u_com_18);
    test_register("U-COM-21", "http_resolve_framing: TE + CL is 400", t_u_com_21);
    test_register("U-COM-26", "http_resolve_framing: response CL 100", t_u_com_26);
    test_register("U-COM-29", "http_body_track: LENGTH 10 with 10 bytes", t_u_com_29);
    test_register("U-COM-32", "http_body_track: chunked body to the end", t_u_com_32);
    test_register("U-COM-33", "http_body_track: fragmented chunked body", t_u_com_33);
    test_register("U-COM-38", "http_keep_alive: HTTP/1.1 Connection variants", t_u_com_38);
    test_register("U-COM-40", "http_is_hop_by_hop: fixed list, mixed case", t_u_com_40);
}
