#ifndef HTTPD_UPSTREAM_INTERNAL_DEMO_TRACE_H
#define HTTPD_UPSTREAM_INTERNAL_DEMO_TRACE_H

#include "common/types.h"
#include "upstream/internal_state.h"

/** 현재 스레드에서 시연 요청의 라우팅 시도 기록을 시작한다. 작업은 시도 종료까지 빌린다. */
void demo_trace_begin(const proxy_job_t *job);

/** 기록 중이면 단조 시계의 마이크로초를, 그 외에는 0을 반환한다. */
uint64_t demo_trace_clock_us(void);

/** 선택 시간과 결과를 기록한다. 선택 중에는 고정 크기 배열만 사용한다. */
void demo_trace_choice(const char *policy, size_t limit, uint64_t started,
                       const backend_t *preferred);

/** 그룹 잠금 아래에서 후보의 활성 수와 순위를 복사한다. */
void demo_trace_candidate(const backend_t *backend, const char *reason, size_t rank);

/** 예약 결과와 선택 기록을 비블로킹 루프백 소켓으로 보내고 시도 기록을 닫는다. */
void demo_trace_attempt(const backend_t *selected, int error);

/** 진행 중인 업스트림 시도의 실패를 비블로킹 소켓으로 알린다. */
void demo_trace_failure(const proxy_job_t *job, proxy_fail_t reason);

/** 그룹 상태를 잠금 아래에서 복사한 뒤 잠금 밖에서 비블로킹으로 알린다. */
void demo_trace_state(upstream_group_t *group);

#endif
