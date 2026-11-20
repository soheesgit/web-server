/* src/downstream/conn.h
 *
 * 클라이언트 연결 하나의 상태 머신 (읽기 → 파싱 → 라우팅 → 처리 → 송신 → 연결 판정).
 * 디렉토리 책임과 관련 테스트: src/downstream/README.md
 *
 * ── 요청 하나의 처리 순서 (v1·v2 공통) ─────────────────────────────────────────
 *  1. 읽기: 수신 버퍼(상한 max_head + max_body 이상)에 buf_read_fd. 헤더 마감은 연결(또는 이전
 *     응답 완료) 시각 + header_timeout_ms의 절대 시각이다 (바이트가 조금씩 와도 연장 안 함).
 *  2. request_parse_head. INCOMPLETE면 1로. ERROR면 그 상태 코드로 plan_error, close = true.
 *  3. 본문: framing이 LENGTH면 head_len 뒤로 length바이트가 모일 때까지 읽는다(마감
 * body_timeout_ms, 본문 첫 대기 시작부터). 다 모이면 req.body가 그 구간을 빌린다. 본문이 다 오기
 * 전에는 처리기를 부르지 않는다.
 *  4. rt = router_match(router, req.host, req.path). NULL이면 plan_error(404).
 *  5. r = handler_dispatch(&req, rt, &ctx, &job, &plan). ctx의 now·now_ms는 이 시각.
 *  6. 프록시 루프 (r == HANDLER_NEED_UPSTREAM인 동안):
 *       s = relay_exchange_blocking(&job, lim, connect_timeout, response_timeout, &uc, &resp, &why)
 *           (v2는 relay_start / relay_step / relay_take, engine_watch로 relay_fd 감시)
 *       s == RELAY_OK:   proxy_on_head(&job, uc, &resp, &plan); 끝
 *       s == RELAY_FAIL: proxy_on_fail(&job, why, &plan) == PROXY_GIVE_UP이면 끝 (plan은
 * 502/503/504) RETRY면 job.backend가 바뀌어 있으니 다시 relay
 *  7. CGI(BODY_PIPE, head_pending): in_data를 in_fd에 쓰면서(다 쓰면 in_fd close) out_fd를 읽어
 *     cgi_parse_head가 DONE이 될 때까지 모은다. ERROR(502)·EOF 전 미완(502)·자식 비정상
 *     종료(502)·deadline_ms 초과(504, 자식 SIGKILL·waitpid)면 plan을 에러 계획으로 바꾼다.
 *     DONE이면 그 머리로 head를 확정하고 남은 바이트를 본문 앞부분으로 쓴다. CGI가
 *     Content-Length를 주지 않았으면 클라이언트 1.1은 CHUNKED, 1.0은 CLOSE (D-029).
 *  8. response_write_head(&plan.head, req.minor, now, wbuf). 요청이 HEAD면 본문을 보내지 않는다
 *     (머리의 Content-Length는 GET과 같게 둔다).
 *  9. 본문 송신: MEM은 data, FILE은 platform_sendfile(off, len), PIPE는 out_fd를 EOF까지 읽어
 *     framing대로(CHUNKED면 response_write_chunk·last_chunk), UPSTREAM은 uc->rbuf의 남은 본문 →
 *     uc->fd에서 읽기를 http_body_track이 DONE일 때까지 그대로 전달. 업스트림 본문이 ERROR이거나
 *     길이 미달 EOF면 클라이언트 연결을 닫는다(응답이 이미 나갔으므로 에러 응답 불가).
 *     끝나면 upstream_release(uc, resp.keep_alive && DONE)를 부르고 plan.body.upstream.conn = NULL.
 *     클라이언트가 중계 중 끊기면 upstream_release(uc, false).
 * 10. log_access 한 줄. proxy_job_cleanup(&job); plan_cleanup(&plan).
 * 11. 연결 판정: 닫음 조건 = 요청이 keep_alive 아님 || plan.head.close || 에러 응답(4xx 파서 오류,
 *     408) || 이 연결의 요청 수가 max_keepalive_requests에 도달 (그 응답에 Connection: close를
 *     넣는다) || 서버 종료 중. 유지하면 수신 버퍼에서 head_len + 본문 길이만큼 consume하고
 *     (파이프라이닝으로 남은 바이트는 다음 요청), 유휴 마감 keepalive_timeout_ms를 걸고 1로.
 *     남은 바이트가 있으면 유휴 대기 없이 바로 2부터.
 * 닫을 때 에러 응답 뒤에는 shutdown(SHUT_WR) 후 잠깐(최대 1초 또는 마감) 남은 입력을 버리며
 *     읽어 RST로 응답이 유실되지 않게 한다.
 * 타임아웃 상태 코드: 헤더 수신 중(바이트를 하나라도 받음) 408, 본문 수신 중 408, 유휴(요청 사이,
 *     받은 바이트 없음)는 응답 없이 close, 업스트림 응답 대기 504, CGI 실행 504.
 * 클라이언트가 처리 도중(CGI 실행, 중계, 송신) 끊으면 즉시 자원을 회수한다(자식 kill·waitpid 포함).
 */
