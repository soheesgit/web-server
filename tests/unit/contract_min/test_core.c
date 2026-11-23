/* tests/unit/contract_min/test_core.c
 *
 * core/ 계약 최소셋. 기대값의 원래 표: 기획서 7장. ID 목록: src/core/README.md
 */
#include "unit/testlib.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/buf.h"
#include "core/config.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/str.h"

/* 임시 디렉토리에 www/를 만들고 그곳으로 chdir한다 (기본 root "./www"가 존재하도록). */
static int enter_tmp_with_www(void)
{
    char dir[] = "/tmp/httpd-config-test.XXXXXX";
    char www[sizeof(dir) + sizeof("/www")];

    if (mkdtemp(dir) == NULL || snprintf(www, sizeof(www), "%s/www", dir) >= (int)sizeof(www)) {
        return -1;
    }
    if (mkdir(www, 0755) != 0) {
        return -1;
    }
    return chdir(dir);
}

static void t_u_core_04(void)
{
    uint64_t v = 99;

    CHECK_EQ_INT(str_to_u64(STR_LIT("0"), UINT64_MAX, &v), 0);
    CHECK_EQ_INT(v, 0);
    CHECK_EQ_INT(str_to_u64(STR_LIT("123"), UINT64_MAX, &v), 0);
    CHECK_EQ_INT(v, 123);
}

static void t_u_core_05(void)
{
    static const char *bad[] = {"", "-1", "+5", "12a", " 12"};
    uint64_t v = 0;

    /* 정상 대조 (D-017): 스텁(항상 -1)에서 통과하지 않도록 */
    CHECK_EQ_INT(str_to_u64(STR_LIT("5"), UINT64_MAX, &v), 0);
    CHECK_EQ_INT(v, 5);
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        str_t s = {bad[i], strlen(bad[i])};
        v = 77;
        if (str_to_u64(s, UINT64_MAX, &v) != -1) {
            test_fail(__FILE__, __LINE__, "str_to_u64(\"%s\") should fail", bad[i]);
            return;
        }
        CHECK_EQ_INT(v, 77); /* 실패하면 *out_v 불변 */
    }
}

static void t_u_core_06(void)
{
    uint64_t value = 0;
    CHECK_EQ_INT(str_to_u64(STR_LIT("18446744073709551615"), UINT64_MAX, &value), 0);
    CHECK(value == UINT64_MAX);
    CHECK_EQ_INT(str_to_u64(STR_LIT("18446744073709551616"), UINT64_MAX, &value), -1);
    CHECK_EQ_INT(errno, ERANGE);
    CHECK(value == UINT64_MAX);
}

static void t_u_core_08(void)
{
    buf_t b;

    CHECK_EQ_INT(buf_init(&b, 16, 1024), 0);
    CHECK_EQ_INT(buf_append(&b, "hello", 5), 0);
    CHECK_EQ_INT(b.len, 5);
    CHECK_EQ_MEM(b.data, b.len, "hello", 5);
    CHECK(b.cap >= b.len);
    buf_cleanup(&b);
}

static void t_u_core_09(void)
{
    buf_t b;
    const char bytes[] = {'a', '\0', 'b'};
    CHECK_EQ_INT(buf_init(&b, sizeof(bytes), 32), 0);
    CHECK_EQ_INT(buf_append(&b, bytes, sizeof(bytes)), 0);
    /* 재할당이 필요한 상태에서 버퍼 자신이 소유하는 바이트를 복사한다. */
    CHECK_EQ_INT(buf_append(&b, b.data + 1, 2), 0);
    CHECK_EQ_MEM(b.data, b.len, "a\0b\0b", 5);
    buf_cleanup(&b);
}

static void t_u_core_10(void)
{
    buf_t b;
    char a[30];

    memset(a, 'a', sizeof(a));
    CHECK_EQ_INT(buf_init(&b, 8, 32), 0);
    CHECK_EQ_INT(buf_append(&b, a, sizeof(a)), 0);
    errno = 0;
    CHECK_EQ_INT(buf_append(&b, "xyz", 3), -1);
    CHECK_EQ_INT(errno, EMSGSIZE);
    CHECK_EQ_INT(b.len, 30);
    CHECK_EQ_MEM(b.data, b.len, a, sizeof(a));
    buf_cleanup(&b);
}

