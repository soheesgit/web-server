/* src/common/http_headers.h
 *
 * 헤더 끝 탐색, 헤더 줄 파싱·조회, hop-by-hop 판별, 헤더 직렬화 (요청·응답 공용).
 * 디렉토리 책임과 관련 테스트: src/common/README.md
 */
#ifndef HTTPD_COMMON_HTTP_HEADERS_H
#define HTTPD_COMMON_HTTP_HEADERS_H

#include <stdbool.h>
#include <stddef.h>

#include "common/types.h"
#include "core/buf.h"
#include "core/str.h"

/**
 * 수신 바이트에서 메시지 머리(시작 줄 + 헤더 + 빈 줄)의 끝을 찾는다.
 *
 * 1. 맨 앞의 빈 줄("\r\n" 또는 "\n")을 건너뛴다. 건너뛴 바이트 수를 *out_skip에 쓴다.
 * 2. 그 뒤에서 첫 빈 줄 종료자를 찾는다. 허용하는 종료자: "\r\n\r\n", "\n\n", "\r\n\n".
 *    ("\n\r\n"도 LF 뒤 CRLF 빈 줄로 허용한다.)
 * 3. 찾으면 DONE. *out_head_len = p부터 종료자 마지막 바이트까지의 길이 (out_skip 포함, D-024).
 *    종료자 뒤의 바이트(본문, 다음 요청)는 세지 않는다.
 * 4. head_len > max_head이면 ERROR, *out_status = 431.
 *    종료자를 못 찾았는데 len > max_head이면 ERROR 431, len ≤ max_head이면 INCOMPLETE.
 * 같은 입력을 몇 조각으로 나눠 누적하며 불러도(앞부분을 다시 넘기며) 결과가 같다. 1바이트씩
 * 늘리며 부르면 마지막 바이트 전까지 INCOMPLETE, 마지막에 DONE.
 * 예: "GET / HTTP/1.1\r\nHost: a\r\n\r\n" → DONE, skip 0, head_len 27.
 *
 * @param p             수신 바이트. len이 0이면 NULL 허용
 * @param len           바이트 수
 * @param max_head      머리 상한 (바이트)
 * @param out_skip      [out] DONE이면 앞 빈 줄 바이트 수. 그 밖에는 바꾸지 않음. NULL 불가
 * @param out_head_len  [out] DONE이면 머리 길이. 그 밖에는 바꾸지 않음. NULL 불가
 * @param out_status    [out] ERROR이면 431. 그 밖에는 바꾸지 않음. NULL 불가
 * @return              DONE, INCOMPLETE, ERROR
 *
 * 에러:    *out_status 431 (머리 상한 초과)
 * 소유권:  p를 빌림
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-COM-01, U-COM-02, U-COM-04, E-ER-06, E-ER-14 · 추후 U-COM-03, U-COM-05, U-COM-06
 */
http_parse_result_t http_find_head_end(const char *p, size_t len, size_t max_head, size_t *out_skip,
                                       size_t *out_head_len, int *out_status);

/**
 * 헤더 줄들을 파싱한다.
 *
 * p는 시작 줄 다음 바이트부터 빈 줄 직전까지다. 각 줄은 "\r\n" 또는 "\n"으로 끝나야 한다
 * (마지막 줄 포함). 줄마다 "name:value"로 나눈다:
 *   - name은 http_token_valid를 만족해야 한다. 이름과 ':' 사이 공백("Host : a")은 400.
 *   - ':'이 없는 줄은 400. SP·HTAB로 시작하는 줄(obs-fold)은 400.
 *   - value는 str_trim_ows로 앞뒤 OWS를 뗀다. 빈 값 허용.
 *   - value에 NUL, CR·LF 외의 0x00~0x1F(HTAB 제외), 0x7F가 있으면 400.
 *   - 헤더 수가 max_headers(≤ HTTP_MAX_HEADERS)를 넘으면 431.
 * 같은 이름은 합치지 않고 받은 순서대로 모두 담는다. len이 0이면 n = 0으로 성공.
 *
 * @param p            헤더 바이트. len이 0이면 NULL 허용
 * @param len          바이트 수
 * @param max_headers  헤더 개수 상한
 * @param out_h        [out] 성공 시 헤더 목록. 실패 시 정의되지 않음. NULL 불가
 * @param out_status   [out] 실패 시 400 또는 431. 성공 시 바꾸지 않음. NULL 불가
 * @return             0 성공, -1 실패
 *
 * 에러:    *out_status 400 (형식 오류), 431 (개수 초과)
 * 소유권:  out_h의 str_t는 p를 빌림
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-COM-07, U-COM-10, E-ER-13 · 추후 U-COM-08, U-COM-09, U-COM-11~15
 */
