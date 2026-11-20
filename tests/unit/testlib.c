/* tests/unit/testlib.c
 *
 * testlib.h 구현: 등록 표, 실패 기록, 임시 디렉토리.
 */
#include "unit/testlib.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static test_entry_t g_tests[TEST_MAX];
static size_t g_ntests;
static int g_failed;

void test_register(const char *id, const char *name, test_fn fn)
{
    if (g_ntests >= TEST_MAX) {
        fprintf(stderr, "testlib: too many tests (max %d)\n", TEST_MAX);
        exit(2);
    }
    g_tests[g_ntests].id = id;
    g_tests[g_ntests].name = name;
    g_tests[g_ntests].fn = fn;
    g_ntests++;
}

void test_fail(const char *file, int line, const char *fmt, ...)
{
    va_list ap;

    g_failed = 1;
    fprintf(stderr, "%s:%d: ", file, line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void test_skip(const char *reason)
{
    fprintf(stderr, "%s\n", reason);
    fflush(NULL);
    exit(TEST_EXIT_SKIP);
}

const char *test_tmpdir(void)
{
    const char *base = getenv("TMPDIR");
    char tmpl[1024];
    char *dir;

    if (base == NULL || base[0] == '\0') {
        base = "/tmp";
    }
    if (snprintf(tmpl, sizeof(tmpl), "%s/httpd-test.XXXXXX", base) >= (int)sizeof(tmpl)) {
        return NULL;
    }
    if (mkdtemp(tmpl) == NULL) {
        return NULL;
    }
    dir = realpath(tmpl, NULL);
    return dir;
}

int test_write_file(const char *dir, const char *name, const void *data, size_t n, int mode)
{
    char path[4096];
    int fd;
    const char *p = data;

    if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path)) {
        return -1;
    }
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) {
        return -1;
    }
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) {
            close(fd);
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    if (fchmod(fd, (mode_t)mode) != 0) {
        close(fd);
        return -1;
    }
    return close(fd);
}

size_t testlib_count(void)
{
    return g_ntests;
}

test_entry_t *testlib_entries(void)
{
    return g_tests;
}

int testlib_failed(void)
{
    return g_failed;
}
