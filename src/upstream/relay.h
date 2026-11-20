/* src/upstream/relay.h
 *
 * 프록시의 기다림이 있는 단계 실행: 연결 획득, 요청 송신, 응답 머리 수신.
 * 디렉토리 책임과 관련 테스트: src/upstream/README.md
 *
 * 재시도 여부는 판단하지 않고 실패 이유만 돌려준다 (판단은 handler의 proxy_on_fail).
 * 예외: 재사용 연결의 첫 송신 실패·응답 전 EOF는 같은 백엔드로 새 연결을 1회 다시 연다 (D-011).
 * 응답 본문 중계는 하지 않는다 (conn의 책임).
 */
#ifndef HTTPD_UPSTREAM_RELAY_H
#define HTTPD_UPSTREAM_RELAY_H

#include <stdint.h>

#include "common/types.h"
#include "upstream/upstream.h"

/**
 * relay 진행 결과.
 *
 * 생성:   relay_exchange_blocking, relay_step
 * 소멸:   없음
 * 수명:   없음
 * 스레드: 값 타입
 */
typedef enum {
    RELAY_OK,        /* 응답 머리 수신 완료 */
    RELAY_FAIL,      /* 실패 (이유는 out_fail / relay_take) */
    RELAY_WANT_READ, /* v2: relay_fd 읽기 대기 */
    RELAY_WANT_WRITE /* v2: relay_fd 쓰기 대기 */
} relay_status_t;

/**
 * v1: 시도 한 번을 끝까지 blocking으로 실행한다.
 *
 * 1. upstream_acquire(job->group, job->backend). 실패면 FAIL, PFAIL_CONNECT.
 * 2. connecting이면 poll(POLLOUT)로 connect_timeout_ms까지 기다리고 upstream_connect_result.
 *    시간 초과·오류는 FAIL, PFAIL_CONNECT.
 * 3. job->request를 모두 보낸다(poll + platform_send). 1바이트라도 보내면 job->sent_any = true.
 *    송신 실패는 FAIL, PFAIL_SEND (재사용 연결이면 D-011 규칙으로 새 연결 1회).
 * 4. 응답 머리를 rbuf에 읽으며 response_parse_head(job->method). 마감은 송신 완료 시각 +
 *    response_timeout_ms. 1xx는 head_len만큼 버리고 계속. 초과는 FAIL, PFAIL_TIMEOUT.
 *    ERROR·머리 전 EOF는 FAIL, PFAIL_BAD_RESPONSE (재사용 연결의 응답 0바이트 EOF는 D-011).
 * 5. 성공: OK, *out_conn = 연결(rbuf 앞 head_len은 머리, 나머지는 본문 앞부분),
 *    *out_resp의 str_t는 그 rbuf를 빌림.
 * 실패하면 연결은 이미 upstream_release(false)되어 있고 *out_conn은 NULL.
 * upstream_report는 부르지 않는다 (proxy_on_fail·proxy_on_head가 부름).
 *
 * @param job                  작업 (backend가 정해져 있음). NULL 불가
 * @param lim                  응답 머리 한도. NULL 불가
 * @param connect_timeout_ms   connect 마감 (ms)
 * @param response_timeout_ms  응답 머리 마감 (ms)
 * @param out_conn             [out] OK면 연결, FAIL이면 NULL. NULL 불가
 * @param out_resp             [out] OK면 응답 머리. NULL 불가
 * @param out_fail             [out] FAIL이면 이유. NULL 불가
 * @return                     RELAY_OK 또는 RELAY_FAIL
 *
 * 에러:    *out_fail (PFAIL_*)
 * 소유권:  OK면 연결 소유권이 호출자로 이전 (보통 proxy_on_head로 계획에 넘김)
 * 스레드:  [per-object] (job 기준)
 * I/O:     소켓
 * 블로킹:  최대 connect_timeout_ms + 송신 + response_timeout_ms
 * 테스트:  E-PX-01~17 · 추후 U-US-29~36
 */
relay_status_t relay_exchange_blocking(proxy_job_t *job, const http_limits_t *lim,
                                       int connect_timeout_ms, int response_timeout_ms,
                                       upstream_conn_t **out_conn, http_response_t *out_resp,
                                       proxy_fail_t *out_fail);

/**
 * v2 relay 상태 머신 (불투명). 정의는 relay.c.
 *
 * 생성:   relay_start
 * 소멸:   relay_free
 * 수명:   시도 한 번. job을 빌린다 (job이 더 오래 산다)
 * 스레드: [per-object]
 */
typedef struct relay relay_t;

