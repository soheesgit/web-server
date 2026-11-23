#include "downstream/response_writer.h"

#include "common/http_status.h"

#include <errno.h>
#include <inttypes.h>

static int write_date(buf_t *out, time_t now)
{
    static const char *const weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    struct tm utc;
    if (gmtime_r(&now, &utc) == NULL) {
        return -1;
    }
    return buf_appendf(out, "Date: %s, %02d %s %04ld %02d:%02d:%02d GMT\r\n", weekdays[utc.tm_wday],
                       utc.tm_mday, months[utc.tm_mon], (long)utc.tm_year + 1900, utc.tm_hour,
                       utc.tm_min, utc.tm_sec);
}

static int write_framing(const http_head_out_t *head, int client_minor, buf_t *out)
{
    if (head->framing.kind == FRAMING_LENGTH && head->status >= 200 && head->status != 204 &&
        head->status != 304) {
        return buf_appendf(out, "Content-Length: %" PRIu64 "\r\n", head->framing.length);
    }
    if (head->framing.kind == FRAMING_CHUNKED && client_minor == 1) {
        return buf_appendf(out, "Transfer-Encoding: chunked\r\n");
    }
    return 0;
}

int response_write_head(const http_head_out_t *head, int client_minor, time_t now, buf_t *out)
{
    if (head->status < 100 || head->status > 599) {
        errno = EINVAL;
        return -1;
    }

    size_t original_len = out->len;
    const char *connection = "";
    if (head->close || head->framing.kind == FRAMING_CLOSE ||
        (client_minor == 0 && head->framing.kind == FRAMING_CHUNKED)) {
        connection = "Connection: close\r\n";
    } else if (client_minor == 0) {
        connection = "Connection: keep-alive\r\n";
    }
    if (buf_appendf(out, "HTTP/1.1 %d %s\r\n", head->status, http_reason(head->status)) != 0 ||
        write_date(out, now) != 0 || buf_appendf(out, "Server: httpd/0.1\r\n") != 0 ||
        buf_append(out, head->fields.data, head->fields.len) != 0 ||
        write_framing(head, client_minor, out) != 0 ||
        buf_appendf(out, "%s\r\n", connection) != 0) {
        out->len = original_len;
        return -1;
    }
    return 0;
}

int response_write_chunk(const void *p, size_t n, buf_t *out)
{
    if (n == 0) {
        return 0;
    }
    size_t original_len = out->len;
    if (buf_appendf(out, "%zx\r\n", n) != 0 || buf_append(out, p, n) != 0 ||
        buf_append(out, "\r\n", 2) != 0) {
        out->len = original_len;
        return -1;
    }
    return 0;
}

int response_write_last_chunk(buf_t *out)
{
    return buf_append(out, "0\r\n\r\n", 5);
}
