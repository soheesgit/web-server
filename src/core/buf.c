/* src/core/buf.c
 *
 * 가변 버퍼. 명세: buf.h
 */
#include "core/buf.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/platform.h"

#define BUF_MIN_GROW 64
#define BUF_READ_CHUNK 65536

/* cap이 need 이상이 되도록 늘린다 (max를 넘겨 할당하지 않음). 실패 시 내용 불변. */
static int buf_reserve(buf_t *b, size_t need)
{
    size_t ncap;
    char *nd;

    if (need <= b->cap) {
        return 0;
    }
    ncap = b->cap < BUF_MIN_GROW ? BUF_MIN_GROW : b->cap;
    while (ncap < need) {
        ncap = ncap > SIZE_MAX / 2 ? need : ncap * 2;
    }
    if (ncap > b->max) {
        ncap = b->max;
    }
    nd = realloc(b->data, ncap);
    if (nd == NULL) {
        errno = ENOMEM;
        return -1;
    }
    b->data = nd;
    b->cap = ncap;
    return 0;
}

int buf_init(buf_t *b, size_t initial, size_t max)
{
    memset(b, 0, sizeof(*b));
    if (max == 0) {
        errno = EINVAL;
        return -1;
    }
    if (initial > max) {
        initial = max;
    }
    if (initial > 0) {
        b->data = malloc(initial);
        if (b->data == NULL) {
            errno = ENOMEM;
            return -1;
        }
    }
    b->cap = initial;
    b->max = max;
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
    size_t off = 0;
    int inside;

    if (n == 0) {
        return 0;
    }
    if (n > b->max - b->len) {
        errno = EMSGSIZE;
        return -1;
    }
    /* p가 b->data 안을 가리키면 재할당 후 위치를 다시 계산한다 */
    inside = b->data != NULL && (const char *)p >= b->data && (const char *)p < b->data + b->cap;
    if (inside) {
        off = (size_t)((const char *)p - b->data);
    }
    if (buf_reserve(b, b->len + n) != 0) {
        return -1;
    }
    memmove(b->data + b->len, inside ? b->data + off : p, n);
    b->len += n;
    return 0;
}

int buf_appendf(buf_t *b, const char *fmt, ...)
{
    va_list ap;
    int need;
    char *tmp;
    int rc;

    va_start(ap, fmt);
    need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) {
        errno = EINVAL;
        return -1;
    }
    if ((size_t)need > b->max - b->len) {
        errno = EMSGSIZE;
        return -1;
    }
    tmp = malloc((size_t)need + 1);
    if (tmp == NULL) {
        errno = ENOMEM;
        return -1;
    }
    va_start(ap, fmt);
    (void)vsnprintf(tmp, (size_t)need + 1, fmt, ap);
    va_end(ap);
    rc = buf_append(b, tmp, (size_t)need);
    free(tmp);
    return rc;
}

void buf_consume(buf_t *b, size_t n)
{
    if (n >= b->len) {
        b->len = 0;
        return;
    }
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
}

void buf_reset(buf_t *b)
{
    b->len = 0;
}

ssize_t buf_read_fd(buf_t *b, int fd)
{
    size_t want;
    ssize_t r;

    if (b->len >= b->max) {
        errno = EMSGSIZE;
        return -1;
    }
    want = b->max - b->len;
    if (want > BUF_READ_CHUNK) {
        want = BUF_READ_CHUNK;
    }
    if (buf_reserve(b, b->len + want) != 0) {
        /* 큰 덩어리를 못 잡았으면 남은 용량만큼만 읽는다 */
        if (b->cap <= b->len) {
            return -1;
        }
        want = b->cap - b->len;
    }
    do {
        r = read(fd, b->data + b->len, want);
    } while (r < 0 && errno == EINTR);
    if (r > 0) {
        b->len += (size_t)r;
    }
    return r;
}

ssize_t buf_write_fd(buf_t *b, int fd)
{
    ssize_t w;

    if (b->len == 0) {
        return 0;
    }
    w = platform_send(fd, b->data, b->len);
    if (w < 0 && errno == ENOTSOCK) {
        do {
            w = write(fd, b->data, b->len);
        } while (w < 0 && errno == EINTR);
    }
    if (w > 0) {
        buf_consume(b, (size_t)w);
    }
    return w;
}