static void t_u_core_11(void)
{
    buf_t b;

    CHECK_EQ_INT(buf_init(&b, 16, 1024), 0);
    CHECK_EQ_INT(buf_append(&b, "0123456789", 10), 0);
    buf_consume(&b, 4);
    CHECK_EQ_INT(b.len, 6);
    CHECK_EQ_MEM(b.data, b.len, "456789", 6);

    buf_reset(&b);
    CHECK_EQ_INT(buf_append(&b, "0123456789", 10), 0);
    buf_consume(&b, 10);
    CHECK_EQ_INT(b.len, 0);

    CHECK_EQ_INT(buf_append(&b, "0123456789", 10), 0);
    buf_consume(&b, 20);
    CHECK_EQ_INT(b.len, 0);
    buf_cleanup(&b);
}

static void t_u_core_13(void)
{
    int sv[2];
    buf_t b;
    ssize_t r;

    CHECK_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    CHECK(fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK) == 0);
    CHECK_EQ_INT(buf_init(&b, 16, 1024), 0);

    errno = 0;
    r = buf_read_fd(&b, sv[0]);
    CHECK_EQ_INT(r, -1);
    CHECK(errno == EAGAIN || errno == EWOULDBLOCK);

    CHECK_EQ_INT(write(sv[1], "abc", 3), 3);
    r = buf_read_fd(&b, sv[0]);
    CHECK_EQ_INT(r, 3);
    CHECK_EQ_MEM(b.data, b.len, "abc", 3);

    close(sv[1]);
    r = buf_read_fd(&b, sv[0]);
    CHECK_EQ_INT(r, 0);
    close(sv[0]);
    buf_cleanup(&b);
}

static void t_u_core_15(void)
{
    int fd[2];
    buf_t b;
    char got[3];
    CHECK_EQ_INT(pipe(fd), 0);
    CHECK_EQ_INT(buf_init(&b, 0, 32), 0);
    CHECK_EQ_INT(buf_append(&b, "a\0b", sizeof(got)), 0);
    CHECK_EQ_INT(buf_write_fd(&b, fd[1]), sizeof(got));
    CHECK_EQ_INT(b.len, 0);
    CHECK_EQ_INT(read(fd[0], got, sizeof(got)), sizeof(got));
    CHECK_EQ_MEM(got, sizeof(got), "a\0b", sizeof(got));
    close(fd[0]);
    close(fd[1]);
    buf_cleanup(&b);
}

static void t_u_core_16(void)
{
    buf_t b;
    CHECK_EQ_INT(buf_init(&b, 0, 5), 0);
    CHECK_EQ_INT(buf_appendf(&b, "%d-%s", 42, "x"), 0);
    CHECK_EQ_INT(buf_appendf(&b, "%s", "yz"), -1);
    CHECK_EQ_INT(errno, EMSGSIZE);
    CHECK_EQ_MEM(b.data, b.len, "42-x", 4);
    CHECK_EQ_INT(buf_appendf(&b, "%c", 0), 0);
    CHECK_EQ_MEM(b.data, b.len, "42-x\0", 5);
    buf_cleanup(&b);
}

