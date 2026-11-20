/* src/engine/engine.h
 *
 * 동시성 엔진 공통 API: v1 스레드 풀(engine_pool.c), v2 이벤트 루프(engine_loop.c).
 * 디렉토리 책임과 관련 테스트: src/engine/README.md
 *
 * engine은 HTTP를 모른다. 연결 객체는 void *이고, 무엇을 할지는 콜백이 정한다.
 */
#ifndef HTTPD_ENGINE_ENGINE_H
#define HTTPD_ENGINE_ENGINE_H

#include <signal.h>
#include <stdint.h>
#include <sys/socket.h>

/**
 * 엔진 종류.
 *
 * 생성:   app (cfg_engine_t 변환)
 * 소멸:   없음
 * 수명:   없음
 * 스레드: 값 타입
 */
typedef enum {
    ENGINE_THREAD_POOL, /* v1: 메인 스레드 accept + 워커가 serve_blocking */
    ENGINE_EVENT_LOOP   /* v2: 단일 루프 스레드 + poller */
} engine_kind_t;

/* fd 이벤트 비트. poller.h도 같은 값을 쓴다 (D-021). */
enum { EV_READ = 1u, EV_WRITE = 2u, EV_ERROR = 4u };

/**
 * 엔진 (불투명). 정의는 engine_pool.c / engine_loop.c.
 *
 * 생성:   engine_new
 * 소멸:   engine_free
 * 수명:   서버 수명
 * 스레드: engine_new·engine_run·engine_free는 [main-only]. engine_watch·engine_unwatch는
 *         콜백 안(루프 스레드)에서 부른다
 */
typedef struct engine engine_t;

/**
 * 엔진이 연결 객체를 다루는 콜백 묶음.
 *
 * 생성:   app이 정적 상수로 둠
 * 소멸:   없음
 * 수명:   engine_new에 넘긴 포인터가 엔진 수명 동안 유효해야 함
 * 스레드: v1은 serve_blocking이 워커 스레드에서 동시에 불린다(객체는 서로 다름).
 *         on_accept·on_busy는 accept 스레드, on_close는 객체를 처리한 스레드.
 *         v2는 모두 루프 스레드에서 불리며 블로킹하면 안 된다
 */
typedef struct {
    /* 새 연결. fd 소유권을 넘겨받아 객체를 돌려준다. NULL이면 엔진이 fd를 닫는다. */
    void *(*on_accept)(void *ctx, int fd, const struct sockaddr_storage *peer, int64_t now_ms);
    /* v1: 워커가 연결 하나를 끝까지 처리 (반환 후 엔진이 on_close). */
    void (*serve_blocking)(void *ctx, void *obj);
    /* v2: fd 이벤트. 0 계속(엔진은 객체가 원하는 이벤트를 다시 감시), -1 닫기(on_close). */
    int (*on_event)(void *ctx, void *obj, int fd, unsigned events, int64_t now_ms);
    /* v2: 마감 경과. 0 계속, -1 닫기. */
    int (*on_timeout)(void *ctx, void *obj, int64_t now_ms);
    /* v2: 객체의 다음 마감 (단조 ms). -1 없음. */
    int64_t (*deadline_ms)(void *ctx, void *obj);
    /* 객체 해제 (객체가 소유한 fd 포함). 객체마다 정확히 한 번. */
    void (*on_close)(void *ctx, void *obj);
    /* v1 작업 큐가 가득: 503 응답 후 fd close (D-005). NULL이면 엔진이 그냥 닫는다. */
    void (*on_busy)(void *ctx, int fd);
} engine_callbacks_t;

/**
 * 엔진을 만든다.
 *
 * THREAD_POOL: workers개 워커 스레드와 길이 queue_len의 작업 큐를 준비한다(스레드는 engine_run
 * 에서 시작해도 되고 여기서 시작해도 된다). EVENT_LOOP: poller_new. workers·queue_len은 무시.
 * cb와 ctx는 빌려 둔다. THREAD_POOL이면 cb->on_accept·serve_blocking·on_close가 필요하고
 * EVENT_LOOP이면 on_accept·on_event·on_timeout·deadline_ms·on_close가 필요하다(없으면 NULL +
 * EINVAL).
 *
 * @param kind       엔진 종류
 * @param workers    v1 워커 수 (1 이상)
 * @param queue_len  v1 작업 큐 길이 (1 이상)
 * @param cb         콜백. NULL 불가
 * @param ctx        콜백에 넘길 값. NULL 허용
 * @return           새 엔진, NULL 실패
 *
 * 에러:    EINVAL, ENOMEM, pthread_create·poller_new 실패
 * 소유권:  엔진을 할당 (engine_free로 해제). cb·ctx는 빌림
 * 스레드:  [main-only]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-EN-01, U-EN-02 · 추후 U-EN-03
 */