#ifndef HTTPD_DOWNSTREAM_CONN_H
#define HTTPD_DOWNSTREAM_CONN_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>

#include "common/types.h"
#include "core/config.h"
#include "engine/engine.h"
#include "router/router.h"

/**
 * 클라이언트 연결 하나의 상태 (불투명). 정의는 conn.c.
 *
 * 생성:   conn_new
 * 소멸:   conn_free
 * 수명:   연결이 닫힐 때까지. 수신·송신 버퍼, 현재 요청의 계획·프록시 작업, fd를 소유
 * 스레드: [per-object] — 한 번에 한 워커(v1) 또는 루프 스레드(v2)만 다룬다
 */
typedef struct conn conn_t;

/**
 * conn_on_event·conn_on_timeout 후 engine이 할 일.
 *
 * 생성:   conn_on_event, conn_on_timeout
 * 소멸:   없음
 * 수명:   없음
 * 스레드: 값 타입
 */
typedef enum {
    CONN_WANT_READ,  /* 클라이언트 읽기 대기 */
    CONN_WANT_WRITE, /* 클라이언트 쓰기 대기 */
    CONN_WAIT_FD, /* pipe·업스트림 등 다른 fd 대기 (conn이 engine_watch로 이미 등록함) */
    CONN_CLOSE /* 종료. 호출자가 conn_free */
} conn_status_t;

/**
 * 모든 연결이 공유하는 실행 환경 (app이 만든다).
 *
 * 생성:   app_new
 * 소멸:   app_free
 * 수명:   서버 수명. conn은 포인터만 빌린다
 * 스레드: 읽기 전용 공유 가능 (router는 [thread-safe])
 */
typedef struct {
    const config_t *cfg;      /* 타임아웃·한도 값. NULL 불가 */
    const http_limits_t *lim; /* 요청 파싱 한도. NULL 불가 */
    const router_t *router;   /* 라우터. NULL 불가 */
    engine_t *engine; /* v2에서 추가 fd 등록용. v1은 NULL. v2인데 NULL이면 CGI·PROXY는
                         500 (D-025) */
    bool nonblocking; /* v1 false(타임아웃 있는 blocking), v2 true */
} conn_env_t;

/**
 * 연결 상태를 만든다.
 *
 * fd의 모드를 env->nonblocking에 맞춘다(D-012): true면 O_NONBLOCK을 켜고, false면 끄고
 * SO_RCVTIMEO·SO_SNDTIMEO를 짧은 주기(구현 재량, 1초 이하)로 설정해 마감을 확인할 수 있게 한다.
 * 헤더 마감 = now_ms + cfg->header_timeout_ms. 실패하면 fd를 닫지 않고 NULL (호출자가 닫음).
 *
 * @param fd      연결 소켓 (cloexec). 성공하면 conn이 소유
 * @param peer    상대 주소 (접근 로그·X-Forwarded-For용). NULL이면 "-"
 * @param env     실행 환경. NULL 불가. 서버 수명 동안 유효해야 함
 * @param now_ms  현재 단조 시각 (platform_now_ms)
 * @return        새 연결, NULL 실패
 *
 * 에러:    ENOMEM, fcntl·setsockopt의 errno
 * 소유권:  성공 시 fd가 conn으로 이전. env·peer는 빌림(peer는 복사)
 * 스레드:  [thread-safe]
 * I/O:     없음 (fd 설정만)
 * 블로킹:  없음
 * 테스트:  U-DS-40, U-DS-44 · 추후 U-DS-52
 */
conn_t *conn_new(int fd, const struct sockaddr_storage *peer, const conn_env_t *env,
                 int64_t now_ms);

/**
 * 연결을 정리한다.
 *
 * 멱등이며 NULL 허용. 현재 계획(plan_cleanup: 파일 fd, pipe fd, CGI 자식 kill·waitpid,
 * 업스트림 연결 release(false)), 프록시 작업, v2에서 등록한 추가 fd(engine_unwatch), 버퍼,
 * 클라이언트 fd를 모두 회수한다. 어느 단계에서 불러도 fd·자식 누수가 없어야 한다.
 *
 * @param c  연결. NULL 허용
 *
 * 에러:    없음 (errno를 바꾸지 않음)
 * 소유권:  c와 c가 소유한 모든 것을 해제
 * 스레드:  [per-object]
 * I/O:     close, kill, waitpid
 * 블로킹:  waitpid (SIGKILL 직후라 짧음)
 * 테스트:  E-SL-04, E-CG-09, E-PX-17 · 추후 U-DS-52
 */
void conn_free(conn_t *c);

