/* src/handler/handler.h
 *
 * 처리기 진입점: handler_dispatch와 STATIC·CGI·PROXY 처리기.
 * 디렉토리 책임과 관련 테스트: src/handler/README.md
 *
 * 처리기 공통 계약:
 *   - 입력 요청과 route는 빌린다. 결과는 out_plan(그리고 PROXY면 out_job)에만 쓴다.
 *   - 호출 전에 out_plan은 plan_init된 상태여야 한다. 처리기는 클라이언트 소켓을 모른다.
 *   - HANDLER_DONE이면 out_plan이 보낼 응답을 완전히 담고 있다(에러면 plan_error 결과).
 *   - 블로킹은 open·fstat·realpath·pipe·fork·execve만 허용한다. 소켓 I/O 금지.
 */
#ifndef HTTPD_HANDLER_HANDLER_H
#define HTTPD_HANDLER_HANDLER_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "common/types.h"
#include "core/config.h"
#include "core/str.h"
#include "router/router.h"

/**
 * 처리기가 참고하는 요청 밖의 정보.
 *
 * 생성:   conn이 요청마다 채움 (테스트는 직접 채움)
 * 소멸:   없음 (값 타입)
 * 수명:   처리기 호출 동안. cfg·lim·client_ip는 빌림
 * 스레드: [per-object]
 */
typedef struct {
    const config_t *cfg; /* 서버 설정 (CGI 타임아웃, 업스트림 값 등). NULL 불가 */
    const http_limits_t *lim; /* 요청 한도. NULL 불가 */
    str_t client_ip;          /* "127.0.0.1" 형식. CGI REMOTE_ADDR, X-Forwarded-For */
    uint16_t server_port;     /* 서버 listen 포트. CGI SERVER_PORT */
    time_t now;               /* 벽시계 (초). Last-Modified 비교·Date용 */
    int64_t now_ms;           /* 단조 시각 (ms). CGI 마감, 백엔드 제외 판단 */
} handler_ctx_t;

/**
 * 처리기 결과.
 *
 * 생성:   처리기
 * 소멸:   없음
 * 수명:   없음
 * 스레드: 값 타입
 */
typedef enum {
    HANDLER_DONE,         /* out_plan 완성 */
    HANDLER_NEED_UPSTREAM /* out_job 완성, conn이 relay를 실행해야 함 */
} handler_result_t;

/**
 * route 종류에 따라 처리기를 부른다. conn은 이 함수만 호출한다.
 *
 * ROUTE_STATIC → static_handle, ROUTE_CGI → cgi_handle, ROUTE_PROXY → proxy_begin.
 * 그 밖의 kind는 plan_error(500) 후 DONE. PROXY가 아니면 out_job은 건드리지 않는다.
 *
 * @param req       요청 (body 포함). NULL 불가
 * @param rt        router_match 결과. NULL 불가
 * @param ctx       문맥. NULL 불가
 * @param out_job   [out] PROXY일 때 채움. NULL 불가 (PROXY가 아니어도 넘김)
 * @param out_plan  [out] 계획. plan_init된 상태. NULL 불가
 * @return          DONE 또는 NEED_UPSTREAM
 *
 * 에러:    없음 (오류는 out_plan의 에러 계획)
 * 소유권:  req·rt·ctx를 빌림. out_plan·out_job에 자원을 넘김
 * 스레드:  [thread-safe] (객체가 다르면)
 * I/O:     처리기에 따름 (파일, 프로세스)
 * 블로킹:  처리기에 따름
 * 테스트:  E-ST-*, E-CG-*, E-PX-*
 */
handler_result_t handler_dispatch(const http_request_t *req, const route_t *rt,
                                  const handler_ctx_t *ctx, proxy_job_t *out_job,
                                  response_plan_t *out_plan);

