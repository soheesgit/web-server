/* src/common/http_status.h
 *
 * 상태 코드의 사유 문구와 에러 페이지 본문.
 * 디렉토리 책임과 관련 테스트: src/common/README.md
 */
#ifndef HTTPD_COMMON_HTTP_STATUS_H
#define HTTPD_COMMON_HTTP_STATUS_H

#include "core/buf.h"

/**
 * 상태 코드의 사유 문구를 돌려준다 (RFC 9110 이름).
 *
 * 최소한 다음을 안다: 100 Continue, 200 OK, 201 Created, 204 No Content,
 * 301 Moved Permanently, 302 Found, 304 Not Modified, 400 Bad Request, 403 Forbidden,
 * 404 Not Found, 405 Method Not Allowed, 408 Request Timeout, 413 Content Too Large,
 * 414 URI Too Long, 431 Request Header Fields Too Large, 500 Internal Server Error,
 * 501 Not Implemented, 502 Bad Gateway, 503 Service Unavailable, 504 Gateway Timeout,
 * 505 HTTP Version Not Supported. 모르는 코드는 "Unknown".
 *
 * @param status  상태 코드
 * @return        정적 문자열 (해제 금지). NULL을 돌려주지 않음
 *
 * 에러:    없음
 * 소유권:  정적 메모리
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-DS-30 (response_write_head를 통해) · 추후 U-COM-44
 */
const char *http_reason(int status);

/**
 * 에러 페이지 HTML을 out 끝에 덧붙인다.
 *
 * 내용: "<!doctype html><title>NNN 사유</title><h1>NNN 사유</h1>\n" 형식 (NNN = 3자리 상태,
 * 사유 = http_reason). 길이는 항상 0보다 크다. Content-Type은 호출자가
 * "text/html; charset=utf-8"로 붙인다. 실패하면 out을 호출 전 len으로 되돌린다.
 *
 * @param status  상태 코드
 * @param out     대상 버퍼 (buf_init됨). NULL 불가
 * @return        0 성공, -1 실패
 *
 * 에러:    EMSGSIZE·ENOMEM
 * 소유권:  out에 쓴다
 * 스레드:  [per-object] (out 기준)
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-HD-02 (plan_error를 통해), E-ST-04 · 추후 U-COM-45
 */
int http_error_body(int status, buf_t *out);

#endif
