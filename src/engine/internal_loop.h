/* src/engine/internal_loop.h
 *
 * engine 내부 전용: v2 이벤트 루프(engine_loop.c)와 공통 진입점(engine_pool.c) 사이 함수.
 * 다른 디렉토리에서 include 금지. 디렉토리 책임과 관련 테스트: src/engine/README.md
 */
#ifndef HTTPD_ENGINE_INTERNAL_LOOP_H
#define HTTPD_ENGINE_INTERNAL_LOOP_H

#include <signal.h>

#include "engine/engine.h"

/**
 * v2 이벤트 루프 (불투명). 정의는 engine_loop.c.
 *
 * 생성:   loop_new
 * 소멸:   loop_free
 * 수명:   엔진 수명
 * 스레드: 루프 스레드 하나만 쓴다
 */
typedef struct loop loop_t;

/**
 * 이벤트 루프를 만든다 (engine_new(EVENT_LOOP)가 부른다).
 *
 * @param cb   콜백. NULL 불가 (on_accept·on_event·on_timeout·deadline_ms·on_close 필요)
 * @param ctx  콜백에 넘길 값. NULL 허용
 * @return     새 루프, NULL 실패
 *
 * 에러:    ENOMEM, poller_new의 errno
 * 소유권:  루프를 할당 (loop_free로 해제). cb·ctx는 빌림
 * 스레드:  [main-only]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  E-* 전체 (-e loop)
 */
loop_t *loop_new(const engine_callbacks_t *cb, void *ctx);

/**
 * 이벤트 루프를 돌린다. engine_run의 v2 본체이며 동작은 engine.h의 engine_run 명세를 따른다.
 *
 * @param l                  루프. NULL 불가
 * @param listen_fd          대기 소켓 (non-blocking). 닫지 않음
 * @param stop               정지 플래그. NULL 불가
 * @param shutdown_grace_ms  유예 시간 (ms)
 * @return                   0 정상 정지, -1 poller 오류
 *
 * 에러:    poller 오류의 errno
 * 소유권:  accept한 fd는 on_accept로 이전
 * 스레드:  [main-only]
 * I/O:     소켓
 * 블로킹:  *stop이 설 때까지 (poller 대기)
 * 테스트:  E-* 전체 (-e loop)
 */
int loop_run(loop_t *l, int listen_fd, volatile sig_atomic_t *stop, int shutdown_grace_ms);

/**
 * 남은 연결을 on_close하고 루프를 해제한다. 멱등이며 NULL 허용.
 *
 * @param l  루프. NULL 허용
 *
 * 에러:    없음 (errno를 바꾸지 않음)
 * 소유권:  l과 남은 연결을 해제
 * 스레드:  [main-only]
 * I/O:     없음 (on_close가 fd를 닫음)
 * 블로킹:  없음
 * 테스트:  E-* 전체 (-e loop)
 */
void loop_free(loop_t *l);

/**
 * 엔진의 이벤트 루프를 돌려준다.
 *
 * @param e  엔진. NULL 불가
 * @return   v2 엔진이면 루프, v1이면 NULL (engine_watch·engine_unwatch가 ENOTSUP으로 판정)
 *
 * 에러:    없음
 * 소유권:  엔진 소유 포인터
 * 스레드:  루프 스레드
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  E-* 전체 (-e loop)
 */
loop_t *engine_loop_of(engine_t *e);

#endif