/**
 * 정적 파일 응답 계획을 만든다.
 *
 * 1. 메서드가 GET·HEAD가 아니면 405 + "Allow: GET, HEAD".
 * 2. 파일 경로 = rt->root + req->path에서 rt->prefix를 뗀 나머지(앞에 '/' 보장). req->path는 이미
 *    정규화되어 루트 위로 가지 않는다(D-001).
 * 3. realpath(파일 경로)가 realpath(rt->root)로 시작하지 않으면(경계는 '/' 단위) 403
 *    — 심볼릭 링크로 루트 밖을 가리키는 경우. realpath가 ENOENT·ENOTDIR이면 404, EACCES면 403.
 * 4. open(O_RDONLY | O_CLOEXEC | O_NONBLOCK) 후 fstat. ENOENT·ENOTDIR 404, EACCES 403.
 *    (O_NONBLOCK은 FIFO에서 막히지 않기 위해서이며, 일반 파일 확인 후 끈다.)
 * 5. 디렉토리: 경로가 '/'로 끝나지 않으면 301 + "Location: <req->path>/"(쿼리가 있으면 "?<query>"
 *    덧붙임), 본문 없음. '/'로 끝나면 그 안의 index.html로 3~4를 다시 하고 없으면 403.
 * 6. 일반 파일이 아니면(FIFO, 장치, 소켓) 403.
 * 7. 성공: 200, BODY_FILE(fd, off 0, len st_size), framing LENGTH st_size,
 *    "Content-Type: <mime_for_path>", "Last-Modified: <IMF-fixdate(st_mtime)>".
 *    HEAD도 같은 계획을 돌려준다(본문 생략은 conn). 처리기는 파일 내용을 읽지 않는다.
 * 8. (선택, D-006) If-Modified-Since가 st_mtime 이상이면 304, BODY_NONE, fd를 닫는다.
 * 에러 응답은 plan_error로 만든다.
 *
 * @param req       요청. NULL 불가
 * @param rt        STATIC 규칙. NULL 불가
 * @param ctx       문맥. NULL 불가
 * @param out_plan  [out] 계획 (plan_init됨). NULL 불가
 * @return          항상 HANDLER_DONE
 *
 * 에러:    계획의 상태 코드: 301, 403, 404, 405, 304(선택), 500(그 밖의 open·fstat 실패)
 * 소유권:  연 fd를 out_plan에 넘긴다
 * 스레드:  [thread-safe]
 * I/O:     파일
 * 블로킹:  realpath, open, fstat
 * 테스트:  U-HD-09, U-HD-11, U-HD-14, E-ST-01~12 · 추후 U-HD-10, U-HD-12, U-HD-13, U-HD-15~20
 */
handler_result_t static_handle(const http_request_t *req, const route_t *rt,
                               const handler_ctx_t *ctx, response_plan_t *out_plan);

/**
 * CGI 스크립트를 시작하고 pipe 계획을 만든다 (D-013).
 *
 * 1. 메서드가 GET·HEAD·POST가 아니면 405 + "Allow: GET, HEAD, POST".
 * 2. 스크립트: req->path에서 rt->prefix를 뗀 나머지의 첫 세그먼트. 그 뒤 나머지(있으면 '/'로
 *    시작)는 PATH_INFO. 첫 세그먼트가 비면 404.
 * 3. 스크립트 경로 = rt->root + "/" + 세그먼트. realpath가 realpath(rt->root) 밖이면 403,
 *    없으면 404, 일반 파일이 아니면 403, 실행 권한(access X_OK)이 없으면 403.
 * 4. 환경 변수(새 envp만, 서버 환경은 물려주지 않음): GATEWAY_INTERFACE=CGI/1.1,
 *    SERVER_SOFTWARE=httpd/0.1, SERVER_PROTOCOL=HTTP/1.<minor>, SERVER_NAME(Host에서 포트 뗀 값),
 *    SERVER_PORT, REQUEST_METHOD, SCRIPT_NAME(prefix + 세그먼트), PATH_INFO, QUERY_STRING(원본,
 *    없으면 빈 값), REMOTE_ADDR, CONTENT_LENGTH(본문 있을 때), CONTENT_TYPE(헤더 있을 때),
 *    PATH=/usr/bin:/bin, 그리고 요청 헤더마다 HTTP_<이름 대문자, '-'→'_'>. 단 Proxy 헤더는
 *    HTTP_PROXY로 옮기지 않고(httpoxy), Content-Length·Content-Type은 HTTP_*로 중복하지 않는다.
 * 5. pipe 2개(cloexec) → fork → 자식: stdin·stdout을 dup2, 작업 디렉토리를 스크립트 디렉토리로,
 *    execve(스크립트, [스크립트], envp). 실패하면 _exit(127). stderr는 서버 것을 그대로 쓴다.
 * 6. 부모: BODY_PIPE, head_pending true, pid, in_fd(자식 stdin 쓰기 쪽), out_fd(자식 stdout 읽기
 *    쪽), in_data = 요청 본문 사본(없으면 길이 0), deadline_ms = ctx->now_ms + cfg->cgi_timeout_ms.
 *    pipe·fork 실패는 500.
 *
 * @param req       요청 (본문 수신 완료). NULL 불가
 * @param rt        CGI 규칙. NULL 불가
 * @param ctx       문맥. NULL 불가
 * @param out_plan  [out] 계획 (plan_init됨). NULL 불가
 * @return          항상 HANDLER_DONE
 *
 * 에러:    계획의 상태 코드: 403, 404, 405, 500
 * 소유권:  pipe fd 2개와 자식 pid를 out_plan에 넘긴다 (회수는 plan_cleanup)
 * 스레드:  [thread-safe]
 * I/O:     파일, 프로세스
 * 블로킹:  realpath, pipe, fork
 * 테스트:  U-HD-21, E-CG-01~10 · 추후 U-HD-22~25
 */