/**
 * v1: 연결이 끝날 때까지 blocking으로 요청들을 처리한다.
 *
 * 파일 머리의 처리 순서를 그대로 실행한다. 대기는 SO_RCVTIMEO 주기마다 깨어나 마감
 * (platform_now_ms)을 확인하고, 마감이면 conn_on_timeout과 같은 규칙으로 응답·종료한다.
 * 반환하면 연결은 끝났고 호출자가 conn_free를 부른다(fd는 conn_free가 닫음).
 * env->nonblocking이 true인 연결에 부르면 아무것도 하지 않고 돌아온다.
 *
 * @param c  연결. NULL 불가
 *
 * 에러:    없음 (오류는 응답 또는 종료로 처리)
 * 소유권:  없음
 * 스레드:  [per-object]
 * I/O:     소켓, 파일, 프로세스
 * 블로킹:  연결이 끝날 때까지 (각 대기는 cfg의 마감으로 상한)
 * 테스트:  E-ST-*, E-CG-*, E-PX-*, E-KA-*, E-ER-*, E-SL-* (v1 엔진으로) · 추후 U-DS-54
 */
void conn_serve_blocking(conn_t *c);

/**
 * v2: fd 하나의 이벤트를 처리하고 다음에 기다릴 것을 돌려준다.
 *
 * fd는 클라이언트 소켓(conn_fd) 또는 이 연결이 engine_watch로 등록한 pipe·업스트림 fd다.
 * 막히지 않는 만큼만 진행한다(EAGAIN이면 멈춤). 파일 머리의 처리 순서를 상태 머신으로 실행하며,
 * 추가 fd를 기다려야 하면 engine_watch로 등록하고 CONN_WAIT_FD를 돌려준다(필요 없어진 fd는
 * engine_unwatch). 응답을 다 보냈고 연결을 유지하면 CONN_WANT_READ, 송신 버퍼가 남았으면
 * CONN_WANT_WRITE, 닫아야 하면 CONN_CLOSE.
 *
 * @param c       연결. NULL 불가
 * @param fd      이벤트가 난 fd
 * @param events  EV_READ·EV_WRITE·EV_ERROR 비트 (engine.h)
 * @param now_ms  현재 단조 시각
 * @return        다음 상태
 *
 * 에러:    없음 (오류는 응답 또는 CONN_CLOSE)
 * 소유권:  없음
 * 스레드:  [per-object]
 * I/O:     소켓, 파일, 프로세스 (non-blocking)
 * 블로킹:  없음
 * 테스트:  U-DS-40, U-DS-44 · 추후 U-DS-41~53
 */
conn_status_t conn_on_event(conn_t *c, int fd, unsigned events, int64_t now_ms);

/**
 * 마감 시각이 지났을 때 단계에 맞게 처리한다.
 *
 * 헤더 수신 중(바이트 1개 이상)·본문 수신 중이면 408 + Connection: close 응답을 보내도록 하고
 * (보낼 것이 남으면 CONN_WANT_WRITE, 다 보냈으면 CONN_CLOSE), 유휴(받은 바이트 없음)면 응답
 * 없이 CONN_CLOSE, 업스트림 응답 대기면 504, CGI 실행이면 자식 SIGKILL·waitpid 후 504.
 * 응답 머리를 이미 보낸 뒤의 마감(본문 중계·송신 정체)은 CONN_CLOSE.
 * 아직 마감 전이면(now_ms < conn_deadline_ms) 아무것도 하지 않고 현재 대기 상태를 돌려준다.
 *
 * @param c       연결. NULL 불가
 * @param now_ms  현재 단조 시각
 * @return        다음 상태
 *
 * 에러:    없음
 * 소유권:  없음
 * 스레드:  [per-object]
 * I/O:     소켓, 프로세스
 * 블로킹:  없음
 * 테스트:  E-ER-10, E-ER-11, E-KA-06, E-SL-01, E-CG-07, E-PX-10 · 추후 U-DS-47, U-DS-48
 */
conn_status_t conn_on_timeout(conn_t *c, int64_t now_ms);

/**
 * 클라이언트 소켓 fd를 돌려준다.
 *
 * @param c  연결. NULL 불가
 * @return   fd, 이미 닫혔으면 -1
 *
 * 에러:    없음
 * 소유권:  fd는 계속 conn 소유
 * 스레드:  [per-object]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-DS-40
 */
int conn_fd(const conn_t *c);

/**
 * 현재 단계의 마감 시각을 돌려준다 (헤더·본문·유휴·CGI·업스트림 중 지금 적용되는 것).
 *
 * @param c  연결. NULL 불가
 * @return   단조 시각 (ms), 마감이 없으면 -1
 *
 * 에러:    없음
 * 소유권:  없음
 * 스레드:  [per-object]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  추후 U-EN-10
 */
int64_t conn_deadline_ms(const conn_t *c);

#endif
