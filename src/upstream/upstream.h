/* src/upstream/upstream.h
 *
 * 백엔드 레지스트리·그룹·백엔드 상태, 라운드로빈 선택, 연결 획득·반납(풀).
 * 디렉토리 책임과 관련 테스트: src/upstream/README.md
 */
#ifndef HTTPD_UPSTREAM_UPSTREAM_H
#define HTTPD_UPSTREAM_UPSTREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/buf.h"
#include "core/config.h"
#include "core/str.h"

/**
 * 백엔드 하나 (불투명): 해석된 주소, 연속 실패 횟수, 제외 만료 시각, 유휴 연결 목록.
 *
 * 생성:   upstream_registry_new
 * 소멸:   upstream_registry_free
 * 수명:   레지스트리 수명 (서버 수명)
 * 스레드: 공개 함수로만 다루며 각 함수 표기를 따른다 (내부 상태는 mutex로 보호)
 */
typedef struct backend backend_t;

/**
 * upstream 그룹 하나 (불투명): 백엔드 목록 + 라운드로빈 카운터.
 *
 * 생성:   upstream_registry_new
 * 소멸:   upstream_registry_free
 * 수명:   레지스트리 수명
 * 스레드: 각 함수 표기를 따른다
 */
typedef struct upstream_group upstream_group_t;

/**
 * 설정의 모든 upstream 그룹을 담는 레지스트리 (불투명).
 *
 * 생성:   upstream_registry_new
 * 소멸:   upstream_registry_free
 * 수명:   서버 수명. 설정 값(타임아웃·임계값·풀 상한)을 복사해 둔다
 * 스레드: 생성·해제는 [main-only], 조회는 [thread-safe]
 */
typedef struct upstream_registry upstream_registry_t;

/**
 * 업스트림 연결 하나. 공개 구조체: conn과 relay가 fd와 rbuf를 직접 쓴다.
 *
 * 생성:   upstream_acquire
 * 소멸:   upstream_release
 * 수명:   acquire부터 release까지. 그동안 한 소유자(relay → 계획 → conn)만 쓴다
 * 스레드: [per-object]
 */
typedef struct upstream_conn {
    int fd;             /* 백엔드 소켓 (non-blocking, cloexec) */
    backend_t *backend; /* 이 연결의 백엔드. 빌림 */
    bool reused; /* 풀에서 꺼낸 연결. 첫 송신 실패 시 새 연결로 1회 재시도 허용 (D-011) */
    bool connecting; /* non-blocking connect 진행 중 (완료는 쓰기 가능 + upstream_connect_result) */
    buf_t rbuf; /* 응답 수신 버퍼. 헤더 파싱 후 남은 본문 앞부분을 담고 있음. 소유 */
} upstream_conn_t;

/**
 * 설정으로 레지스트리를 만든다.
 *
 * cfg->upstreams마다 그룹을, 백엔드마다 backend_t를 만든다. 주소는 getaddrinfo(AF_INET,
 * SOCK_STREAM)로 시작 시 1회 해석하고 첫 결과를 쓴다. 해석 실패면 만든 것을 모두 정리하고 NULL,
 * err에 "upstream <name>: cannot resolve <host>: <gai 메시지>". cfg의 upstream_connect_timeout_ms,
 * backend_fail_threshold, backend_cooldown_ms, pool_max_idle을 복사해 둔다. 그룹이 0개여도 성공.
 *
 * @param cfg     설정. NULL 불가 (복사하므로 호출 뒤 해제 가능)
 * @param err     [out] 실패 메시지. errcap이 0이면 NULL 허용
 * @param errcap  err 크기
 * @return        새 레지스트리, NULL 실패
 *
 * 에러:    EINVAL (해석 실패), ENOMEM
 * 소유권:  레지스트리를 할당 (upstream_registry_free로 해제)
 * 스레드:  [main-only]
 * I/O:     DNS (getaddrinfo)
 * 블로킹:  getaddrinfo
 * 테스트:  U-PX-01, U-US-17, U-US-24, E-PX-* · 추후 U-US-28
 */
upstream_registry_t *upstream_registry_new(const config_t *cfg, char *err, size_t errcap);

/**
 * 레지스트리를 해제한다. 멱등이며 NULL 허용.
 *
 * 모든 유휴 연결을 닫고 그룹·백엔드를 해제한다. 빌려 간 upstream_conn_t가 남아 있으면 안 된다
 * (호출자가 먼저 모두 release).
 *
 * @param reg  레지스트리. NULL 허용
 *
 * 에러:    없음 (errno를 바꾸지 않음)
 * 소유권:  reg와 소유한 모든 것을 해제
 * 스레드:  [main-only]
 * I/O:     소켓 close
 * 블로킹:  없음
 * 테스트:  U-US-17
 */
