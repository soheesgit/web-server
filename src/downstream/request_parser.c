#include "downstream/request_parser.h"

#include "common/http_framing.h"
#include "common/http_headers.h"
#include "downstream/uri.h"

#include <string.h>

static http_parse_result_t reject(int status, int *out_status)
{
    *out_status = status;
    return HTTP_PARSE_ERROR;
}

static bool request_line_too_long(const char *p, size_t len, size_t max_uri)
{
    size_t start = 0;
    while (start < len) {
        if (p[start] == '\n') {
            start++;
        } else if (len - start >= 2 && p[start] == '\r' && p[start + 1] == '\n') {
            start += 2;
        } else {
            break;
        }
    }
    if (len - start <= max_uri || len - start - max_uri <= 16) {
        return false;
    }
    const char *line = p + start;
    const char *newline = memchr(line, '\n', len - start);
    size_t line_len = newline == NULL ? len - start : (size_t)(newline - line);
    return line_len > max_uri && line_len - max_uri > 16;
}

static bool split_request_line(str_t line, str_t *method, str_t *target, str_t *version)
{
    const char *end = line.ptr + line.len;
    const char *first = memchr(line.ptr, ' ', line.len);
    if (first == NULL || first == line.ptr) {
        return false;
    }
    const char *second = memchr(first + 1, ' ', (size_t)(end - first - 1));
    if (second == NULL || second == first + 1 || second + 1 == end ||
        memchr(second + 1, ' ', (size_t)(end - second - 1)) != NULL) {
        return false;
    }
    for (const char *at = line.ptr; at < end; at++) {
        unsigned char ch = (unsigned char)*at;
        if (ch < ' ' || ch == 0x7f) {
            return false;
        }
    }

    *method = (str_t){line.ptr, (size_t)(first - line.ptr)};
    *target = (str_t){first + 1, (size_t)(second - first - 1)};
    *version = (str_t){second + 1, (size_t)(end - second - 1)};
    return true;
}

static int parse_version(str_t version, int *out_minor)
{
    if (version.len == 8 && memcmp(version.ptr, "HTTP/1.", 7) == 0 &&
        (version.ptr[7] == '0' || version.ptr[7] == '1')) {
        *out_minor = version.ptr[7] - '0';
        return 0;
    }
    if (version.len == 8 && memcmp(version.ptr, "HTTP/", 5) == 0 && version.ptr[5] >= '0' &&
        version.ptr[5] <= '9' && version.ptr[6] == '.' && version.ptr[7] >= '0' &&
        version.ptr[7] <= '9') {
        return 505;
    }
    return 400;
}

static int parse_method(str_t token, http_method_t *out_method)
{
    if (!http_token_valid(token)) {
        return 400;
    }
    if (token.len == 3 && memcmp(token.ptr, "GET", 3) == 0) {
        *out_method = HTTP_GET;
    } else if (token.len == 4 && memcmp(token.ptr, "HEAD", 4) == 0) {
        *out_method = HTTP_HEAD;
    } else if (token.len == 4 && memcmp(token.ptr, "POST", 4) == 0) {
        *out_method = HTTP_POST;
    } else {
        return 501;
    }
    return 0;
}

static bool is_authority_form(str_t target)
{
    const char *end = target.ptr + target.len;
    const char *port = NULL;
    if (target.ptr[0] == '[') {
        const char *closing = memchr(target.ptr, ']', target.len);
        if (closing != NULL && closing > target.ptr + 1 && end - closing > 2 && closing[1] == ':') {
            port = closing + 2;
        }
    } else {
        const char *colon = memchr(target.ptr, ':', target.len);
        if (colon != NULL && colon > target.ptr && colon + 1 < end) {
            port = colon + 1;
        }
    }
    if (port == NULL) {
        return false;
    }
    for (const char *at = target.ptr; at < port; at++) {
        if (strchr("/?#@", *at) != NULL) {
            return false;
        }
    }
    for (const char *at = port; at < end; at++) {
        if (*at < '0' || *at > '9') {
            return false;
        }
    }
    return true;
}

static int parse_target(http_request_t *req, size_t max_uri)
{
    str_t rest = req->target;
    if (rest.len > max_uri) {
        return 414;
    }
    if (rest.ptr[0] != '/' && uri_parse_absolute(req->target, &req->host, &rest) != 0) {
        return (rest.len == 1 && rest.ptr[0] == '*') || is_authority_form(rest) ? 501 : 400;
    }
    if (uri_split(rest, &req->raw_path, &req->query) != 0 ||
        uri_decode_path(req->raw_path, req->path, sizeof(req->path), NULL) != 0 ||
        path_normalize(req->path) != 0) {
        return 400;
    }
    return 0;
}

http_parse_result_t request_parse_head(const char *p, size_t len, const http_limits_t *lim,
                                       http_request_t *out_req, int *out_status)
{
    size_t skip;
    size_t head_len;
    http_parse_result_t result =
        http_find_head_end(p, len, lim->max_head, &skip, &head_len, out_status);
    if (request_line_too_long(p, len, lim->max_uri)) {
        return reject(414, out_status);
    }
    if (result != HTTP_PARSE_DONE) {
        return result;
    }

    const char *start = p + skip;
    const char *newline = memchr(start, '\n', head_len - skip);
    const char *line_end = newline;
    if (line_end > start && line_end[-1] == '\r') {
        line_end--;
    }
    str_t version;
    http_request_t req = {0};
    if (!split_request_line((str_t){start, (size_t)(line_end - start)}, &req.method_raw,
                            &req.target, &version)) {
        return reject(400, out_status);
    }

    int status = parse_method(req.method_raw, &req.method);
    if (status == 0) {
        status = parse_version(version, &req.minor);
    }
    if (status == 0) {
        status = parse_target(&req, lim->max_uri);
    }
    if (status != 0) {
        return reject(status, out_status);
    }

    const char *fields = newline + 1;
    const char *fields_end = p + head_len - 1;
    if (fields_end > fields && fields_end[-1] == '\r') {
        fields_end--;
    }
    if (http_parse_headers(fields, (size_t)(fields_end - fields), lim->max_headers, &req.headers,
                           out_status) != 0) {
        return HTTP_PARSE_ERROR;
    }
    if (req.target.ptr[0] == '/') {
        if (req.minor == 1 && http_header_count(&req.headers, "Host") != 1) {
            return reject(400, out_status);
        }
        const http_header_t *host = http_header_get(&req.headers, "Host");
        if (host != NULL) {
            req.host = host->value;
        }
    }
    if (http_resolve_framing(&req.headers, false, 0, req.method, &req.framing, out_status) != 0) {
        return HTTP_PARSE_ERROR;
    }
    if (req.framing.kind == FRAMING_CHUNKED) {
        return reject(501, out_status);
    }
    if (req.framing.kind == FRAMING_LENGTH && req.framing.length > lim->max_body) {
        return reject(413, out_status);
    }
    req.keep_alive = http_keep_alive(req.minor, &req.headers);
    req.head_len = head_len;
    *out_req = req;
    return HTTP_PARSE_DONE;
}
