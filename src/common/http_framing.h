/* src/common/http_framing.h
 *
 * 본문 길이 판정, 본문 경계 추적, keep-alive 판단 (요청·응답 공용).
 * 디렉토리 책임과 관련 테스트: src/common/README.md
 */
#ifndef HTTPD_COMMON_HTTP_FRAMING_H
#define HTTPD_COMMON_HTTP_FRAMING_H

#include <stdbool.h>
#include <stddef.h>

#include "common/types.h"

/**
 * 헤더로 메시지 본문의 길이 방식을 정한다 (RFC 7230 3.3.3).
 *
 * 요청 (is_response = false, status·req_method 무시):
 *   - Transfer-Encoding과 Content-Length가 함께 있으면 -1, 400 (smuggling 방지).
 *   - Transfer-Encoding이 있으면: 쉼표로 나눈 마지막 코딩이 "chunked"(대소문자 무시)면 CHUNKED,
 *     아니면 -1, 400. (chunked 요청을 501로 거부하는 것은 request_parse_head의 정책)
 *   - Content-Length: 값이 str_to_u64로 해석되는 10진수여야 한다. 여러 헤더나 "5, 5"처럼
 *     쉼표로 나열된 값은 모두 같으면 그 값, 하나라도 다르거나 형식 오류("-1", "abc", "", "1 2")면
 *     -1, 400. 성공하면 LENGTH (0도 LENGTH 0).
 *   - 둘 다 없으면 NONE.
 * 응답 (is_response = true):
 *   - req_method가 HEAD이거나 status가 1xx·204·304면 헤더와 무관하게 NONE.
 *   - 그 밖에는 위와 같은 TE·CL 규칙(오류는 -1, *out_status 502), 둘 다 없으면 CLOSE.
 *
 * @param h            헤더 목록. NULL 불가
 * @param is_response  응답 헤더인가
 * @param status       응답 상태 코드 (응답일 때만 사용)
 * @param req_method   이 응답을 부른 요청 메서드 (응답일 때만 사용)
 * @param out_f        [out] 성공 시 방식과 길이. 실패 시 바꾸지 않음. NULL 불가
 * @param out_status   [out] 실패 시 400(요청) 또는 502(응답). NULL 불가
 * @return             0 성공, -1 실패
 *
 * 에러:    *out_status 400 / 502
 * 소유권:  빌림
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-COM-18, U-COM-21, U-COM-26, E-ER-08 · 추후 U-COM-17, U-COM-19, U-COM-20, U-COM-22~28
 */
int http_resolve_framing(const http_headers_t *h, bool is_response, int status,
                         http_method_t req_method, http_framing_t *out_f, int *out_status);

/**
 * 본문 경계 추적기를 초기화한다.
 *
 * LENGTH면 remaining = f.length, CHUNKED면 state 0(청크 크기 줄 대기), 그 밖에는 0.
 *
 * @param t  초기화할 추적기. NULL 불가
 * @param f  추적할 방식
 *
 * 에러:    없음
 * 소유권:  없음
 * 스레드:  [per-object]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-COM-29, U-COM-32 (http_body_track과 함께)
 */
void http_body_tracker_init(http_body_tracker_t *t, http_framing_t f);

/**
 * 새로 받은 바이트 중 본문에 속하는 만큼을 세고 본문이 끝났는지 알려 준다.
 *
 * 바이트를 복사하지 않는다. *out_used는 p 중 이 본문에 속한 바이트 수이며, 그 뒤의 바이트
 * (다음 메시지)는 쓰지 않는다. 호출자는 p[0..used)를 그대로 전달하면 된다(chunked도 원본 그대로).
 *   - NONE: 즉시 DONE, used 0.
 *   - LENGTH: min(len, remaining)을 쓰고 remaining이 0이 되면 DONE, 아니면 MORE.
 *     LENGTH 0이면 len과 무관하게 즉시 DONE, used 0.
 *   - CHUNKED: chunk-size(16진, 16자리 이하) [; ext] CRLF, data, CRLF 반복, "0" CRLF,
 *     트레일러 줄들, 빈 줄까지. 마지막 빈 줄까지 세면 DONE. 크기 형식 오류, 16자리 초과,
 *     데이터 뒤 CRLF 누락이면 ERROR. 줄 끝은 CRLF만 허용(LF 단독도 허용).
 *   - CLOSE: 항상 MORE, used = len (끝은 EOF가 정한다).
 * 같은 입력을 어떻게 나눠 넣어도 used 합계와 DONE 시점이 같다.
 *
 * @param t         추적기. NULL 불가
 * @param p         새 바이트. len이 0이면 NULL 허용
 * @param len       바이트 수
 * @param out_used  [out] 본문에 속한 바이트 수 (ERROR여도 그 직전까지). NULL 불가
 * @return          MORE, DONE, ERROR
 *
 * 에러:    BODY_ERROR (chunked 형식 오류)
 * 소유권:  p를 빌림
 * 스레드:  [per-object]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-COM-29, U-COM-32, E-PX-05 · 추후 U-COM-30, U-COM-31, U-COM-33~37
 */
http_body_result_t http_body_track(http_body_tracker_t *t, const char *p, size_t len,
                                   size_t *out_used);

/**
 * 메시지 후 연결을 유지할지 판단한다.
 *
 * 모든 Connection 헤더 값을 쉼표로 나눠 OWS를 떼고 대소문자 무시로 본다.
 *   - "close" 항목이 하나라도 있으면 false.
 *   - minor ≥ 1이면 true.
 *   - minor 0이면 "keep-alive" 항목이 있을 때만 true.
 * 예: 1.1 없음 → true, "close"·"Close"·"keep-alive, close" → false, 1.0 없음 → false,
 *     1.0 "keep-alive" → true.
 *
 * @param minor  HTTP/1.minor
 * @param h      헤더 목록. NULL 불가
 * @return       유지하면 true
 *
 * 에러:    없음
 * 소유권:  빌림
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-COM-38, E-KA-03, E-KA-04, E-KA-05 · 추후 U-COM-39
 */
bool http_keep_alive(int minor, const http_headers_t *h);

#endif