int http_parse_headers(const char *p, size_t len, size_t max_headers, http_headers_t *out_h,
                       int *out_status);

/**
 * 이름으로 헤더를 찾는다 (ASCII 대소문자 무시).
 *
 * 같은 이름이 여럿이면 첫 번째를 돌려준다.
 *
 * @param h     헤더 목록. NULL 불가
 * @param name  찾을 이름, NUL 종료. NULL 불가
 * @return      찾은 헤더 (h 안을 가리킴), 없으면 NULL
 *
 * 에러:    없음
 * 소유권:  h를 빌린 포인터를 돌려준다
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  추후 U-COM-16
 */
const http_header_t *http_header_get(const http_headers_t *h, const char *name);

/**
 * 이름이 같은 헤더의 개수를 센다 (ASCII 대소문자 무시).
 *
 * @param h     헤더 목록. NULL 불가
 * @param name  이름, NUL 종료. NULL 불가
 * @return      개수 (0 이상)
 *
 * 에러:    없음
 * 소유권:  빌림
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-DS-12 (request_parse_head를 통해)
 */
size_t http_header_count(const http_headers_t *h, const char *name);

/**
 * 문자열이 RFC 7230 token(tchar 1개 이상)인지 판정한다.
 *
 * tchar = "!" / "#" / "$" / "%" / "&" / "'" / "*" / "+" / "-" / "." / "^" / "_" / "`" / "|" / "~"
 *         / DIGIT / ALPHA. 길이 0이면 false.
 *
 * @param s  검사할 뷰
 * @return   token이면 true
 *
 * 에러:    없음
 * 소유권:  빌림
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-DS-07 (request_parse_head를 통해) · 추후 U-COM-13, U-DS-10
 */
bool http_token_valid(str_t s);

/**
 * 헤더 이름이 hop-by-hop인지 판정한다 (ASCII 대소문자 무시).
 *
 * 고정 목록: Connection, Keep-Alive, Proxy-Connection, TE, Trailer, Transfer-Encoding, Upgrade,
 * Proxy-Authenticate, Proxy-Authorization. 그리고 h의 모든 Connection 헤더 값에
 * 쉼표로 나열된 이름 (각 항목은 OWS를 떼고 비교). h가 NULL이면 고정 목록만 본다.
 *
 * @param name  검사할 이름
 * @param h     같은 메시지의 헤더 목록 (Connection 값 확인용). NULL 허용
 * @return      hop-by-hop이면 true
 *
 * 에러:    없음
 * 소유권:  빌림
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-COM-40, E-PX-03 · 추후 U-COM-41
 */
bool http_is_hop_by_hop(str_t name, const http_headers_t *h);

/**
 * 헤더 목록을 "Name: value\r\n" 줄들로 out 끝에 덧붙인다.
 *
 * 입력 순서를 지키고 이름의 대소문자를 바꾸지 않는다. skip_hop_by_hop이면
 * http_is_hop_by_hop(name, h)인 헤더를 건너뛴다. 빈 줄은 붙이지 않는다.
 * 실패하면 out을 호출 전 len으로 되돌린다.
 *
 * @param h                헤더 목록. NULL 불가
 * @param skip_hop_by_hop  hop-by-hop 헤더를 뺄지
 * @param out              대상 버퍼 (buf_init됨). NULL 불가
 * @return                 0 성공, -1 실패
 *
 * 에러:    EMSGSIZE·ENOMEM (buf_append)
 * 소유권:  h를 빌려 out에 복사
 * 스레드:  [per-object] (out 기준)
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  추후 U-COM-42, U-COM-43
 */
int http_headers_write(const http_headers_t *h, bool skip_hop_by_hop, buf_t *out);

#endif