void upstream_registry_free(upstream_registry_t *reg);

/**
 * 이름으로 그룹을 찾는다 (대소문자 구분).
 *
 * @param reg   레지스트리. NULL 불가
 * @param name  그룹 이름, NUL 종료. NULL 불가
 * @return      그룹 (reg 수명), 없으면 NULL
 *
 * 에러:    없음
 * 소유권:  reg를 빌린 포인터
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-PX-01, U-US-17
 */
upstream_group_t *upstream_registry_get(upstream_registry_t *reg, const char *name);

/**
 * router_new에 넘길 이름 → 그룹 콜백. ctx는 upstream_registry_t *.
 *
 * upstream_registry_get(ctx, name)과 같다. ctx나 name이 NULL이면 NULL.
 *
 * @param ctx   레지스트리 (void *)
 * @param name  그룹 이름
 * @return      그룹, 없으면 NULL
 *
 * 에러:    없음
 * 소유권:  빌린 포인터
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  E-PX-* (app을 통해)
 */
struct upstream_group *upstream_lookup_cb(void *ctx, const char *name);

/**
 * 라운드로빈으로 다음 백엔드를 고른다.
 *
 * 그룹 카운터(0에서 시작)가 가리키는 백엔드부터 설정 순서로 돈다. 제외 중(제외 만료 시각 >
 * now_ms)인 백엔드와 exclude는 건너뛴다. 고른 백엔드 다음 위치로 카운터를 옮긴다. 모두 건너뛰면
 * NULL. 제외 시각이 지난 백엔드는 다시 후보가 된다(실패 횟수는 그대로 두고, 다음 성공 보고에서 0).
 * 예: A·B·C, exclude 없음, 9회 → A B C A B C A B C.
 *
 * @param g        그룹. NULL 불가
 * @param exclude  이번에 건너뛸 백엔드 (방금 실패한 것). NULL 허용
 * @param now_ms   현재 단조 시각
 * @return         백엔드, 없으면 NULL
 *
 * 에러:    없음
 * 소유권:  레지스트리를 빌린 포인터
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  짧은 mutex
 * 테스트:  U-US-17, E-PX-01, E-PX-08, E-PX-09 · 추후 U-US-18~21
 */
backend_t *upstream_pick(upstream_group_t *g, const backend_t *exclude, int64_t now_ms);

/**
 * 그룹의 백엔드 수를 돌려준다.
 *
 * @param g  그룹. NULL 불가
 * @return   백엔드 수 (1 이상)
 *
 * 에러:    없음
 * 소유권:  없음
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-PX-05 (proxy_on_fail을 통해)
 */
size_t upstream_backend_count(const upstream_group_t *g);

/**
 * 백엔드 b로 가는 연결을 얻는다.
 *
 * 1. b의 유휴 연결을 최근 반납한 것부터 꺼낸다. 살아 있는지 확인한다(recv MSG_PEEK |
 *    MSG_DONTWAIT가 EAGAIN이면 살아 있음, 0(EOF)·에러·예상 밖 데이터면 닫고 다음 것).
 *    살아 있으면 reused = true, connecting = false로 돌려준다 (rbuf는 비움).
 * 2. 없으면 새 소켓(non-blocking, cloexec, TCP_NODELAY)으로 connect를 시작한다. 즉시 성공하면
 *    connecting = false, EINPROGRESS면 connecting = true. reused = false. 완료를 기다리지 않는다.
 * rbuf는 buf_init(…, 16 KiB, 1 MiB 이상)된 상태로 돌려준다.
 *
 * @param g      b가 속한 그룹. NULL 불가
 * @param b      대상 백엔드. NULL 불가
 * @param out_c  [out] 성공 시 연결. 실패 시 바꾸지 않음. NULL 불가
 * @return       0 성공, -1 실패 (socket·connect 즉시 실패 — 호출자는 PFAIL_CONNECT로 처리)
 *
 * 에러:    ECONNREFUSED 등 connect 즉시 실패, EMFILE, ENOMEM
 * 소유권:  연결을 할당해 돌려준다 (upstream_release로 반납·해제)
 * 스레드:  [thread-safe]
 * I/O:     소켓
 * 블로킹:  없음 (non-blocking connect 시작까지)
 * 테스트:  U-US-24, E-PX-14 · 추후 U-US-22, U-US-23, U-US-26
 */