handler_result_t cgi_handle(const http_request_t *req, const route_t *rt, const handler_ctx_t *ctx,
                            response_plan_t *out_plan);

/**
 * CGI 출력 앞부분에서 CGI 응답 머리를 파싱한다.
 *
 * 줄 끝은 "\n"과 "\r\n" 모두 허용, 빈 줄에서 끝난다. 각 줄은 "name: value"(http_token_valid 이름,
 * ':' 필수). 규칙:
 *   - "Status: NNN [사유]": 3자리 100~599가 상태. 없으면 200, Status 없이 Location만 있으면 302.
 *   - Content-Length가 유효한 10진수면 framing LENGTH, 없으면 FRAMING_NONE으로 두고 conn이
 *     클라이언트 버전에 맞게 CHUNKED·CLOSE로 바꾼다(D-029). Status·Content-Length 외 헤더는
 *     out_head->fields에 "Name: value\r\n"으로 복사하되 Connection·Transfer-Encoding 등 hop-by-hop,
 *     Date·Server는 버린다.
 *   - 빈 줄이 없고 len이 max_head 미만이면 INCOMPLETE. max_head를 넘으면 ERROR 502.
 *   - 콜론 없는 줄, 잘못된 Status, 이름 형식 오류는 ERROR 502.
 * 성공하면 *out_used = 빈 줄 다음 바이트의 위치(본문 시작).
 * 예: "Content-Type: text/plain\n\nbody" → DONE, status 200, used 26.
 *
 * @param p           CGI 출력. len이 0이면 NULL 허용
 * @param len         바이트 수
 * @param max_head    머리 상한 (cfg->cgi_max_head)
 * @param out_head    [out] DONE이면 상태·fields·framing. out_head->fields는 buf_init된 상태로
 *                    넘기거나 0으로 채워 넘김(필요 시 buf_init). NULL 불가
 * @param out_used    [out] DONE이면 머리 길이. NULL 불가
 * @param out_status  [out] ERROR이면 502. NULL 불가
 * @return            DONE, INCOMPLETE, ERROR
 *
 * 에러:    *out_status 502
 * 소유권:  p를 빌려 out_head->fields에 복사
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-HD-26, E-CG-04, E-CG-05 · 추후 U-HD-27~30
 */
http_parse_result_t cgi_parse_head(const char *p, size_t len, size_t max_head,
                                   http_head_out_t *out_head, size_t *out_used, int *out_status);

/**
 * 프록시 요청을 준비한다.
 *
 * 1. rt->upstream 그룹에서 upstream_pick(g, NULL, ctx->now_ms). NULL이면 503 계획, DONE.
 * 2. out_job: group, backend, method = req->method, attempts 1,
 *    max_attempts = upstream_backend_count(g), sent_any false, client_minor = req->minor,
 *    client_keep_alive = req->keep_alive.
 * 3. request_write_upstream(req, &rw, &out_job->request). rw.client_ip = ctx->client_ip,
 *    rw.upstream_host = backend_hostport(backend), rw.strip_prefix = rt->strip_prefix면
 *    rt->prefix(끝 '/' 제외 — "/api/"를 떼어 "/api/users" → "/users"), 아니면 길이 0,
 *    rw.keep_host = rt->keep_host. 실패하면 500, DONE.
 * 4. HANDLER_NEED_UPSTREAM. out_plan은 건드리지 않는다.
 *
 * @param req       요청 (본문 포함). NULL 불가
 * @param rt        PROXY 규칙. NULL 불가
 * @param ctx       문맥. NULL 불가
 * @param out_job   [out] 프록시 작업. NULL 불가
 * @param out_plan  [out] 503·500일 때 에러 계획. NULL 불가
 * @return          NEED_UPSTREAM 또는 DONE(에러)
 *
 * 에러:    계획의 상태 코드: 503 (선택 가능한 백엔드 없음), 500
 * 소유권:  out_job->request 버퍼를 할당 (proxy_job_cleanup이 해제)
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-PX-01, E-PX-01, E-PX-02, E-PX-09 · 추후 U-PX-02~04
 */
