/* src/upstream/response_parser.h
 *
 * 업스트림 응답의 상태 줄과 헤더를 파싱한다.
 * 디렉토리 책임과 관련 테스트: src/upstream/README.md
 */
#ifndef HTTPD_UPSTREAM_RESPONSE_PARSER_H
#define HTTPD_UPSTREAM_RESPONSE_PARSER_H

#include <stddef.h>

#include "common/types.h"

/**
 * 업스트림 수신 버퍼 앞부분에서 응답 머리를 파싱한다.
 *
 * 1. 머리 끝: http_find_head_end(p, len, lim->max_head). INCOMPLETE는 그대로, 431은 ERROR 502.
 * 2. 상태 줄: "HTTP/1.<digit> SP <3자리 숫자> [SP 사유]". 사유는 비어 있어도 된다(끝 공백 허용).
 *    3자리가 아니거나 100~599 밖("99", "2x0"), "HTTP/"가 아님("FOO/1.1"), minor가 숫자 아님은
 *    ERROR 502. 앞의 빈 줄은 http_find_head_end가 건너뛴다(줄 앞 공백은 오류).
 * 3. 헤더: http_parse_headers(lim->max_headers). 오류면 ERROR 502.
 * 4. framing: http_resolve_framing(is_response = true, status, req_method). 오류면 ERROR 502.
 *    HEAD 요청에 대한 응답·1xx·204·304는 NONE.
 * 5. keep_alive = minor ≥ 1이고 Connection에 close가 없고 framing이 CLOSE가 아님
 *    (1.0은 keep-alive 명시가 있을 때만).
 * 6. head_len = 머리 길이. 1xx도 DONE으로 돌려주며 호출자(relay)가 head_len만큼 버리고 다시
 *    파싱한다.
 *
 * @param p           업스트림 수신 바이트. len이 0이면 NULL 허용
 * @param len         바이트 수
 * @param req_method  이 응답을 부른 요청의 메서드
 * @param lim         한도 (max_head, max_headers). NULL 불가
 * @param out_resp    [out] DONE이면 응답 머리 (str_t는 p를 빌림). NULL 불가
 * @param out_status  [out] ERROR이면 502. NULL 불가
 * @return            DONE, INCOMPLETE, ERROR
 *
 * 에러:    *out_status 502
 * 소유권:  out_resp의 str_t는 p를 빌림
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-US-09, U-US-12, E-PX-11 · 추후 U-US-10, U-US-11, U-US-13~16
 */
http_parse_result_t response_parse_head(const char *p, size_t len, http_method_t req_method,
                                        const http_limits_t *lim, http_response_t *out_resp,
                                        int *out_status);

#endif
