/* src/core/buf.c
 *
 * 가변 버퍼. 명세: buf.h
 */
#include "core/buf.h"

#include "core/platform.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int buf_reserve(buf_t *b, size_t need)
{
    size_t cap;
    char *p;
    if (need <= b->cap) {
        return 0;
    }
    cap = b->cap == 0 ? 64 : b->cap;
    if (cap > b->max) {
        cap = b->max;
    }
    while (cap < need) {
        if (cap > b->max / 2) {
            cap = b->max;
            break;
        }
        cap *= 2;
    }
    p = realloc(b->data, cap);
    if (p == NULL) {
        errno = ENOMEM;
        return -1;
    }
    b->data = p;
    b->cap = cap;
    return 0;
}

int buf_init(buf_t *b, size_t initial, size_t max)
{
    if (b == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(b, 0, sizeof(*b));
    if (max == 0) {
        errno = EINVAL;
        return -1;
    }
    if (initial > max) {
        initial = max;
    }
    b->max = max;
    if (initial > 0) {
        b->data = malloc(initial);
        if (b->data == NULL) {
            memset(b, 0, sizeof(*b));
            errno = ENOMEM;
            return -1;
        }
        b->cap = initial;
    }
    return 0;
}

void buf_cleanup(buf_t *b)
{
    int saved = errno;
    if (b != NULL) {
        free(b->data);
        memset(b, 0, sizeof(*b));
    }
    errno = saved;
}

int buf_append(buf_t *b, const void *p, size_t n)
{
    size_t need, offset = 0;
    int internal = 0;
    if (b == NULL || (p == NULL && n != 0)) {
        errno = EINVAL;
        return -1;
    }
    if (n == 0) {
        return 0;
    }
    if (n > b->max - b->len) {
        errno = EMSGSIZE;
        return -1;
    }
    if (b->data != NULL && (uintptr_t)p >= (uintptr_t)b->data &&
        (uintptr_t)p < (uintptr_t)b->data + b->len) {
        offset = (size_t)((uintptr_t)p - (uintptr_t)b->data);
        if (n > b->len - offset) {
            errno = EINVAL;
            return -1;
        }
        internal = 1;
    }
    need = b->len + n;
    if (buf_reserve(b, need) != 0) {
        return -1;
    }
    if (internal) {
        p = b->data + offset;
    }
    memmove(b->data + b->len, p, n);
    b->len = need;
    return 0;
}

int buf_appendf(buf_t *b, const char *fmt, ...)
{
    va_list ap, copy;
    int n;
    char *tmp;
    if (b == NULL || fmt == NULL) {
        errno = EINVAL;
        return -1;
    }
    va_start(ap, fmt);
    va_copy(copy, ap);
    n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n < 0) {
        va_end(ap);
        errno = EINVAL;
        return -1;
    }
    if ((size_t)n > b->max - b->len) {
        va_end(ap);
        errno = EMSGSIZE;
        return -1;
    }
    tmp = malloc((size_t)n + 1);
    if (tmp == NULL) {
        va_end(ap);
        errno = ENOMEM;
        return -1;
    }
    if (vsnprintf(tmp, (size_t)n + 1, fmt, ap) != n) {
        va_end(ap);
        free(tmp);
        errno = EINVAL;
        return -1;
    }
    va_end(ap);
    if (buf_append(b, tmp, (size_t)n) != 0) {
        free(tmp);
        return -1;
    }
    free(tmp);
    return 0;
}

void buf_consume(buf_t *b, size_t n)
{
    if (b == NULL) {
        return;
    }
    if (n >= b->len) {
        b->len = 0;
    } else {
        memmove(b->data, b->data + n, b->len - n);
        b->len -= n;
    }
}

void buf_reset(buf_t *b)
{
    if (b != NULL) {
        b->len = 0;
    }
}

ssize_t buf_read_fd(buf_t *b, int fd)
{
    size_t room;
    ssize_t n;
    if (b == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (b->len >= b->max) {
        errno = EMSGSIZE;
        return -1;
    }
    room = b->max - b->len;
    if (room > 16384) {
        room = 16384;
    }
    if (buf_reserve(b, b->len + room) != 0) {
        return -1;
    }
    do {
        n = read(fd, b->data + b->len, room);
    } while (n < 0 && errno == EINTR);
    if (n > 0) {
        b->len += (size_t)n;
    }
    return n;
}

ssize_t buf_write_fd(buf_t *b, int fd)
{
    ssize_t n;
    if (b == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (b->len == 0) {
        return 0;
    }
    n = platform_send(fd, b->data, b->len);
    if (n < 0 && errno == ENOTSOCK) {
        do {
            n = write(fd, b->data, b->len);
        } while (n < 0 && errno == EINTR);
    }
    if (n > 0) {
        buf_consume(b, (size_t)n);
    }
    return n;
}
