/* src/handler/static.c
 *
 * 정적 파일 처리기. 명세: handler.h
 */
#include "handler/handler.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/http_headers.h"
#include "handler/internal_path.h"
#include "handler/mime.h"
#include "handler/plan.h"

#define HTTP_DATE_SIZE 30

static int format_http_date(const struct tm *date, char out[HTTP_DATE_SIZE])
{
    static const char *const weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    return snprintf(out, HTTP_DATE_SIZE, "%s, %02d %s %04d %02d:%02d:%02d GMT",
                    weekdays[date->tm_wday], date->tm_mday, months[date->tm_mon],
                    date->tm_year + 1900, date->tm_hour, date->tm_min, date->tm_sec);
}

static uint64_t http_date_key(const struct tm *date)
{
    uint64_t days = ((uint64_t)(date->tm_year + 1900) * 12 + (unsigned)date->tm_mon) * 31 +
                    (unsigned)date->tm_mday;
    return ((days * 24 + (unsigned)date->tm_hour) * 60 + (unsigned)date->tm_min) * 60 +
           (unsigned)date->tm_sec;
}

static bool not_modified(const http_request_t *req, const struct tm *modified)
{
    const http_header_t *since = http_header_get(&req->headers, "If-Modified-Since");
    struct tm parsed = {0};
    char date[HTTP_DATE_SIZE];
    char canonical[HTTP_DATE_SIZE];
    static const int days_per_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    if (since == NULL || since->value.len != sizeof(date) - 1 ||
        str_to_cstr(since->value, date, sizeof(date)) < 0) {
        return false;
    }
    const char *end = strptime(date, "%a, %d %b %Y %H:%M:%S GMT", &parsed);
    if (end != date + sizeof(date) - 1 ||
        format_http_date(&parsed, canonical) != (int)sizeof(canonical) - 1 ||
        strcmp(date, canonical) != 0) {
        return false;
    }
    int year = parsed.tm_year + 1900;
    bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    int days = days_per_month[parsed.tm_mon] + (parsed.tm_mon == 1 && leap);
    return parsed.tm_mday <= days && http_date_key(&parsed) >= http_date_key(modified);
}

static int open_static(const char *root, str_t relative, struct stat *out_st, int *out_fd)
{
    char path[PATH_MAX];
    int status = handler_resolve_path(root, relative, path);

    if (status != 0) {
        return status;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENXIO || errno == ENODEV) {
            return 403;
        }
        return handler_path_status(errno);
    }
    if (fstat(fd, out_st) < 0) {
        (void)close(fd);
        return 500;
    }
    *out_fd = fd;
    return 0;
}

static int redirect_directory(const http_request_t *req, response_plan_t *plan)
{
    static const char hex[] = "0123456789ABCDEF";
    char escaped[3 * HTTP_PATH_MAX];
    size_t n = 0;

    for (const unsigned char *p = (const unsigned char *)req->path; *p != '\0'; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            strchr("/-._~!$&'()*+,;=:@", c) != NULL) {
            escaped[n++] = (char)c;
        } else {
            escaped[n++] = '%';
            escaped[n++] = hex[c >> 4];
            escaped[n++] = hex[c & 15];
        }
    }
    escaped[n++] = '/';
    escaped[n] = '\0';
    if (req->query.len > SIZE_MAX - n - 2) {
        return -1;
    }
    char *location = malloc(n + req->query.len + 2);
    if (location == NULL) {
        return -1;
    }
    memcpy(location, escaped, n);
    if (req->query.len != 0) {
        location[n++] = '?';
        memcpy(location + n, req->query.ptr, req->query.len);
        n += req->query.len;
    }
    location[n] = '\0';
    int result = plan_add_header(plan, "Location", location);
    free(location);
    if (result == 0) {
        plan->head.status = 301;
        plan->head.framing = (http_framing_t){FRAMING_LENGTH, 0};
    }
    return result;
}

static int serve_static(const http_request_t *req, const route_t *rt, response_plan_t *plan)
{
    char root[PATH_MAX];
    char index_path[HTTP_PATH_MAX + sizeof("index.html")];
    struct stat st;
    int fd;

    if (realpath(rt->root, root) == NULL) {
        return handler_path_status(errno);
    }
    const char *relative = req->path + rt->prefix_len;
    if (*relative == '/') {
        relative++;
    }
    int status = open_static(root, (str_t){relative, strlen(relative)}, &st, &fd);
    if (status != 0) {
        return status;
    }
    if (S_ISDIR(st.st_mode)) {
        (void)close(fd);
        if (req->path[strlen(req->path) - 1] != '/') {
            return redirect_directory(req, plan) == 0 ? 0 : 500;
        }
        (void)snprintf(index_path, sizeof(index_path), "%sindex.html", relative);
        relative = index_path;
        status = open_static(root, (str_t){relative, strlen(relative)}, &st, &fd);
        if (status != 0) {
            return status == 404 ? 403 : status;
        }
    }
    if (!S_ISREG(st.st_mode)) {
        (void)close(fd);
        return 403;
    }

    struct tm modified;
    char last_modified[HTTP_DATE_SIZE];
    if (gmtime_r(&st.st_mtime, &modified) == NULL ||
        format_http_date(&modified, last_modified) != (int)sizeof(last_modified) - 1) {
        (void)close(fd);
        return 500;
    }
    bool unchanged = not_modified(req, &modified);
    if ((!unchanged && plan_add_header(plan, "Content-Type", mime_for_path(relative)) < 0) ||
        plan_add_header(plan, "Last-Modified", last_modified) < 0) {
        (void)close(fd);
        return 500;
    }
    if (unchanged) {
        (void)close(fd);
        plan->head.status = 304;
        return 0;
    }
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0) {
        (void)close(fd);
        return 500;
    }

    plan->kind = BODY_FILE;
    plan->head.status = 200;
    plan->head.framing = (http_framing_t){FRAMING_LENGTH, (uint64_t)st.st_size};
    plan->body.file.fd = fd;
    plan->body.file.off = 0;
    plan->body.file.len = st.st_size;
    return 0;
}

handler_result_t static_handle(const http_request_t *req, const route_t *rt,
                               const handler_ctx_t *ctx, response_plan_t *out_plan)
{
    (void)ctx;
    if (req->method != HTTP_GET && req->method != HTTP_HEAD) {
        if (plan_error(out_plan, 405) == 0 && plan_add_header(out_plan, "Allow", "GET, HEAD") < 0) {
            (void)plan_error(out_plan, 500);
        }
        return HANDLER_DONE;
    }

    int status = serve_static(req, rt, out_plan);
    if (status != 0) {
        (void)plan_error(out_plan, status);
    }
    return HANDLER_DONE;
}
