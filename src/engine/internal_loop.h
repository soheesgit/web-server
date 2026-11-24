#ifndef HTTPD_ENGINE_INTERNAL_LOOP_H
#define HTTPD_ENGINE_INTERNAL_LOOP_H

#include "engine/engine.h"

typedef struct engine_loop engine_loop_t;

/** poller와 v2 엔진 상태를 만든다. 콜백은 엔진 수명 동안 빌린다. */
engine_loop_t *engine_loop_new(const engine_callbacks_t *cb, void *ctx);

/** 클라이언트, 추가 fd 및 마감을 단일 루프에서 처리한다. */
int engine_loop_run(engine_loop_t *loop, int lfd, const volatile sig_atomic_t *stop, int grace_ms);

/** 루프와 소유한 연결을 정리한다. NULL 허용. */
void engine_loop_free(engine_loop_t *loop);

/** 현재 콜백이 소유한 fd를 등록하거나 관심 이벤트를 바꾼다. */
int engine_loop_watch(engine_loop_t *loop, int fd, unsigned events, void *obj);

/** 현재 콜백이 등록한 추가 fd를 감시에서 제거한다. */
int engine_loop_unwatch(engine_loop_t *loop, int fd);

#endif