handler_result_t proxy_begin(const http_request_t *req, const route_t *rt, const handler_ctx_t *ctx,
                             proxy_job_t *out_job, response_plan_t *out_plan);

/**
 * relay 실패 뒤 재시도할지 정한다 (D-004).
 *
 * 먼저 upstream_report(job->backend, false, 지금 단조 시각)를 부른다. 그다음:
 *   - PFAIL_CONNECT: attempts < max_attempts이고 upstream_pick(group, 실패한 백엔드, now)이
 *     NULL이 아니면 그 백엔드로 바꾸고 attempts += 1, sent_any = false, RETRY. 아니면 502, GIVE_UP.
 *   - PFAIL_SEND: sent_any가 false면 CONNECT와 같다. sent_any가 true면 method가 GET·HEAD일 때만
 *     CONNECT와 같은 규칙으로 RETRY, 그 밖(POST)은 502, GIVE_UP.
 *   - PFAIL_TIMEOUT: 504, GIVE_UP (GET도 재시도하지 않음).
 *   - PFAIL_BAD_RESPONSE: 502, GIVE_UP.
 * 재시도는 job->request를 바꾸지 않고 다시 보낸다 (D-010). GIVE_UP이면 out_plan에
 * plan_error(상태)를 만든다.
 *
 * @param job       작업. NULL 불가
 * @param why       실패 이유
 * @param out_plan  [out] GIVE_UP이면 에러 계획. NULL 불가
 * @return          RETRY 또는 GIVE_UP
 *
 * 에러:    계획의 상태 코드: 502, 504
 * 소유권:  없음
 * 스레드:  [per-object] (job 기준)
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-PX-05, U-PX-07, E-PX-08~11, E-PX-13 · 추후 U-PX-06, U-PX-08, U-PX-09, U-PX-15
 */
proxy_next_t proxy_on_fail(proxy_job_t *job, proxy_fail_t why, response_plan_t *out_plan);

/**
 * 업스트림 응답 머리로 클라이언트 응답 계획을 만든다.
 *
 * 1. head.status = resp->status (404·500도 그대로, 변환하지 않음).
 * 2. resp->headers 중 end-to-end 헤더만 순서대로 fields에 복사한다: http_is_hop_by_hop인 헤더와
 *    Date·Server·Content-Length는 건너뛴다(D-015).
 * 3. framing: job->method가 HEAD이거나 resp framing이 NONE이면 NONE. LENGTH는 그대로.
 *    CHUNKED는 클라이언트 1.1이면 CHUNKED(원본 청크를 그대로 중계), 1.0이면 CLOSE.
 *    CLOSE면 head.close = true. 클라이언트가 keep-alive가 아니면 head.close = true.
 * 4. kind BODY_UPSTREAM, body.upstream.conn = c, tracker = http_body_tracker_init(resp->framing)
 *    (업스트림 쪽 원래 방식으로 끝을 판정).
 * 5. upstream_report(c->backend, true, 지금 단조 시각).
 * 실패(메모리)하면 upstream_release(c, false) 후 502 계획을 만들고 -1.
 *
 * @param job       작업. NULL 불가
 * @param c         relay가 넘긴 연결 (rbuf에 응답 머리 + 본문 앞부분). NULL 불가
 * @param resp      파싱된 응답 머리 (c->rbuf를 빌림). NULL 불가
 * @param out_plan  [out] 계획 (plan_init됨). NULL 불가
 * @return          0 성공, -1 실패 (out_plan은 502)
 *
 * 에러:    ENOMEM
 * 소유권:  성공하면 c가 out_plan으로 이전
 * 스레드:  [per-object]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-PX-10, E-PX-03, E-PX-05, E-PX-06, E-PX-16 · 추후 U-PX-11~14
 */
int proxy_on_head(proxy_job_t *job, struct upstream_conn *c, const http_response_t *resp,
                  response_plan_t *out_plan);

/**
 * 프록시 작업을 정리한다. 멱등이며 NULL 허용.
 *
 * request 버퍼를 해제하고 backend·group 포인터를 NULL로 둔다. 업스트림 연결은 계획이 소유하므로
 * 건드리지 않는다.
 *
 * @param job  작업. NULL 허용
 *
 * 에러:    없음 (errno를 바꾸지 않음)
 * 소유권:  request 버퍼 해제
 * 스레드:  [per-object]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-PX-01
 */
void proxy_job_cleanup(proxy_job_t *job);

#endif
