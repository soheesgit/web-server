/* src/app/app.h
 *
 * 서버 조립: config → limits·upstream·router·listener·engine.
 * 디렉토리 책임과 관련 테스트: src/app/README.md
 */
#ifndef HTTPD_APP_APP_H
#define HTTPD_APP_APP_H

#include <signal.h>
#include <stddef.h>

#include "core/config.h"

/**
 * 조립된 서버 (불투명): cfg 사본, limits, registry, router, listen fd, conn 환경, engine.
 *
 * 생성:   app_new
 * 소멸:   app_free
 * 수명:   main 수명
 * 스레드: [main-only]
 */
typedef struct app app_t;

/**
 * 서버를 조립한다.
 *
 * 순서: cfg를 복사 → http_limits_t 변환 → upstream_registry_new → router_new(upstream_lookup_cb,
 * registry) → listener_open(listen_host, port, backlog) → conn_env_t 구성 → engine_new(cfg의
 * engine, workers, queue_len, conn 콜백). conn 콜백: on_accept = conn_new, serve_blocking =
 * conn_serve_blocking, on_event = conn_on_event(CONN_CLOSE면 -1, 그 밖에는 원하는 이벤트를
 * 다시 감시), on_timeout = conn_on_timeout, deadline_ms = conn_deadline_ms, on_close = conn_free,
 * on_busy = "HTTP/1.1 503 …, Connection: close" 응답을 보내고 close.
 * 실패하면 만든 것을 역순으로 정리하고 NULL, err에 원인(listener는 "bind: Address already in use"
 * 형식 그대로).
 *
 * @param cfg     설정. NULL 불가 (복사함)
 * @param err     [out] 실패 메시지. errcap이 0이면 NULL 허용
 * @param errcap  err 크기
 * @return        새 서버, NULL 실패
 *
 * 에러:    하위 함수의 errno
 * 소유권:  서버를 할당 (app_free로 해제)
 * 스레드:  [main-only]
 * I/O:     소켓, DNS
 * 블로킹:  getaddrinfo
 * 테스트:  E-CF-03, E-CF-04, E-* 전체
 */
app_t *app_new(const config_t *cfg, char *err, size_t errcap);

/**
 * 서버를 실행한다. *stop이 1이 되면 정리를 시작해 돌아온다.
 *
 * engine_run(engine, listen fd, stop, cfg.shutdown_grace_ms). 돌아오기 전 log_msg(LOG_INFO)로
 * 시작·종료를 남긴다.
 *
 * @param a     서버. NULL 불가
 * @param stop  정지 플래그. NULL 불가
 * @return      0 정상 종료, -1 엔진 오류
 *
 * 에러:    engine_run의 errno
 * 소유권:  없음
 * 스레드:  [main-only]
 * I/O:     소켓
 * 블로킹:  *stop이 설 때까지
 * 테스트:  E-* 전체 · 추후 N-SIG-01, N-SIG-02
 */
int app_run(app_t *a, volatile sig_atomic_t *stop);

/**
 * 서버를 해제한다. 멱등이며 NULL 허용.
 *
 * 역순: engine_free → listener_close → router_free → upstream_registry_free → 해제.
 *
 * @param a  서버. NULL 허용
 *
 * 에러:    없음 (errno를 바꾸지 않음)
 * 소유권:  a와 소유한 모든 것을 해제
 * 스레드:  [main-only]
 * I/O:     close
 * 블로킹:  pthread_join (engine_free)
 * 테스트:  추후 N-MEM-02
 */
void app_free(app_t *a);

#endif