engine_t *engine_new(engine_kind_t kind, int workers, int queue_len, const engine_callbacks_t *cb,
                     void *ctx);

/**
 * 대기 소켓에서 연결을 받아 처리한다. *stop이 1이 될 때까지 돌아온다.
 *
 * listen_fd는 non-blocking 대기 소켓이다(listener_open). 연결마다 listener_accept와 같은 규칙으로
 * accept하고(엔진은 downstream을 include할 수 없으므로 accept를 직접 부른다 — cloexec 보장,
 * EINTR 재시도, EMFILE이면 잠시 쉼) cb->on_accept로 객체를 만든다.
 *   v1: 객체를 작업 큐에 넣고 워커가 serve_blocking → on_close. 큐가 가득이면 on_busy(fd)
 *       (객체는 만들지 않는다 — on_accept 전에 큐 자리를 확인한다).
 *   v2: 클라이언트 fd를 EV_READ로 등록하고 이벤트마다 on_event, 마감마다 on_timeout.
 *       on_event·on_timeout이 -1이면 engine_unwatch 후 on_close.
 * 정지: *stop을 짧은 주기(100ms 이하)로 확인한다. 1이 되면 새 연결을 받지 않고(대기 소켓을 더
 * 감시하지 않음), 진행 중 연결을 shutdown_grace_ms까지 기다린다. 유예가 지나면 남은 연결의
 * fd를 shutdown(SHUT_RDWR)해 워커를 깨우고 on_close한다. 모든 워커를 join한 뒤 0을 돌려준다.
 *
 * @param e                  엔진. NULL 불가
 * @param listen_fd          대기 소켓 (non-blocking). 엔진은 닫지 않음
 * @param stop               정지 플래그 (시그널 핸들러가 1로 씀). NULL 불가
 * @param shutdown_grace_ms  유예 시간 (ms)
 * @return                   0 정상 정지, -1 실패 (poller·스레드 오류)
 *
 * 에러:    poller·pthread 오류의 errno
 * 소유권:  accept한 fd는 on_accept로 이전 (NULL이면 엔진이 닫음)
 * 스레드:  [main-only]
 * I/O:     소켓
 * 블로킹:  *stop이 설 때까지
 * 테스트:  U-EN-01, U-EN-02, E-* 전체 · 추후 U-EN-03, U-EN-04, U-EN-11, N-SIG-01, N-SIG-02
 */
int engine_run(engine_t *e, int listen_fd, volatile sig_atomic_t *stop, int shutdown_grace_ms);

/**
 * v2: 객체의 추가 fd(pipe·업스트림)를 등록하거나 감시 이벤트를 바꾼다.
 *
 * 처음 보는 fd면 추가, 이미 등록된 fd면 이벤트·객체를 바꾼다. 그 fd의 이벤트는 obj의
 * on_event로 전달된다. v1 엔진이면 -1 + ENOTSUP.
 *
 * @param e       엔진. NULL 불가
 * @param fd      감시할 fd (non-blocking)
 * @param events  EV_READ·EV_WRITE 비트
 * @param obj     이벤트를 받을 객체. NULL 불가
 * @return        0 성공, -1 실패
 *
 * 에러:    ENOTSUP (v1), poller 오류의 errno
 * 소유권:  fd는 호출자(객체) 소유 유지. 닫기 전에 engine_unwatch해야 함
 * 스레드:  루프 스레드 (콜백 안)
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  추후 v2 단계 (U-EN-05, U-EN-06은 poller로 검증)
 */
int engine_watch(engine_t *e, int fd, unsigned events, void *obj);

/**
 * v2: 추가 fd 등록을 해제한다.
 *
 * 등록되지 않은 fd면 -1 + ENOENT. v1이면 -1 + ENOTSUP.
 *
 * @param e   엔진. NULL 불가
 * @param fd  해제할 fd
 * @return    0 성공, -1 실패
 *
 * 에러:    ENOENT, ENOTSUP
 * 소유권:  없음
 * 스레드:  루프 스레드 (콜백 안)
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  추후 v2 단계
 */
int engine_unwatch(engine_t *e, int fd);

/**
 * 엔진을 해제한다. 멱등이며 NULL 허용.
 *
 * engine_run이 돌아온 뒤에 부른다. 남은 워커를 join하고 큐에 남은 객체가 있으면 on_close한다.
 *
 * @param e  엔진. NULL 허용
 *
 * 에러:    없음 (errno를 바꾸지 않음)
 * 소유권:  e를 해제
 * 스레드:  [main-only]
 * I/O:     없음
 * 블로킹:  pthread_join
 * 테스트:  U-EN-01
 */
void engine_free(engine_t *e);

#endif
