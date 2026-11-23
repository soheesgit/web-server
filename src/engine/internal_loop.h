/* src/engine/internal_loop.h
 *
 * engine 구현 파일끼리 공유하는 상태. 다른 디렉토리에서 include하지 않는다.
 * v1(engine_pool.c)과 v2(engine_loop.c)가 같은 struct engine을 쓰고, engine_new·engine_run·
 * engine_free는 kind로 나눠 부른다.
 */
#ifndef HTTPD_ENGINE_INTERNAL_LOOP_H
#define HTTPD_ENGINE_INTERNAL_LOOP_H

#include "engine/engine.h"
#include "engine/poller.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct work_item {
    int fd;
    void *obj;
    struct work_item *next;
} work_item_t;

/* v2: fd 번호로 찾는 등록 정보. client가 true면 live 배열의 slot에 있는 연결의 소켓이다. */
typedef struct {
    void *obj;      /* 이벤트를 받을 객체. NULL이면 등록 안 됨 */
    unsigned watch; /* 지금 poller에 걸린 이벤트 */
    bool client; /* 엔진이 accept한 클라이언트 fd인지 (engine_watch로 추가한 fd가 아님) */
    size_t slot; /* client일 때 live 배열 위치 */
} loop_fd_t;

struct engine {
    engine_kind_t kind;
    const engine_callbacks_t *cb;
    void *ctx;
    bool running;
    /* v1 스레드 풀 */
    int worker_count;
    size_t queue_cap;
    size_t queue_len;
    size_t queue_head;
    size_t queue_tail;
    work_item_t **queue;
    pthread_t *workers;
    int workers_started;
    bool worker_stop;
    pthread_mutex_t lock;
    pthread_cond_t ready;
    pthread_cond_t changed;
    work_item_t *active;
    /* v2 이벤트 루프 */
    poller_t *poller;
    loop_fd_t *fds; /* fd 번호로 바로 찾는 표 */
    size_t fds_cap;
    int *live; /* 살아 있는 연결의 클라이언트 fd (마감 검사·종료 정리용) */
    size_t live_len;
    size_t live_cap;
};

/**
 * v2 엔진을 만든다 (engine_new의 EVENT_LOOP 분기). on_accept·on_event·on_timeout·deadline_ms·
 * on_close가 모두 있어야 한다(없으면 NULL + EINVAL). poller_new 실패도 NULL.
 */
engine_t *loop_engine_new(const engine_callbacks_t *cb, void *ctx);

/**
 * v2 루프를 돈다 (engine_run의 EVENT_LOOP 분기). 정지 규칙은 engine_run과 같다: *stop이 서면
 * 새 연결을 받지 않고 유예 시간까지 기다린 뒤 남은 연결을 on_close한다.
 */
int loop_engine_run(engine_t *e, int listen_fd, volatile sig_atomic_t *stop, int shutdown_grace_ms);

/**
 * v2 엔진을 해제한다 (engine_free의 EVENT_LOOP 분기). 남은 연결은 on_close한다. e는 NULL 불가.
 */
void loop_engine_free(engine_t *e);

#endif
