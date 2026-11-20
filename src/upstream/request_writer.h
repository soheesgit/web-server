/* src/upstream/request_writer.h
 *
 * 클라이언트 요청을 업스트림으로 보낼 바이트로 직렬화한다.
 * 디렉토리 책임과 관련 테스트: src/upstream/README.md
 */
#ifndef HTTPD_UPSTREAM_REQUEST_WRITER_H
#define HTTPD_UPSTREAM_REQUEST_WRITER_H

#include <stdbool.h>

#include "common/types.h"
#include "core/buf.h"
#include "core/str.h"

/**
 * 업스트림 요청을 만들 때의 재작성 규칙.
 *
 * 생성:   proxy_begin이 채움 (테스트는 직접)
 * 소멸:   없음 (값 타입)
 * 수명:   request_write_upstream 호출 동안. 모든 str_t는 빌림
 * 스레드: 값 타입
 */
typedef struct {
    str_t client_ip;     /* X-Forwarded-For에 붙일 값 ("127.0.0.1") */
    str_t upstream_host; /* keep_host가 false일 때 Host 값 (backend_hostport) */
    str_t strip_prefix;  /* raw_path 앞에서 뗄 접두사. 길이 0이면 strip 안 함 */
    bool keep_host;      /* true면 원래 Host(req->host) 유지 */
} upstream_rewrite_t;

/**
 * 업스트림 요청 바이트를 out 끝에 쓴다.
 *
 * 1. 요청 라인: "<method_raw> <경로>[?<query>] HTTP/1.1\r\n". 경로는 디코딩하지 않은 raw_path
 *    (재인코딩 금지, D-002). strip_prefix가 있고 raw_path가 그것으로 시작하면 떼고, 결과가 비거나
 *    '/'로 시작하지 않으면 앞에 '/'를 붙인다. 클라이언트가 1.0이어도 항상 HTTP/1.1.
 * 2. "Host: <keep_host ? req->host : upstream_host>".
 * 3. 클라이언트 헤더를 순서대로 복사하되 http_is_hop_by_hop인 것(Connection에 나열된 이름 포함),
 *    Host, X-Forwarded-For, X-Forwarded-Host, X-Forwarded-Proto는 뺀다. Content-Length는 유지.
 * 4. "X-Forwarded-For: <기존 값들을 ", "로 이은 것>, <client_ip>" (기존이 없으면 client_ip만).
 *    "X-Forwarded-Host: <req->host>", "X-Forwarded-Proto: http".
 * 5. Connection 헤더는 넣지 않는다 (1.1 기본 유지 — 풀 재사용, U-US-08).
 * 6. 빈 줄, 그다음 req->body 그대로.
 * 실패하면 out을 호출 전 len으로 되돌린다.
 *
 * @param req  요청 (본문 포함). NULL 불가
 * @param rw   재작성 규칙. NULL 불가
 * @param out  대상 버퍼 (buf_init됨, 상한 ≥ max_head + max_body + 여유). NULL 불가
 * @return     0 성공, -1 실패
 *
 * 에러:    EMSGSIZE·ENOMEM
 * 소유권:  req·rw를 빌려 out에 복사
 * 스레드:  [thread-safe] (out이 다르면)
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-US-01, U-PX-01, E-PX-02, E-PX-03, E-PX-04 · 추후 U-US-02~08
 */
int request_write_upstream(const http_request_t *req, const upstream_rewrite_t *rw, buf_t *out);

#endif
