/* src/common/http_framing.c
 *
 * 본문 길이·경계 추적·keep-alive. 명세: http_framing.h
 */
#include "common/http_framing.h"
#include "core/stub.h"

#include <stdint.h>

#include "common/http_headers.h"
#include "core/str.h"

/* v를 쉼표로 나눈 i번째 항목(OWS 제거)을 out에 넣는다. 항목이 없으면 false. */
static bool list_item(str_t v, size_t idx, str_t *out)
{
    size_t start = 0;
    size_t n = 0;

    for (size_t k = 0; k <= v.len; k++) {
        if (k == v.len || v.ptr[k] == ',') {
            if (n == idx) {
                str_t item = {v.ptr + start, k - start};
                *out = str_trim_ows(item);
                return true;
            }
            n++;
            start = k + 1;
        }
    }
    return false;
}

/* 모든 Content-Length 값(쉼표 목록 포함)이 같은 10진수인지 확인한다. */
static int resolve_content_length(const http_headers_t *h, uint64_t *out_len)
{
    bool have = false;
    uint64_t want = 0;

    for (size_t i = 0; i < h->n; i++) {
        str_t item;

        if (!str_eq_ci(h->v[i].name, "Content-Length")) {
            continue;
        }
        for (size_t idx = 0; list_item(h->v[i].value, idx, &item); idx++) {
            uint64_t v;
            if (str_to_u64(item, UINT64_MAX, &v) != 0) {
                return -1;
            }
            if (have && v != want) {
                return -1;
            }
            have = true;
            want = v;
        }
    }
    *out_len = want;
    return have ? 0 : -1;
}

/* 마지막 Transfer-Encoding 헤더의 마지막 코딩이 chunked인가 */
static bool te_ends_with_chunked(const http_headers_t *h)
{
    const http_header_t *last = NULL;
    str_t item = {NULL, 0};
    str_t found = {NULL, 0};

    for (size_t i = 0; i < h->n; i++) {
        if (str_eq_ci(h->v[i].name, "Transfer-Encoding")) {
            last = &h->v[i];
        }
    }
    if (last == NULL) {
        return false;
    }
    for (size_t idx = 0; list_item(last->value, idx, &item); idx++) {
        found = item;
    }
    return str_eq_ci(found, "chunked");
}

int http_resolve_framing(const http_headers_t *h, bool is_response, int status,
                         http_method_t req_method, http_framing_t *out_f, int *out_status)
{
    int err = is_response ? 502 : 400;
    size_t n_te = http_header_count(h, "Transfer-Encoding");
    size_t n_cl = http_header_count(h, "Content-Length");

    if (is_response && (req_method == HTTP_HEAD || (status >= 100 && status < 200) ||
                        status == 204 || status == 304)) {
        out_f->kind = FRAMING_NONE;
        out_f->length = 0;
        return 0;
    }
    if (n_te > 0 && n_cl > 0) {
        *out_status = err; /* request smuggling 방지 */
        return -1;
    }
    if (n_te > 0) {
        if (!te_ends_with_chunked(h)) {
            *out_status = err;
            return -1;
        }
        out_f->kind = FRAMING_CHUNKED;
        out_f->length = 0;
        return 0;
    }
    if (n_cl > 0) {
        uint64_t len;
        if (resolve_content_length(h, &len) != 0) {
            *out_status = err;
            return -1;
        }
        out_f->kind = FRAMING_LENGTH;
        out_f->length = len;
        return 0;
    }
    out_f->kind = is_response ? FRAMING_CLOSE : FRAMING_NONE;
    out_f->length = 0;
    return 0;
}

void http_body_tracker_init(http_body_tracker_t *t, http_framing_t f)
{
    UNUSED(t);
    UNUSED(f);
    /* TODO(common): 구현 — U-COM-29, U-COM-32 */
    STUB_VOID();
}

http_body_result_t http_body_track(http_body_tracker_t *t, const char *p, size_t len,
                                   size_t *out_used)
{
    UNUSED(t);
    UNUSED(p);
    UNUSED(len);
    UNUSED(out_used);
    /* TODO(common): 구현 — U-COM-29, U-COM-32, E-PX-05 */
    STUB_RETURN(BODY_ERROR);
}

bool http_keep_alive(int minor, const http_headers_t *h)
{
    UNUSED(minor);
    UNUSED(h);
    /* TODO(common): 구현 — U-COM-38, E-KA-03, E-KA-04, E-KA-05 */
    STUB_RETURN(false);
}
