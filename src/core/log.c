/* 접근 로그와 에러 로그를 완성된 한 줄 단위로 기록한다. */
#include "core/log.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/buf.h"
#include "core/platform.h"

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static int access_fd = -1;
static int error_fd = -1;
static log_level_t minimum = LOG_DEBUG;

/* -1은 외부에서 소유하는 stderr를 사용한다는 뜻이다. */
static int open_log(const char *path, int *out_fd)
{
    *out_fd = -1;
    if (path == NULL || path[0] == '\0') {
        return 0;
    }
    *out_fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    return *out_fd < 0 ? -1 : 0;
}

static void reset_logs(void)
{
    if (access_fd >= 0) {
        (void)close(access_fd);
    }
    if (error_fd >= 0) {
        (void)close(error_fd);
    }
    access_fd = -1;
    error_fd = -1;
    minimum = LOG_DEBUG;
}

int log_open(const char *access_path, const char *error_path, log_level_t min)
{
    pthread_mutex_lock(&log_lock);
    reset_logs();
    int rc = 0;
    if (open_log(access_path, &access_fd) < 0 || open_log(error_path, &error_fd) < 0) {
        int saved = errno;
        reset_logs();
        errno = saved;
        rc = -1;
    } else {
        minimum = min;
    }
    pthread_mutex_unlock(&log_lock);
    return rc;
}

void log_close(void)
{
    int saved = errno;
    pthread_mutex_lock(&log_lock);
    reset_logs();
    pthread_mutex_unlock(&log_lock);
    errno = saved;
}

static void timestamp(char out[32])
{
    time_t now = platform_wall_time();
    struct tm utc;
    if (now == (time_t)-1 || gmtime_r(&now, &utc) == NULL ||
        strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) {
        memcpy(out, "1970-01-01T00:00:00Z", sizeof("1970-01-01T00:00:00Z"));
    }
}

static void submit(const buf_t *line, bool access, log_level_t level)
{
    /* fd 교체와 단일 write를 같은 잠금 아래에 두어 닫힌 fd에 기록하는 경합을 막는다. */
    pthread_mutex_lock(&log_lock);
    int fd = access ? access_fd : error_fd;
    if (access || level >= minimum) {
        ssize_t written = write(fd < 0 ? STDERR_FILENO : fd, line->data, line->len);
        (void)written; /* 실패하거나 일부만 써도 두 번째 write로 다른 줄과 섞지 않는다. */
    }
    pthread_mutex_unlock(&log_lock);
}

static const char *or_dash(const char *s)
{
    return s == NULL ? "-" : s;
}

static int append_log_text(buf_t *out, const char *text, size_t len, bool target)
{
    static const char hex[] = "0123456789ABCDEF";

    size_t start = 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)text[i];
        bool escape_byte = target ? (c < 0x20 || c == 0x7f || c == '"') : c == '\r' || c == '\n';
        if (escape_byte) {
            /* 원문의 제어 바이트가 로그 줄이나 접근 로그의 따옴표를 끊지 않게 한다. */
            char escape[] = {'\\', 'x', hex[c >> 4], hex[c & 0x0f]};
            if (buf_append(out, text + start, i - start) < 0 ||
                buf_append(out, escape, sizeof(escape)) < 0) {
                return -1;
            }
            start = i + 1;
        }
    }
    return buf_append(out, text + start, len - start);
}

void log_access(const char *client_ip, const char *method, const char *target, int status,
                uint64_t bytes, int64_t elapsed_ms)
{
    char timebuf[32];
    timestamp(timebuf);
    const char *path = or_dash(target);
    buf_t line;
    if (buf_init(&line, 0, SIZE_MAX) == 0 &&
        buf_appendf(&line, "%s %s \"%s ", timebuf, or_dash(client_ip), or_dash(method)) == 0 &&
        append_log_text(&line, path, strlen(path), true) == 0 &&
        buf_appendf(&line, "\" %d %" PRIu64 " %" PRId64 "ms\n", status, bytes, elapsed_ms) == 0) {
        submit(&line, true, LOG_DEBUG);
    }
    buf_cleanup(&line);
}

void log_msg(log_level_t lvl, const char *fmt, ...)
{
    static const char *const levels[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    if ((unsigned)lvl >= sizeof(levels) / sizeof(levels[0])) {
        return;
    }

    va_list ap, measure;
    va_start(ap, fmt);
    va_copy(measure, ap);
    int len = vsnprintf(NULL, 0, fmt, measure);
    va_end(measure);
    char *message = len >= 0 ? malloc((size_t)len + 1) : NULL;
    int written = message == NULL ? -1 : vsnprintf(message, (size_t)len + 1, fmt, ap);
    va_end(ap);
    if (message == NULL || written != len) {
        free(message);
        return;
    }

    size_t end = (size_t)len;
    while (end > 0 && message[end - 1] == '\n') {
        --end;
    }
    char timebuf[32];
    timestamp(timebuf);
    buf_t line;
    if (buf_init(&line, 0, SIZE_MAX) == 0 &&
        buf_appendf(&line, "%s [%s] ", timebuf, levels[lvl]) == 0 &&
        append_log_text(&line, message, end, false) == 0 && buf_append(&line, "\n", 1) == 0) {
        submit(&line, false, lvl);
    }
    buf_cleanup(&line);
    free(message);
}
