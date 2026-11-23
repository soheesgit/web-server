/* 상한이 있는 바이트 버퍼와 fd 입출력. */
#include "core/buf.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/platform.h"

enum { BUF_READ_SIZE = 16 * 1024 };

int buf_init(buf_t *b, size_t initial, size_t max)
{
    *b = (buf_t){0};
    if (max == 0) {
        errno = EINVAL;
        return -1;
    }
    size_t cap = initial < max ? initial : max;
    if (cap > 0) {
        char *data = malloc(cap);
        if (data == NULL) {
            return -1;
        }
        b->data = data;
    }
    b->cap = cap;
    b->max = max;
    return 0;
}

void buf_cleanup(buf_t *b)
{
    if (b != NULL) {
        int saved = errno;
        free(b->data);
        *b = (buf_t){0};
        errno = saved;
    }
}

static size_t growth_capacity(const buf_t *b, size_t need)
{
    size_t doubled = b->cap <= b->max / 2 ? 2 * b->cap : b->max;
    return need > doubled ? need : doubled;
}

int buf_append(buf_t *b, const void *p, size_t n)
{
    if (n == 0) {
        return 0;
    }
    if (n > b->max - b->len) {
        errno = EMSGSIZE;
        return -1;
    }

    size_t end = b->len + n;
    if (end > b->cap) {
        size_t cap = growth_capacity(b, end);
        char *data = malloc(cap);
        if (data == NULL) {
            return -1;
        }
        /* p가 기존 버퍼를 가리켜도 두 복사가 끝나기 전까지 원본을 해제하지 않는다. */
        if (b->len > 0) {
            memcpy(data, b->data, b->len);
        }
        memcpy(data + b->len, p, n);
        free(b->data);
        b->data = data;
        b->cap = cap;
    } else {
        memmove(b->data + b->len, p, n);
    }
    b->len = end;
    return 0;
}

int buf_appendf(buf_t *b, const char *fmt, ...)
{
    va_list ap, measure;
    va_start(ap, fmt);
    va_copy(measure, ap);
    int len = vsnprintf(NULL, 0, fmt, measure);
    va_end(measure);
    if (len < 0 || (size_t)len > b->max - b->len) {
        va_end(ap);
        errno = len < 0 ? EINVAL : EMSGSIZE;
        return -1;
    }
    if (len == 0) {
        va_end(ap);
        return 0;
    }

    /* 임시 버퍼에서 포맷을 완성해야 실패할 때 기존 바이트가 그대로 남는다. */
    char *tmp = malloc((size_t)len + 1);
    if (tmp == NULL) {
        va_end(ap);
        return -1;
    }
    int written = vsnprintf(tmp, (size_t)len + 1, fmt, ap);
    va_end(ap);
    int result = written == len ? buf_append(b, tmp, (size_t)len) : -1;
    int saved = written == len ? errno : EINVAL;
    free(tmp);
    errno = saved;
    return result;
}

void buf_consume(buf_t *b, size_t n)
{
    if (n >= b->len) {
        b->len = 0;
    } else if (n != 0) {
        b->len -= n;
        memmove(b->data, b->data + n, b->len);
    }
}

void buf_reset(buf_t *b)
{
    b->len = 0;
}

ssize_t buf_read_fd(buf_t *b, int fd)
{
    size_t room = b->max - b->len;
    if (room == 0) {
        errno = EMSGSIZE;
        return -1;
    }
    size_t count = room < BUF_READ_SIZE ? room : BUF_READ_SIZE;
    size_t end = b->len + count;
    if (end > b->cap) {
        /* 읽기 전에 자리를 확보해 할당 실패로 fd에서 읽은 데이터를 잃지 않는다. */
        size_t cap = growth_capacity(b, end);
        char *data = realloc(b->data, cap);
        if (data == NULL) {
            return -1;
        }
        b->data = data;
        b->cap = cap;
    }
    ssize_t n;
    do {
        n = read(fd, b->data + b->len, count);
    } while (n < 0 && errno == EINTR);
    if (n > 0) {
        b->len += (size_t)n;
    }
    return n;
}

ssize_t buf_write_fd(buf_t *b, int fd)
{
    if (b->len == 0) {
        return 0;
    }
    size_t count = b->len > (size_t)SSIZE_MAX ? (size_t)SSIZE_MAX : b->len;
    ssize_t n = platform_send(fd, b->data, count);
    if (n < 0 && errno == ENOTSOCK) {
        do {
            n = write(fd, b->data, count);
        } while (n < 0 && errno == EINTR);
    }
    /* 부분 전송과 일시적인 오류 모두 아직 전송하지 않은 바이트는 그대로 남긴다. */
    if (n > 0) {
        buf_consume(b, (size_t)n);
    }
    return n;
}