int upstream_acquire(upstream_group_t *g, backend_t *b, upstream_conn_t **out_c);

/**
 * non-blocking connect의 결과를 확인한다.
 *
 * getsockopt(SO_ERROR). 0이면 connecting = false로 바꾸고 0. 오류면 errno에 그 값을 두고 -1.
 * connecting이 false인 연결에 부르면 0.
 *
 * @param c  연결. NULL 불가
 * @return   0 연결됨, -1 실패
 *
 * 에러:    ECONNREFUSED, ETIMEDOUT, EHOSTUNREACH 등
 * 소유권:  없음
 * 스레드:  [per-object]
 * I/O:     없음 (getsockopt)
 * 블로킹:  없음
 * 테스트:  U-US-24 · 추후 U-US-22, U-US-23
 */
int upstream_connect_result(upstream_conn_t *c);

/**
 * 연결을 반납하거나 닫는다.
 *
 * reusable이 true이고 connecting이 false이며 백엔드의 유휴 목록이 pool_max_idle 미만이면 rbuf를
 * 비워 유휴 목록에 넣는다(fd는 열린 채). 아니면 fd를 닫고 rbuf와 c를 해제한다. c가 NULL이면
 * 아무것도 하지 않는다. 호출 뒤 c를 쓰면 안 된다.
 *
 * @param c         연결. NULL 허용
 * @param reusable  응답을 끝까지 읽었고 keep-alive인가
 *
 * 에러:    없음
 * 소유권:  c의 소유권을 가져간다
 * 스레드:  [thread-safe]
 * I/O:     소켓 close
 * 블로킹:  짧은 mutex
 * 테스트:  U-US-24, E-PX-14, E-PX-17 · 추후 U-US-25, U-US-27
 */
void upstream_release(upstream_conn_t *c, bool reusable);

/**
 * 백엔드 시도 결과를 보고한다.
 *
 * ok면 연속 실패 횟수를 0으로. 실패면 횟수 += 1, 횟수 ≥ backend_fail_threshold이면 제외 만료
 * 시각 = now_ms + backend_cooldown_ms (이미 제외 중이면 연장). 제외되는 순간 log_msg(LOG_WARN).
 *
 * @param b       백엔드. NULL이면 아무것도 하지 않음
 * @param ok      성공 여부
 * @param now_ms  현재 단조 시각
 *
 * 에러:    없음
 * 소유권:  없음
 * 스레드:  [thread-safe]
 * I/O:     없음 (로그 제외)
 * 블로킹:  짧은 mutex
 * 테스트:  E-PX-08, E-PX-09 · 추후 U-US-19, U-PX-15
 */
void upstream_report(backend_t *b, bool ok, int64_t now_ms);

/**
 * 포워드 프록시 목적지의 백엔드를 돌려준다 (D-041).
 *
 * authority는 "host[:port]"(포트가 없으면 80). 같은 "host:port"를 전에 만들었으면 그 백엔드를
 * 돌려줘 keep-alive 풀을 함께 쓴다. 처음 보는 목적지면 이름을 해석해 그룹에 더한다. 그룹의
 * 목적지는 최대 256개. IPv6 리터럴("[::1]")은 받지 않는다.
 *
 * @param g          upstream_lookup_cb(reg, CFG_FORWARD_UPSTREAM)이 돌려준 포워드 그룹. NULL 불가
 * @param authority  absolute-form URI의 authority (http_request_t.host)
 * @return           백엔드 (레지스트리 수명), NULL 실패
 *
 * 에러:    EINVAL (형식·이름 해석 실패), E2BIG (목적지 수 상한), ENOMEM
 * 소유권:  레지스트리가 소유 (해제 금지)
 * 스레드:  [thread-safe]
 * I/O:     이름 해석 (getaddrinfo)
 * 블로킹:  처음 보는 목적지의 이름 해석
 * 테스트:  E-FW-01
 */
backend_t *upstream_forward_backend(upstream_group_t *g, str_t authority);

/**
 * 백엔드의 "host:port" 문자열을 돌려준다 (Host 헤더·로그용).
 *
 * 설정에 적힌 host 그대로(해석 전 이름) + ":" + port.
 *
 * @param b  백엔드. NULL 불가
 * @return   레지스트리 수명의 NUL 종료 문자열 (해제 금지)
 *
 * 에러:    없음
 * 소유권:  레지스트리 메모리
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-US-17, U-US-01
 */
const char *backend_hostport(const backend_t *b);

#endif
