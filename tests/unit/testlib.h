/* tests/unit/testlib.h
 *
 * C 단위·계약 테스트 라이브러리: 등록, CHECK 매크로, 임시 디렉토리 도우미.
 * 사용법: docs/TESTING.md "테스트 추가 방법"
 */
#ifndef HTTPD_TESTS_TESTLIB_H
#define HTTPD_TESTS_TESTLIB_H

#include <stddef.h>
#include <string.h>

#include "core/str.h"

typedef void (*test_fn)(void);

/* 테스트 하나를 등록한다. id는 기획서 ID ("U-CORE-04"), name은 한 줄 설명. 문자열은 빌림. */
void test_register(const char *id, const char *name, test_fn fn);

/* 현재 테스트를 실패로 표시하고 메시지를 남긴다 (CHECK 매크로가 부름). */
void test_fail(const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* 현재 테스트를 건너뛴다 (예: root 권한). 돌아오지 않는다. */
void test_skip(const char *reason) __attribute__((noreturn));

/* mkdtemp로 임시 디렉토리를 만들어 경로를 돌려준다 (내부 정적 표, 프로세스 끝까지 유효,
 * 테스트 하나에서 최대 8번). 실패하면 NULL. 테스트는 fork된 자식에서 돌므로 정리하지 않아도
 * 된다. */
const char *test_tmpdir(void);

/* dir/name 파일에 data를 쓴다. 0 성공, -1 실패. */
int test_write_file(const char *dir, const char *name, const void *data, size_t n, int mode);

/* 실패하면 메시지(파일:줄, 기대·실제)를 남기고 현재 테스트 함수에서 return */
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            test_fail(__FILE__, __LINE__, "CHECK(%s)", #cond);                                     \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define CHECK_EQ_INT(actual, expected)                                                             \
    do {                                                                                           \
        long long a_ = (long long)(actual);                                                        \
        long long e_ = (long long)(expected);                                                      \
        if (a_ != e_) {                                                                            \
            test_fail(__FILE__, __LINE__, "%s: expected %lld, got %lld", #actual, e_, a_);         \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define CHECK_EQ_STR(actual_cstr, expected_cstr)                                                   \
    do {                                                                                           \
        const char *a_ = (actual_cstr);                                                            \
        const char *e_ = (expected_cstr);                                                          \
        if (a_ == NULL || strcmp(a_, e_) != 0) {                                                   \
            test_fail(__FILE__, __LINE__, "%s: expected \"%s\", got \"%s\"", #actual_cstr, e_,     \
                      a_ == NULL ? "(null)" : a_);                                                 \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define CHECK_EQ_MEM(ap, alen, bp, blen)                                                           \
    do {                                                                                           \
        size_t al_ = (size_t)(alen);                                                               \
        size_t bl_ = (size_t)(blen);                                                               \
        if (al_ != bl_ || (al_ > 0 && ((ap) == NULL || memcmp((ap), (bp), al_) != 0))) {           \
            test_fail(__FILE__, __LINE__, "%s: %zu bytes differ from expected %zu bytes", #ap,     \
                      al_, bl_);                                                                   \
            return;                                                                                \
        }                                                                                          \
    } while (0)

/* str_t 비교 */
#define CHECK_STR_EQ(str_t_actual, expected_cstr)                                                  \
    do {                                                                                           \
        str_t s_ = (str_t_actual);                                                                 \
        const char *e_ = (expected_cstr);                                                          \
        size_t el_ = strlen(e_);                                                                   \
        if (s_.len != el_ || (el_ > 0 && (s_.ptr == NULL || memcmp(s_.ptr, e_, el_) != 0))) {      \
            test_fail(__FILE__, __LINE__, "%s: expected \"%s\", got \"%.*s\"", #str_t_actual, e_,  \
                      (int)s_.len, s_.ptr == NULL ? "" : s_.ptr);                                  \
            return;                                                                                \
        }                                                                                          \
    } while (0)

/* ── 러너(run_unit.c) 전용 ─────────────────────────────────────────────── */
#define TEST_MAX 512      /* 등록 가능한 테스트 수 */
#define TEST_EXIT_SKIP 77 /* test_skip이 쓰는 자식 종료 코드 */

typedef struct {
    const char *id;   /* 테스트 ID */
    const char *name; /* 한 줄 설명 */
    test_fn fn;       /* 테스트 함수 */
} test_entry_t;

size_t testlib_count(void);          /* 등록된 테스트 수 */
test_entry_t *testlib_entries(void); /* 등록 표 */
int testlib_failed(void);            /* 현재 프로세스에서 실패가 기록됐는가 */

/* 각 contract_min/test_<area>.c가 정의 */
void register_core_tests(void);
void register_common_tests(void);
void register_downstream_tests(void);
void register_router_tests(void);
void register_handler_tests(void);
void register_upstream_tests(void);
void register_engine_tests(void);

#endif
