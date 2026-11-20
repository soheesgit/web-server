/* src/downstream/response_writer.h
 *
 * 응답 상태 줄·헤더 조립과 chunked 프레이밍.
 * 디렉토리 책임과 관련 테스트: src/downstream/README.md
 */
#ifndef HTTPD_DOWNSTREAM_RESPONSE_WRITER_H
#define HTTPD_DOWNSTREAM_RESPONSE_WRITER_H

#include <stddef.h>
#include <time.h>

#include "common/types.h"
#include "core/buf.h"

/**
 * 응답 머리를 직렬화해 out 끝에 덧붙인다.
 *
 * 순서 (모든 줄 끝 CRLF):
 *  1. "HTTP/1.1 <3자리 코드> <http_reason(code)>" — 클라이언트 버전과 무관하게 항상 1.1.
 *  2. "Date: <IMF-fixdate>" — now를 gmtime_r로 바꾼 "Tue, 14 Nov 2023 22:13:20 GMT" 형식.
 *  3. "Server: httpd/0.1"
 *  4. head->fields 그대로.
 *  5. 길이: framing이 LENGTH면 "Content-Length: <n>". CHUNKED면 클라이언트가 1.1일 때
 *     "Transfer-Encoding: chunked", 1.0이면 둘 다 넣지 않고 연결 종료로 끝낸다(아래 6에서
 *     close로 바뀜). NONE·CLOSE는 길이 헤더 없음. status가 204·304·1xx면 framing과 무관하게
 *     Content-Length를 붙이지 않는다.
 *  6. 연결: close가 true이거나 (1.0 + CHUNKED) 또는 framing CLOSE이면 "Connection: close".
 *     그렇지 않고 client_minor가 0이면(1.0 클라이언트를 유지) "Connection: keep-alive".
 *     1.1을 유지하면 Connection 헤더를 넣지 않는다.
 *  7. 빈 줄.
 * 실패하면 out을 호출 전 len으로 되돌린다.
 * 예 (now 1700000000, 200, fields "Content-Type: text/html; charset=utf-8\r\n", LENGTH 5, 1.1):
 *   "HTTP/1.1 200 OK\r\nDate: Tue, 14 Nov 2023 22:13:20 GMT\r\nServer: httpd/0.1\r\n"
 *   "Content-Type: text/html; charset=utf-8\r\nContent-Length: 5\r\n\r\n"
 *
 * @param head          응답 머리. NULL 불가
 * @param client_minor  클라이언트 HTTP/1.minor
 * @param now           벽시계 (초)
 * @param out           대상 버퍼 (buf_init됨). NULL 불가
 * @return              0 성공, -1 실패
 *
 * 에러:    EMSGSIZE·ENOMEM (out 상한), EINVAL (status가 100~599 밖)
 * 소유권:  head를 빌려 out에 쓴다
 * 스레드:  [per-object] (out 기준)
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-DS-30, E-ST-01, E-KA-03, E-KA-04, E-KA-05, E-KA-07, E-CG-08 · 추후 U-DS-31~35
 */
int response_write_head(const http_head_out_t *head, int client_minor, time_t now, buf_t *out);

/**
 * chunked 청크 하나를 out 끝에 덧붙인다: "<소문자 16진 n>\r\n<data>\r\n".
 *
 * n이 0이면 아무것도 쓰지 않고 0 (마지막 청크는 response_write_last_chunk).
 * 실패하면 out을 호출 전 len으로 되돌린다.
 *
 * @param p    데이터. n이 0이면 NULL 허용
 * @param n    바이트 수
 * @param out  대상 버퍼. NULL 불가
 * @return     0 성공, -1 실패
 *
 * 에러:    EMSGSIZE·ENOMEM
 * 소유권:  p를 빌려 복사
 * 스레드:  [per-object]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  E-CG-08
 */
int response_write_chunk(const void *p, size_t n, buf_t *out);

/**
 * 마지막 청크 "0\r\n\r\n"을 out 끝에 덧붙인다.
 *
 * @param out  대상 버퍼. NULL 불가
 * @return     0 성공, -1 실패
 *
 * 에러:    EMSGSIZE·ENOMEM
 * 소유권:  없음
 * 스레드:  [per-object]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  E-CG-08
 */
int response_write_last_chunk(buf_t *out);

#endif