static void t_u_core_17(void)
{
    char a0[] = "httpd";
    char *argv[] = {a0, NULL};
    config_t cfg;
    char err[256] = "";
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    int want_workers = ncpu >= 1 ? (ncpu > 512 ? 1024 : (int)ncpu * 2) : 4;

    CHECK_EQ_INT(enter_tmp_with_www(), 0);
    CHECK_EQ_INT(config_load(1, argv, &cfg, err, sizeof(err)), 0);
    CHECK_EQ_STR(cfg.listen_host, "0.0.0.0");
    CHECK_EQ_INT(cfg.port, 8080);
    CHECK_EQ_STR(cfg.root, "./www");
    CHECK_EQ_INT(cfg.engine, CFG_ENGINE_POOL);
    CHECK_EQ_INT(cfg.workers, want_workers);
    CHECK_EQ_INT(cfg.max_head, 8192);
    CHECK_EQ_INT(cfg.max_uri, 2048);
    CHECK_EQ_INT(cfg.max_headers, 64);
    CHECK_EQ_INT(cfg.max_body, 10485760);
    CHECK_EQ_INT(cfg.header_timeout_ms, 10000);
    CHECK_EQ_INT(cfg.body_timeout_ms, 30000);
    CHECK_EQ_INT(cfg.keepalive_timeout_ms, 5000);
    CHECK_EQ_INT(cfg.max_keepalive_requests, 100);
    CHECK_EQ_INT(cfg.n_upstreams, 0);
    CHECK_EQ_INT(cfg.n_routes, 0);
}

static void t_u_core_19(void)
{
    static const char input[] = "# comment\r\n\r\nlisten 127.0.0.1:8000\r\nroot ./www\nworkers 4\n"
                                "route /api/ proxy service strip keep_host\n"
                                "upstream service 127.0.0.1:8001 backend.local:8002\n"
                                "route / static ./www\nhost admin.local route /cgi/ cgi ./www\n"
                                "header_timeout_ms 1000\nheader_timeout_ms\t1500 # 마지막 값\n"
                                "access_log access.log\nerror_log error.log\n";
    char *argv[] = {"httpd", "-c", "server.conf", "-p", "9090", "-e", "loop", NULL};
    config_t cfg;
    char err[256] = "";

    CHECK_EQ_INT(enter_tmp_with_www(), 0);
    CHECK_EQ_INT(test_write_file(".", "server.conf", input, sizeof(input) - 1, 0644), 0);
    CHECK_EQ_INT(config_load(7, argv, &cfg, err, sizeof(err)), 0);
    CHECK_EQ_STR(cfg.listen_host, "127.0.0.1");
    CHECK_EQ_INT(cfg.port, 9090);
    CHECK_EQ_INT(cfg.engine, CFG_ENGINE_LOOP);
    CHECK_EQ_INT(cfg.workers, 4);
    CHECK_EQ_INT(cfg.header_timeout_ms, 1500);
    CHECK_EQ_INT(cfg.n_upstreams, 1);
    CHECK_EQ_INT(cfg.upstreams[0].n_backends, 2);
    CHECK_EQ_STR(cfg.upstreams[0].backends[1].host, "backend.local");
    CHECK_EQ_INT(cfg.upstreams[0].backends[1].port, 8002);
    CHECK_EQ_INT(cfg.n_routes, 3);
    CHECK_EQ_STR(cfg.routes[0].upstream, "service");
    CHECK(cfg.routes[0].strip_prefix && cfg.routes[0].keep_host);
    CHECK_EQ_STR(cfg.routes[2].host, "admin.local");
    CHECK_EQ_INT(cfg.routes[2].kind, ROUTE_CGI);
    CHECK_EQ_STR(cfg.access_log, "access.log");
    CHECK_EQ_STR(cfg.error_log, "error.log");

    static const char bad[] = "# comment\nroute / proxy missing\n";
    CHECK_EQ_INT(test_write_file(".", "server.conf", bad, sizeof(bad) - 1, 0644), 0);
    CHECK_EQ_INT(config_load(7, argv, &cfg, err, sizeof(err)), -1);
    CHECK_EQ_STR(err, "server.conf:2: undefined upstream 'missing'");
    static const char unknown[] = "# comment\nrout / static ./www\n";
    CHECK_EQ_INT(test_write_file(".", "server.conf", unknown, sizeof(unknown) - 1, 0644), 0);
    CHECK_EQ_INT(config_load(7, argv, &cfg, err, sizeof(err)), -1);
    CHECK_EQ_STR(err, "server.conf:2: unknown directive 'rout'");
}