/**
 * v2: 시도 한 번을 non-blocking으로 시작한다.
 *
 * relay_exchange_blocking의 1단계(acquire)를 하고 상태 머신을 만든다. 마감은 now_ms 기준으로
 * 계산한다. 첫 relay_step은 events 0으로 불러도 된다.
 *
 * @param job                  작업. NULL 불가
 * @param lim                  한도. NULL 불가
 * @param now_ms               현재 단조 시각
 * @param connect_timeout_ms   connect 마감 (ms)
 * @param response_timeout_ms  응답 머리 마감 (ms)
 * @return                     새 상태 머신 (acquire 실패도 만들고 첫 relay_step이 FAIL), ENOMEM이면
 * NULL
 *
 * 에러:    ENOMEM
 * 소유권:  상태 머신을 할당 (relay_free로 해제). job을 빌림
 * 스레드:  [per-object]
 * I/O:     소켓 (connect 시작)
 * 블로킹:  없음
 * 테스트:  추후 v2 단계 E-PX-* 재통과
 */
relay_t *relay_start(proxy_job_t *job, const http_limits_t *lim, int64_t now_ms,
                     int connect_timeout_ms, int response_timeout_ms);

/**
 * v2: 막히지 않는 만큼 진행한다.
 *
 * 단계와 실패 규칙은 relay_exchange_blocking과 같다. WANT_READ·WANT_WRITE면 호출자는 relay_fd를
 * 그 이벤트로 감시하다가 이벤트가 나면 다시 부른다. 마감(relay_deadline_ms)이 지났으면
 * 해당 단계의 실패(PFAIL_CONNECT 또는 PFAIL_TIMEOUT)로 FAIL. OK·FAIL 뒤에는 relay_take.
 *
 * @param r       상태 머신. NULL 불가
 * @param events  EV_READ·EV_WRITE·EV_ERROR 비트 (engine.h 값), 첫 호출은 0
 * @param now_ms  현재 단조 시각
 * @return        다음 상태
 *
 * 에러:    FAIL (이유는 relay_take)
 * 소유권:  없음
 * 스레드:  [per-object]
 * I/O:     소켓 (non-blocking)
 * 블로킹:  없음
 * 테스트:  추후 v2 단계 E-PX-* 재통과
 */
relay_status_t relay_step(relay_t *r, unsigned events, int64_t now_ms);

/**
 * v2: 지금 감시해야 할 fd.
 *
 * @param r  상태 머신. NULL 불가
 * @return   업스트림 소켓 fd, 없으면 -1
 *
 * 에러:    없음
 * 소유권:  fd는 상태 머신 소유
 * 스레드:  [per-object]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  추후 v2 단계
 */
int relay_fd(const relay_t *r);

/**
 * v2: 현재 단계의 마감 시각.
 *
 * @param r  상태 머신. NULL 불가
 * @return   단조 시각 (ms), 없으면 -1
 *
 * 에러:    없음
 * 소유권:  없음
 * 스레드:  [per-object]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  추후 v2 단계
 */
int64_t relay_deadline_ms(const relay_t *r);

/**
 * v2: OK·FAIL 뒤 결과를 넘겨받는다.
 *
 * OK면 *out_conn에 연결(소유권 이전), *out_resp에 머리. FAIL이면 *out_conn = NULL,
 * *out_fail에 이유(연결은 이미 release). 그 밖의 상태에서 부르면 *out_conn = NULL,
 * *out_fail = PFAIL_CONNECT. 한 번만 부른다.
 *
 * @param r         상태 머신. NULL 불가
 * @param out_conn  [out] 연결 또는 NULL. NULL 불가
 * @param out_resp  [out] 응답 머리. NULL 불가
 * @param out_fail  [out] 실패 이유. NULL 불가
 *
 * 에러:    없음
 * 소유권:  OK면 연결이 호출자로 이전
 * 스레드:  [per-object]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  추후 v2 단계
 */
void relay_take(relay_t *r, upstream_conn_t **out_conn, http_response_t *out_resp,
                proxy_fail_t *out_fail);

/**
 * v2: 상태 머신을 해제한다. 멱등이며 NULL 허용.
 *
 * 넘겨주지 않은 연결이 남아 있으면 upstream_release(false).
 *
 * @param r  상태 머신. NULL 허용
 *
 * 에러:    없음 (errno를 바꾸지 않음)
 * 소유권:  r과 넘겨주지 않은 연결을 해제
 * 스레드:  [per-object]
 * I/O:     소켓 close
 * 블로킹:  없음
 * 테스트:  추후 v2 단계
 */
void relay_free(relay_t *r);

#endif
