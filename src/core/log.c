/* src/core/log.c
 *
 * 접근·에러 로그. 명세: log.h
 */
#include "core/log.h"

#include "core/platform.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int access_fd = STDERR_FILENO;
static int error_fd = STDERR_FILENO;
static log_level_t min_level = LOG_DEBUG;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static int write_line(int fd, const char *p, size_t n)
{
    ssize_t written;
    do {
        written = write(fd, p, n);
    } while (written < 0 && errno == EINTR);
    return written == (ssize_t)n ? 0 : -1;
}

static void format_time(char out[32])
{
    time_t now = platform_wall_time();
    struct tm tm_now;
    if (now == (time_t)-1 || gmtime_r(&now, &tm_now) == NULL ||
        strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tm_now) == 0) {
        (void)snprintf(out, 32, "1970-01-01T00:00:00Z");
    }
}

static const char *level_name(log_level_t lvl)
{
    switch (lvl) {
    case LOG_DEBUG:
        return "DEBUG";
    case LOG_INFO:
        return "INFO";
    case LOG_WARN:
        return "WARN";
    case LOG_ERROR:
        return "ERROR";
    }
    return "ERROR";
}

int log_open(const char *access_path, const char *error_path, log_level_t min)
{
    int new_access = STDERR_FILENO, new_error = STDERR_FILENO;
    int saved;
    if (access_path != NULL && access_path[0] != '\0') {
        new_access = open(access_path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
        if (new_access < 0) {
            return -1;
        }
    }
    if (error_path != NULL && error_path[0] != '\0') {
        new_error = open(error_path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
        if (new_error < 0) {
            saved = errno;
            if (new_access != STDERR_FILENO) {
                (void)close(new_access);
            }
            errno = saved;
            return -1;
        }
    }
    pthread_mutex_lock(&log_lock);
    if (access_fd != STDERR_FILENO) {
        (void)close(access_fd);
    }
    if (error_fd != STDERR_FILENO && error_fd != access_fd) {
        (void)close(error_fd);
    }
    access_fd = new_access;
    error_fd = new_error;
    min_level = min;
    pthread_mutex_unlock(&log_lock);
    return 0;
}

void log_close(void)
{
    int saved = errno;
    pthread_mutex_lock(&log_lock);
    if (access_fd != STDERR_FILENO) {
        (void)close(access_fd);
    }
    if (error_fd != STDERR_FILENO && error_fd != access_fd) {
        (void)close(error_fd);
    }
    access_fd = STDERR_FILENO;
    error_fd = STDERR_FILENO;
    min_level = LOG_DEBUG;
    pthread_mutex_unlock(&log_lock);
    errno = saved;
}

void log_access(const char *client_ip, const char *method, const char *target, int status,
                uint64_t bytes, int64_t elapsed_ms)
{
    const char *ip = client_ip == NULL ? "-" : client_ip;
    const char *m = method == NULL ? "-" : method;
    const unsigned char *t = (const unsigned char *)(target == NULL ? "-" : target);
    size_t ipn = strlen(ip), mn = strlen(m), tn = strlen((const char *)t), escaped = 0, i, pos = 0;
    size_t cap;
    char stamp[32], *line;
    int head;
    for (i = 0; i < tn; i++) {
        size_t add = (t[i] < 0x20 || t[i] == 0x7f || t[i] == '"') ? 4 : 1;
        if (escaped > SIZE_MAX - add) {
            return;
        }
        escaped += add;
    }
    if (ipn > SIZE_MAX - mn - escaped - 256) {
        return;
    }
    cap = ipn + mn + escaped + 256;
    line = malloc(cap);
    if (line == NULL) {
        return;
    }
    format_time(stamp);
    head = snprintf(line, cap, "%s %s \"%s ", stamp, ip, m);
    if (head < 0 || (size_t)head >= cap) {
        free(line);
        return;
    }
    pos = (size_t)head;
    for (i = 0; i < tn; i++) {
        if (t[i] < 0x20 || t[i] == 0x7f || t[i] == '"') {
            int n = snprintf(line + pos, cap - pos, "\\x%02X", (unsigned)t[i]);
            if (n != 4) {
                free(line);
                return;
            }
            pos += 4;
        } else {
            line[pos++] = (char)t[i];
        }
    }
    head = snprintf(line + pos, cap - pos, "\" %d %llu %lldms\n", status, (unsigned long long)bytes,
                    (long long)elapsed_ms);
    if (head >= 0 && (size_t)head < cap - pos) {
        pos += (size_t)head;
        pthread_mutex_lock(&log_lock);
        (void)write_line(access_fd, line, pos);
        pthread_mutex_unlock(&log_lock);
    }
    free(line);
}

void log_msg(log_level_t lvl, const char *fmt, ...)
{
    va_list ap, copy;
    int body_len, prefix_len, suffix_len;
    size_t total;
    char stamp[32], *line;
    if (fmt == NULL) {
        return;
    }
    pthread_mutex_lock(&log_lock);
    if (lvl < min_level) {
        pthread_mutex_unlock(&log_lock);
        return;
    }
    va_start(ap, fmt);
    va_copy(copy, ap);
    body_len = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (body_len < 0 || (size_t)body_len > SIZE_MAX - 128) {
        va_end(ap);
        pthread_mutex_unlock(&log_lock);
        return;
    }
    format_time(stamp);
    prefix_len = snprintf(NULL, 0, "%s [%s] ", stamp, level_name(lvl));
    total = (size_t)prefix_len + (size_t)body_len + 2;
    line = malloc(total);
    if (line == NULL) {
        va_end(ap);
        pthread_mutex_unlock(&log_lock);
        return;
    }
    (void)snprintf(line, total, "%s [%s] ", stamp, level_name(lvl));
    (void)vsnprintf(line + prefix_len, total - (size_t)prefix_len, fmt, ap);
    va_end(ap);
    suffix_len = body_len;
    while (suffix_len > 0 && line[prefix_len + suffix_len - 1] == '\n') {
        suffix_len--;
    }
    line[prefix_len + suffix_len] = '\n';
    (void)write_line(error_fd, line, (size_t)prefix_len + (size_t)suffix_len + 1);
    free(line);
    pthread_mutex_unlock(&log_lock);
}