static void t_u_core_21(void)
{
    static const char *bad[] = {"0", "70000", "abc"};
    char a0[] = "httpd";
    char a1[] = "-p";
    char a2[16];
    char *argv[] = {a0, a1, a2, NULL};
    config_t cfg;
    char err[256];

    CHECK_EQ_INT(enter_tmp_with_www(), 0);
    /* 정상 대조 (D-017) */
    (void)snprintf(a2, sizeof(a2), "%s", "9090");
    CHECK_EQ_INT(config_load(3, argv, &cfg, err, sizeof(err)), 0);
    CHECK_EQ_INT(cfg.port, 9090);
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        (void)snprintf(a2, sizeof(a2), "%s", bad[i]);
        err[0] = '\0';
        if (config_load(3, argv, &cfg, err, sizeof(err)) != -1) {
            test_fail(__FILE__, __LINE__, "-p %s should fail", bad[i]);
            return;
        }
        if (strstr(err, "-p") == NULL) {
            test_fail(__FILE__, __LINE__, "-p %s: message lacks \"-p\": \"%s\"", bad[i], err);
            return;
        }
    }
}

static ssize_t read_log(const char *path, char *out, size_t cap)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    ssize_t count = read(fd, out, cap - 1);
    close(fd);
    if (count >= 0) {
        out[count] = '\0';
    }
    return count;
}

static void t_u_core_26(void)
{
    char dir[] = "/tmp/httpd-log-test.XXXXXX";
    char path[sizeof(dir) + sizeof("/error.log")];
    char line[512];
    CHECK(mkdtemp(dir) != NULL);
    CHECK(snprintf(path, sizeof(path), "%s/error.log", dir) < (int)sizeof(path));
    CHECK_EQ_INT(log_open(NULL, path, LOG_INFO), 0);
    log_msg(LOG_DEBUG, "filtered");
    log_msg(LOG_WARN, "value %d\ncontinued\n\n", 42);
    log_close();
    CHECK(read_log(path, line, sizeof(line)) > 20);
    CHECK_EQ_STR(line + 20, " [WARN] value 42\\x0Acontinued\n");
    CHECK_EQ_INT(unlink(path), 0);
    CHECK_EQ_INT(rmdir(dir), 0);
}

static void t_u_core_27(void)
{
    char dir[] = "/tmp/httpd-log-test.XXXXXX";
    char path[sizeof(dir) + sizeof("/access.log")];
    char line[512];
    CHECK(mkdtemp(dir) != NULL);
    CHECK(snprintf(path, sizeof(path), "%s/access.log", dir) < (int)sizeof(path));
    CHECK_EQ_INT(log_open(path, NULL, LOG_ERROR), 0);
    log_access(NULL, "GET", "/test\t\"\177", 200, 5, 12);
    log_close();
    CHECK(read_log(path, line, sizeof(line)) > 20);
    CHECK_EQ_STR(line + 20, " - \"GET /test\\x09\\x22\\x7F\" 200 5 12ms\n");
    CHECK_EQ_INT(unlink(path), 0);
    CHECK_EQ_INT(rmdir(dir), 0);
}

static void t_u_core_28(void)
{
    int sv[2];

    CHECK_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    CHECK((fcntl(sv[0], F_GETFL) & O_NONBLOCK) == 0);
    CHECK_EQ_INT(platform_set_nonblock(sv[0]), 0);
    CHECK((fcntl(sv[0], F_GETFL) & O_NONBLOCK) != 0);
    close(sv[0]);
    close(sv[1]);
}

