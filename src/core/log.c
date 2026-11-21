/* src/core/log.c
 *
 * 접근·에러 로그. 명세: log.h
 *
 * 한 줄을 스택 버퍼에 다 만든 뒤 write 한 번으로 쓴다(O_APPEND라 줄이 섞이지 않는다).
 * fd 전역은 log_open·log_close(main-only)만 바꾸고, 워커 스레드는 읽기만 한다.
 */
#include "core/log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "core/platform.h"

#define LOG_LINE_MAX 4096
#define LOG_TS_LEN 20 /* "2023-11-14T22:13:20Z" */

static int g_access_fd = -1; /* -1이면 stderr */
static int g_error_fd = -1;  /* -1이면 stderr */
static log_level_t g_min = LOG_DEBUG;

static int fd_or_stderr(int fd)
{
    return fd >= 0 ? fd : STDERR_FILENO;
}

/* ISO 8601 UTC 시각을 out에 쓴다 (NUL 포함 LOG_TS_LEN+1바이트 필요). */
static void format_ts(char *out, size_t cap)
{
    time_t now = platform_wall_time();
    struct tm tm;

    if (now == (time_t)-1 || gmtime_r(&now, &tm) == NULL ||
        strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) {
        (void)snprintf(out, cap, "%s", "-");
    }
}

/* 한 줄을 write 한 번으로 쓴다. 쓰기 실패는 무시하고 errno는 보존한다. */
static void write_line(int fd, const char *p, size_t n)
{
    ssize_t w;

    do {
        w = write(fd, p, n);
    } while (w < 0 && errno == EINTR);
}

/* 줄 버퍼 끝을 LF로 마무리한다. 넘쳤으면 잘라서 LF로 끝낸다. 결과 길이를 돌려준다. */
static size_t finish_line(char *line, size_t len, size_t cap)
{
    if (len >= cap) {
        len = cap - 1;
    }
    while (len > 0 && line[len - 1] == '\n') {
        len--;
    }
    line[len++] = '\n';
    return len;
}

int log_open(const char *access_path, const char *error_path, log_level_t min)
{
    int afd = -1;
    int efd = -1;
    int saved;

    log_close();
    if (access_path != NULL && access_path[0] != '\0') {
        afd = open(access_path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
        if (afd < 0) {
            return -1;
        }
    }
    if (error_path != NULL && error_path[0] != '\0') {
        efd = open(error_path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
        if (efd < 0) {
            saved = errno;
            if (afd >= 0) {
                (void)close(afd);
            }
            errno = saved;
            return -1;
        }
    }
    g_access_fd = afd;
    g_error_fd = efd;
    g_min = min;
    return 0;
}

void log_close(void)
{
    int saved = errno;

    if (g_access_fd >= 0) {
        (void)close(g_access_fd);
    }
    if (g_error_fd >= 0) {
        (void)close(g_error_fd);
    }
    g_access_fd = -1;
    g_error_fd = -1;
    g_min = LOG_DEBUG;
    errno = saved;
}

void log_access(const char *client_ip, const char *method, const char *target, int status,
                uint64_t bytes, int64_t elapsed_ms)
{
    static const char hex[] = "0123456789ABCDEF";
    char line[LOG_LINE_MAX];
    char ts[LOG_TS_LEN + 1];
    size_t len;
    int saved = errno;
    int n;

    format_ts(ts, sizeof(ts));
    n = snprintf(line, sizeof(line), "%s %s \"%s ", ts, client_ip != NULL ? client_ip : "-",
                 method != NULL ? method : "-");
    len = n < 0 ? 0 : (size_t)n;
    if (target == NULL) {
        target = "-";
    }
    /* target: 제어 문자와 '"'는 \xHH. 뒤 꼬리(약 60바이트) 자리를 남기고 넘치면 자른다 */
    for (const char *t = target; *t != '\0' && len + 4 < sizeof(line) - 64; t++) {
        unsigned char c = (unsigned char)*t;

        if (c < 0x20 || c == 0x7F || c == '"') {
            line[len++] = '\\';
            line[len++] = 'x';
            line[len++] = hex[c >> 4];
            line[len++] = hex[c & 0xF];
        } else {
            line[len++] = (char)c;
        }
    }
    if (len < sizeof(line)) {
        n = snprintf(line + len, sizeof(line) - len, "\" %d %llu %lldms", status,
                     (unsigned long long)bytes, (long long)elapsed_ms);
        len += n < 0 ? 0 : (size_t)n;
    }
    len = finish_line(line, len, sizeof(line));
    write_line(fd_or_stderr(g_access_fd), line, len);
    errno = saved;
}

void log_msg(log_level_t lvl, const char *fmt, ...)
{
    static const char *const names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    char line[LOG_LINE_MAX];
    char ts[LOG_TS_LEN + 1];
    const char *name;
    size_t len;
    va_list ap;
    int saved = errno;
    int n;

    if (lvl < g_min) {
        return;
    }
    name = (lvl >= LOG_DEBUG && lvl <= LOG_ERROR) ? names[lvl] : "?";
    format_ts(ts, sizeof(ts));
    n = snprintf(line, sizeof(line), "%s [%s] ", ts, name);
    len = n < 0 ? 0 : (size_t)n;
    va_start(ap, fmt);
    n = vsnprintf(line + len, sizeof(line) - len, fmt, ap);
    va_end(ap);
    len += n < 0 ? 0 : (size_t)n;
    len = finish_line(line, len, sizeof(line));
    write_line(fd_or_stderr(g_error_fd), line, len);
    errno = saved;
}
