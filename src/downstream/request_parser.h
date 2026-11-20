/* src/downstream/request_parser.h
 *
 * 요청 라인과 헤더를 파싱해 http_request_t를 만든다.
 * 디렉토리 책임과 관련 테스트: src/downstream/README.md
 */
#ifndef HTTPD_DOWNSTREAM_REQUEST_PARSER_H
#define HTTPD_DOWNSTREAM_REQUEST_PARSER_H

#include <stddef.h>

#include "common/types.h"

/**
 * 수신 버퍼 앞부분에서 요청 머리(요청 라인 + 헤더 + 빈 줄)를 파싱한다.
 *
 * 순서와 규칙 (먼저 걸린 규칙의 상태 코드):
 *  1. 머리 끝: http_find_head_end(p, len, lim->max_head). INCOMPLETE·ERROR(431)는 그대로 돌려준다.
 *     재구현 금지. 단, 첫 줄(요청 라인)이 끝나기도 전에 max_uri + 16바이트를 넘겼으면 414.
 *  2. 요청 라인: p + skip부터 첫 줄 끝(CRLF 또는 LF)까지. "메서드 SP 대상 SP 버전" 정확히 세 토큰,
 *     구분자는 SP 하나. 토큰 수가 3이 아니거나 연속 공백·HTAB·앞뒤 공백이면 400.
 *  3. 메서드: http_token_valid가 아니면 400. 대소문자 구분으로 "GET"·"HEAD"·"POST"만 지원,
 *     그 밖의 유효한 토큰("PUT", "DELETE", "get", "CONNECT")은 501. method_raw에 원본.
 *  4. 버전: "HTTP/1.0"·"HTTP/1.1"만 허용(minor 0·1). "HTTP/<숫자>.<숫자>"이지만 1.0·1.1이 아니면
 *     505 ("HTTP/2.0", "HTTP/1.2"). 그 밖의 형식("HTTP/1", "HTTP/1.x", "HTTX/1.1")은 400.
 *  5. 대상 길이가 lim->max_uri를 넘으면 414.
 *  6. 대상 형식: '/'로 시작하면 origin-form. "http://"로 시작하면(스킴 대소문자 무시)
 *     absolute-form: uri_parse_absolute로 authority와 나머지를 나눠 host = authority.
 *     "*"(asterisk-form)과 authority-form("a.com:443")은 501. 그 밖은 400.
 *  7. 경로: uri_split → raw_path·query. uri_decode_path(raw_path) → path_normalize → path.
 *     디코딩 실패(잘못된 %XX, %00, 길이 초과)는 400, 루트 위 이탈은 400 (D-001). %2F는 '/'로
 *     디코딩한 뒤 정규화한다 (D-002).
 *  8. 헤더: 요청 라인 다음부터 빈 줄 직전까지 http_parse_headers(lim->max_headers). 400·431.
 *  9. Host: origin-form이고 minor 1이면 Host 헤더가 정확히 하나여야 한다(없거나 2개 이상 400,
 *     http_header_count). absolute-form이면 URI의 authority가 Host보다 우선한다. 1.0은 없어도 됨.
 * 10. 본문 길이: http_resolve_framing(is_response = false). 오류 400. CHUNKED면 501 (D-003).
 *     LENGTH가 lim->max_body를 넘으면 413 — 본문을 기다리지 않고 머리 단계에서 판정한다.
 * 11. keep_alive = http_keep_alive(minor, headers). head_len = http_find_head_end의 head_len.
 *     body는 채우지 않는다 (conn이 채움). 본문 바이트는 p[head_len..]에서 시작한다.
 * 파이프라이닝: 버퍼에 다음 요청이 붙어 있어도 첫 요청만 파싱하고 head_len은 첫 요청까지.
 * 같은 입력을 1바이트씩 늘려 가며 불러도 DONE 시점과 결과가 같다.
 *
 * @param p           수신 버퍼 앞부분. len이 0이면 NULL 허용
 * @param len         바이트 수
 * @param lim         제한값. NULL 불가
 * @param out_req     [out] DONE이면 채운 요청. 그 밖에는 정의되지 않음. NULL 불가
 * @param out_status  [out] ERROR이면 상태 코드(400·413·414·431·501·505). NULL 불가
 * @return            DONE, INCOMPLETE, ERROR
 *
 * 에러:    *out_status 위 목록
 * 소유권:  out_req의 str_t는 p를 빌림. path는 out_req가 소유
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-DS-01, U-DS-06, U-DS-07, U-DS-12, E-ER-01~09, E-ER-12~14, E-ST-06, E-ST-10 ·
 *          추후 U-DS-02~05, U-DS-08~11, U-DS-13~20
 */
http_parse_result_t request_parse_head(const char *p, size_t len, const http_limits_t *lim,
                                       http_request_t *out_req, int *out_status);

#endif