static void t_u_core_29(void)
{
    int fd = open("/dev/null", O_RDONLY);

    CHECK(fd >= 0);
    CHECK((fcntl(fd, F_GETFD) & FD_CLOEXEC) == 0);
    CHECK_EQ_INT(platform_set_cloexec(fd), 0);
    CHECK((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
    close(fd);
}

static void t_u_core_30(void)
{
    int sv[2];
    ssize_t r;

    signal(SIGPIPE, SIG_DFL); /* 구현이 SIGPIPE를 일으키면 이 자식은 시그널로 죽어 실패한다 */
    CHECK_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    close(sv[1]);
    errno = 0;
    r = platform_send(sv[0], "x", 1);
    CHECK_EQ_INT(r, -1);
    CHECK_EQ_INT(errno, EPIPE);
    close(sv[0]);
}

static void t_u_core_31(void)
{
    char dir[] = "/tmp/httpd-sendfile-test.XXXXXX";
    char path[sizeof(dir) + sizeof("/file")];
    int sv[2];
    char bytes[4];
    off_t off = 2;

    CHECK(mkdtemp(dir) != NULL);
    CHECK_EQ_INT(test_write_file(dir, "file", "01a\0b56789", 10, 0644), 0);
    CHECK(snprintf(path, sizeof(path), "%s/file", dir) < (int)sizeof(path));
    int file = open(path, O_RDONLY);
    CHECK(file >= 0);
    CHECK_EQ_INT(unlink(path), 0);
    CHECK_EQ_INT(rmdir(dir), 0);
    CHECK_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    CHECK_EQ_INT(platform_sendfile(sv[0], file, &off, 0), 0);
    CHECK_EQ_INT(off, 2);
    CHECK_EQ_INT(platform_sendfile(sv[0], -1, &off, 4), -1);
    CHECK_EQ_INT(errno, EBADF);
    CHECK_EQ_INT(off, 2);
    CHECK_EQ_INT(platform_sendfile(sv[0], file, &off, sizeof(bytes)), sizeof(bytes));
    CHECK_EQ_INT(off, 6);
    CHECK_EQ_INT(read(sv[1], bytes, sizeof(bytes)), sizeof(bytes));
    CHECK_EQ_MEM(bytes, sizeof(bytes), "a\0b5", 4);
    CHECK_EQ_INT(lseek(file, 0, SEEK_CUR), 0);
    close(sv[1]);
    CHECK_EQ_INT(platform_sendfile(sv[0], file, &off, 1), -1);
    CHECK_EQ_INT(errno, EPIPE);
    CHECK_EQ_INT(off, 6);
    close(sv[0]);
    close(file);
}

static void t_u_core_32(void)
{
    int64_t first = platform_now_ms();
    CHECK(first >= 0);
    CHECK(platform_now_ms() >= first);
}

void register_core_tests(void)
{
    test_register("U-CORE-04", "str_to_u64 accepts \"0\" and \"123\"", t_u_core_04);
    test_register("U-CORE-05", "str_to_u64 rejects empty, sign, junk, spaces", t_u_core_05);
    test_register("U-CORE-06", "str_to_u64: UINT64_MAX and overflow", t_u_core_06);
    test_register("U-CORE-08", "buf_init + buf_append \"hello\"", t_u_core_08);
    test_register("U-CORE-09", "buf_append: binary self-append during growth", t_u_core_09);
    test_register("U-CORE-10", "buf_append over max: -1, EMSGSIZE, unchanged", t_u_core_10);
    test_register("U-CORE-11", "buf_consume part / all / more than len", t_u_core_11);
    test_register("U-CORE-13", "buf_read_fd data / EOF / EAGAIN", t_u_core_13);
    test_register("U-CORE-15", "buf_write_fd: writes to a pipe and consumes bytes", t_u_core_15);
    test_register("U-CORE-16", "buf_appendf: length, binary output and rollback", t_u_core_16);
    test_register("U-CORE-17", "config_load with no args gives defaults", t_u_core_17);
    test_register("U-CORE-19", "config_load: file grammar, validation and CLI precedence",
                  t_u_core_19);
    test_register("U-CORE-21", "config_load rejects -p 0, 70000, abc", t_u_core_21);
    test_register("U-CORE-26", "error log: severity filter and one trailing LF", t_u_core_26);
    test_register("U-CORE-27", "access log: escape target bytes", t_u_core_27);
    test_register("U-CORE-28", "platform_set_nonblock sets O_NONBLOCK", t_u_core_28);
    test_register("U-CORE-29", "platform_set_cloexec sets FD_CLOEXEC", t_u_core_29);
    test_register("U-CORE-30", "platform_send to closed peer: EPIPE, no SIGPIPE", t_u_core_30);
    test_register("U-CORE-31", "platform_sendfile: offset, count and closed peer", t_u_core_31);
    test_register("U-CORE-32", "platform_now_ms: monotonic clock", t_u_core_32);
}
