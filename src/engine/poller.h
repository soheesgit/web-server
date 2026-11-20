/* src/engine/poller.h
 *
 * fd 이벤트 대기 추상화. 빌드 시 poller_epoll.c / poller_kqueue.c / poller_poll.c 중 하나만
 * 링크된다. 디렉토리 책임과 관련 테스트: src/engine/README.md
 *
 * 이벤트 비트 값은 engine.h의 EV_READ(1)·EV_WRITE(2)·EV_ERROR(4)와 같다. 이 헤더는 engine.h를
 * include하지 않는다 — <sys/event.h>의 EV_ERROR 매크로와 충돌하기 때문이다 (D-021).
 */
#ifndef HTTPD_ENGINE_POLLER_H
#define HTTPD_ENGINE_POLLER_H

/**
 * poller (불투명). 정의는 poller_*.c.
 *
 * 생성:   poller_new
 * 소멸:   poller_free
 * 수명:   엔진 수명
 * 스레드: [per-object] — 루프 스레드 하나만 쓴다
 */
typedef struct poller poller_t;

/**
 * poller_wait가 돌려주는 이벤트 하나.
 *
 * 생성:   poller_wait
 * 소멸:   없음
 * 수명:   다음 poller_wait까지
 * 스레드: 값 타입
 */
typedef struct {
    int fd;          /* 이벤트가 난 fd */
    unsigned events; /* 1 읽기 가능(또는 EOF), 2 쓰기 가능, 4 오류·HUP */
    void *ptr;       /* poller_add·poller_mod에 넘긴 값 */
} poller_event_t;

/**
 * poller를 만든다 (epoll_create1(EPOLL_CLOEXEC) / kqueue + cloexec / 내부 pollfd 배열).
 *
 * @return  새 poller, NULL 실패
 *
 * 에러:    ENOMEM, EMFILE
 * 소유권:  poller를 할당 (poller_free로 해제)
 * 스레드:  [main-only]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-EN-05
 */
poller_t *poller_new(void);

/**
 * fd를 등록한다. 레벨 트리거.
 *
 * events는 1·2 비트의 조합(4는 항상 보고되므로 넣지 않아도 된다). 이미 등록된 fd면 -1 + EEXIST.
 *
 * @param p       poller. NULL 불가
 * @param fd      fd
 * @param events  감시 이벤트
 * @param ptr     이벤트와 함께 돌려줄 값. NULL 허용
 * @return        0 성공, -1 실패
 *
 * 에러:    EEXIST, ENOMEM, 그 밖의 epoll_ctl·kevent errno
 * 소유권:  fd는 호출자 소유
 * 스레드:  [per-object]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-EN-05 · 추후 U-EN-09
 */
int poller_add(poller_t *p, int fd, unsigned events, void *ptr);

/**
 * 등록된 fd의 감시 이벤트와 ptr을 바꾼다. 등록되지 않았으면 -1 + ENOENT.
 *
 * @param p       poller. NULL 불가
 * @param fd      fd
 * @param events  새 감시 이벤트 (0이면 감시 중지, 등록은 유지)
 * @param ptr     새 값
 * @return        0 성공, -1 실패
 *
 * 에러:    ENOENT, 그 밖의 errno
 * 소유권:  없음
 * 스레드:  [per-object]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  추후 U-EN-06
 */
int poller_mod(poller_t *p, int fd, unsigned events, void *ptr);

/**
 * fd 등록을 해제한다. 등록되지 않았으면 -1 + ENOENT. fd를 닫기 전에 부른다.
 *
 * @param p   poller. NULL 불가
 * @param fd  fd
 * @return    0 성공, -1 실패
 *
 * 에러:    ENOENT
 * 소유권:  없음
 * 스레드:  [per-object]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  추후 U-EN-06
 */
int poller_del(poller_t *p, int fd);

/**
 * 이벤트를 기다린다.
 *
 * 최대 max개 이벤트를 out에 채운다. fd 하나의 읽기·쓰기는 한 항목으로 합친다. timeout_ms가 -1이면
 * 무한, 0이면 즉시. 이벤트 없이 시간이 지나면 0. 시그널로 깨면(EINTR) 에러 없이 0.
 *
 * @param p           poller. NULL 불가
 * @param out         [out] 이벤트 배열. NULL 불가
 * @param max         out 크기 (1 이상)
 * @param timeout_ms  대기 시간 (ms)
 * @return            이벤트 수 (0 이상), -1 실패
 *
 * 에러:    EINVAL, 그 밖의 epoll_wait·kevent·poll errno (EINTR 제외)
 * 소유권:  없음
 * 스레드:  [per-object]
 * I/O:     없음
 * 블로킹:  timeout_ms까지
 * 테스트:  U-EN-05 · 추후 U-EN-07, U-EN-08
 */
int poller_wait(poller_t *p, poller_event_t *out, int max, int timeout_ms);

/**
 * poller를 해제한다. 멱등이며 NULL 허용. 등록된 fd는 닫지 않는다.
 *
 * @param p  poller. NULL 허용
 *
 * 에러:    없음 (errno를 바꾸지 않음)
 * 소유권:  p를 해제
 * 스레드:  [main-only]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-EN-05
 */
void poller_free(poller_t *p);

/**
 * 빌드된 poller 구현 이름.
 *
 * @return  "epoll" | "kqueue" | "poll" (정적 문자열)
 *
 * 에러:    없음
 * 소유권:  정적 메모리
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  (테스트 없음)
 */
const char *poller_backend_name(void);

#endif
