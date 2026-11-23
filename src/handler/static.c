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
    /* HTTP 날짜의 요일과 월 이름은 프로세스 로케일과 관계없이 영문 약어로 보낸다. */
    static const char *const weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    if (date->tm_year < -1900 || date->tm_year > 8099) {
        return -1;
    }
    return snprintf(out, HTTP_DATE_SIZE, "%s, %02d %s %04d %02d:%02d:%02d GMT",
                    weekdays[date->tm_wday], date->tm_mday, months[date->tm_mon],
                    date->tm_year + 1900, date->tm_hour, date->tm_min, date->tm_sec);
}

static uint64_t http_date_key(const struct tm *date)
{
    /*
     * 연·월·일·시·분·초 순서로 비교할 수 있는 정렬 키다. 각 달에 31일 자리를
     * 배정하므로 실제 경과 초를 계산하거나 시스템 시간대로 변환할 필요가 없다.
     */
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
    /*
     * 파싱한 값을 다시 IMF-fixdate로 써서 자릿수와 구분자를 확인한다.
     * 이어서 윤년을 반영해 해당 월에 실제로 존재하는 날짜인지 검사한다.
     */
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
    /*
     * FIFO의 상대가 없어도 open이 대기하지 않게 한다. 마지막 경로 요소의 심볼릭
     * 링크를 따라가지 않으며, 열린 fd를 fstat해 실제 파일 종류를 확인한다.
     */
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

    /*
     * 정규화 경로에는 디코딩된 '?'나 '#'이 들어 있을 수 있다. 경로의 해당 바이트는
     * 다시 인코딩하고, 리다이렉트 URL에는 원래 쿼리를 경로 뒤에 붙인다.
     */
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
    /* 예를 들어 접두사 /assets/에 매칭된 /assets/logo.png는 루트 아래 logo.png에 대응한다. */
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
        /*
         * 인덱스 자체가 심볼릭 링크일 수 있어 루트 경계를 다시 확인한다.
         * 디렉토리에 인덱스가 없으면 목록을 제공할 수 없으므로 403으로 응답한다.
         */
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
    /* 클라이언트가 가진 사본을 재사용할 수 있으면 fd를 닫고 304 헤더만 돌려준다. */
    if (not_modified(req, &modified)) {
        (void)close(fd);
        plan_cleanup(plan);
        plan->head.status = 304;
        return plan_add_header(plan, "Last-Modified", last_modified) == 0 ? 0 : 500;
    }
    if (plan_add_header(plan, "Content-Type", mime_for_path(relative)) < 0 ||
        plan_add_header(plan, "Last-Modified", last_modified) < 0) {
        (void)close(fd);
        return 500;
    }
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0) {
        (void)close(fd);
        return 500;
    }

    /*
     * 파일을 읽지 않고 fd와 크기를 응답 계획으로 이전한다. GET과 HEAD가 같은 메타데이터를
     * 쓰며, 실제 본문을 보낼지는 연결 처리기가 결정한다.
     */
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
