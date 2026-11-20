/* src/core/str.h
 *
 * 빌린 문자열 뷰 str_t와 비교·트림·숫자 변환.
 * 디렉토리 책임과 관련 테스트: src/core/README.md
 */
#ifndef HTTPD_CORE_STR_H
#define HTTPD_CORE_STR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * 다른 메모리를 빌려 보는 문자열 뷰 (포인터 + 길이).
 *
 * 생성:   str_from_cstr, STR_LIT, 또는 파서가 수신 버퍼의 구간을 가리키도록 직접 채움
 * 소멸:   없음 (해제할 것이 없다)
 * 수명:   ptr이 가리키는 원래 메모리의 수명을 따른다. 수신 버퍼를 빌린 뷰는 그 버퍼를
 *         buf_consume하거나 재할당하기 전까지만 유효하다
 * 스레드: 값 타입. 가리키는 메모리가 바뀌지 않는 동안 공유 가능
 */
typedef struct {
    const char *ptr; /* 첫 바이트. NUL 종료를 보장하지 않음. len이 0이면 NULL일 수 있음 */
    size_t len; /* 바이트 수. 빈 문자열은 0 */
} str_t;

/* 문자열 리터럴로 str_t를 만든다. 인자는 반드시 리터럴이어야 한다 (sizeof 사용). */
#define STR_LIT(s) ((str_t){(s), sizeof(s) - 1})

/**
 * NUL 종료 문자열을 빌려 str_t를 만든다.
 *
 * 길이는 strlen(s). s가 NULL이면 {NULL, 0}을 돌려준다.
 *
 * @param s  NUL 종료 문자열. NULL 허용
 * @return   s를 빌린 뷰
 *
 * 에러:    없음
 * 소유권:  s를 빌린다. 할당 없음
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  (테스트 없음)
 */
str_t str_from_cstr(const char *s);

/**
 * 뷰가 리터럴과 바이트 단위로 같은지 비교한다 (대소문자 구분).
 *
 * 길이가 strlen(lit)과 같고 모든 바이트가 같으면 true. s.len이 0이면 lit이 ""일 때만 true.
 *
 * @param s    비교할 뷰. s.ptr은 s.len이 0이면 NULL 허용
 * @param lit  NUL 종료 문자열. NULL 불가
 * @return     같으면 true
 *
 * 에러:    없음
 * 소유권:  둘 다 빌림
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  (테스트 없음)
 */
bool str_eq(str_t s, const char *lit);

/**
 * 뷰와 리터럴을 ASCII 대소문자를 무시하고 비교한다.
 *
 * 'A'~'Z'와 'a'~'z'만 같은 것으로 본다. 그 밖의 바이트(UTF-8 포함)는 그대로 비교한다.
 * 로케일을 쓰지 않는다. 길이가 다르면 false, 빈 문자열끼리는 true.
 * 예: "Content-Length" vs "content-LENGTH" → true.
 *
 * @param s    비교할 뷰
 * @param lit  NUL 종료 문자열. NULL 불가
 * @return     같으면 true
 *
 * 에러:    없음
 * 소유권:  둘 다 빌림
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  추후 U-CORE-01, U-CORE-02
 */
bool str_eq_ci(str_t s, const char *lit);

/**
 * 두 뷰를 ASCII 대소문자를 무시하고 비교한다.
 *
 * 규칙은 str_eq_ci와 같다. 길이가 다르면 false, 둘 다 길이 0이면 true.
 *
 * @param a  첫 뷰
 * @param b  둘째 뷰
 * @return   같으면 true
 *
 * 에러:    없음
 * 소유권:  둘 다 빌림
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  (테스트 없음)
 */
bool str_eq_ci_str(str_t a, str_t b);

/**
 * 앞뒤의 OWS(SP 0x20, HTAB 0x09)를 뗀 뷰를 돌려준다.
 *
 * 다른 공백 문자(CR, LF, VT 등)는 떼지 않는다. 결과는 s의 부분 구간이다.
 * 공백만 있거나 빈 입력이면 길이 0 뷰를 돌려준다 (ptr은 s.ptr 범위 안 또는 NULL).
 * 예: " \t value \t" → "value".
 *
 * @param s  입력 뷰
 * @return   s의 부분 구간
 *
 * 에러:    없음
 * 소유권:  s를 빌린 뷰를 돌려준다
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  추후 U-CORE-03
 */
str_t str_trim_ows(str_t s);

/**
 * 10진 숫자열을 uint64_t로 변환한다.
 *
 * s 전체가 '0'~'9'로만 이루어져야 한다. 다음은 모두 실패(-1)다: 빈 값, 부호('+', '-'),
 * 앞뒤 공백, 숫자가 아닌 문자, UINT64_MAX 오버플로우, 결과가 max 초과.
 * 앞자리 0은 허용한다 ("007" → 7). 실패하면 *out_v를 바꾸지 않는다.
 * 예: "0" → 0, "123" → 123, "18446744073709551615"(max=UINT64_MAX) → 성공,
 *     "18446744073709551616" → -1, "10485761"(max=10485760) → -1.
 *
 * @param s      입력 뷰
 * @param max    허용하는 최댓값 (포함)
 * @param out_v  [out] 성공 시 변환 값. 실패 시 그대로. NULL 불가
 * @return       0 성공, -1 실패
 *
 * 에러:    -1일 때 errno = EINVAL(형식 오류) 또는 ERANGE(오버플로우·max 초과)
 * 소유권:  s를 빌림
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-CORE-04, U-CORE-05 · 추후 U-CORE-06, U-CORE-07
 */
int str_to_u64(str_t s, uint64_t max, uint64_t *out_v);

/**
 * 뷰를 NUL 종료 문자열로 복사한다.
 *
 * s.len + 1 <= cap이면 s를 복사하고 NUL을 붙인다. 자리가 부족하면 아무것도 쓰지 않고
 * (cap > 0이면 out[0] = '\0'만 쓴다) -1을 돌려준다. s 안의 NUL 바이트는 그대로 복사한다.
 *
 * @param s    입력 뷰
 * @param out  [out] 결과 버퍼. cap > 0이면 NULL 불가
 * @param cap  out의 크기 (바이트, NUL 포함)
 * @return     0 성공, -1 자리 부족
 *
 * 에러:    -1일 때 errno = ENAMETOOLONG
 * 소유권:  s를 빌리고 out에 복사한다. 할당 없음
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  (테스트 없음)
 */
int str_to_cstr(str_t s, char *out, size_t cap);

#endif
