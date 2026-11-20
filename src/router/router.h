/* src/router/router.h
 *
 * 경로·Host 규칙으로 요청에 맞는 route_t를 고른다.
 * 디렉토리 책임과 관련 테스트: src/router/README.md
 */
#ifndef HTTPD_ROUTER_ROUTER_H
#define HTTPD_ROUTER_ROUTER_H

#include <stdbool.h>
#include <stddef.h>

#include "common/types.h"
#include "core/config.h"
#include "core/str.h"

/**
 * 라우팅 규칙 하나 (설정의 route_conf_t를 실행용으로 바꾼 것).
 *
 * 생성:   router_new
 * 소멸:   router_free
 * 수명:   router 수명. 문자열 필드는 router_new에 넘긴 config_t를 빌린다(config_t가 더 오래 산다)
 * 스레드: 읽기 전용 공유 가능
 */
typedef struct {
    route_kind_t kind;  /* 처리 방식 */
    const char *prefix; /* 경로 접두사, NUL 종료. 설정값을 빌림 (config_t 수명) */
    size_t prefix_len;  /* strlen(prefix) */
    const char *host;   /* Host 조건 (포트 없음). NULL이면 모든 Host */
    const char *root;   /* STATIC·CGI 디스크 디렉토리. PROXY면 NULL */
    bool strip_prefix;  /* PROXY: 전달 전에 prefix를 뗀다 */
    bool keep_host;     /* PROXY: 원래 Host 유지 */
    struct upstream_group *upstream; /* PROXY 대상 그룹 (lookup 콜백 결과). 그 밖에는 NULL */
} route_t;

/**
 * 라우터 (불투명). 정의는 router.c.
 *
 * 생성:   router_new
 * 소멸:   router_free
 * 수명:   서버 수명. config_t를 빌린다
 * 스레드: 생성 후 읽기 전용, router_match는 [thread-safe]
 */
typedef struct router router_t;

/**
 * upstream 이름을 그룹 포인터로 바꾸는 콜백 (router는 upstream을 include할 수 없으므로).
 *
 * @param ctx   router_new에 넘긴 lookup_ctx
 * @param name  upstream 이름, NUL 종료
 * @return      그룹, 없으면 NULL
 */
typedef struct upstream_group *(*router_upstream_lookup_fn)(void *ctx, const char *name);

/**
 * 설정으로 라우터를 만든다.
 *
 * cfg->routes를 route_t로 바꾼다. PROXY 규칙은 lookup(lookup_ctx, upstream 이름)으로 그룹을
 * 얻고, NULL이면 실패("route <prefix>: unknown upstream '<name>'"). lookup이 NULL이면 PROXY 규칙이
 * 있을 때 실패. cfg->n_routes가 0이면 {kind STATIC, prefix "/", host NULL, root cfg->root}
 * 규칙 하나를 만든다. 매칭 순서는 router_match 주석의 규칙이 정하므로 설정 순서와 무관하다.
 *
 * @param cfg         설정. NULL 불가. 라우터보다 오래 살아야 함
 * @param lookup      upstream 이름 해석 콜백. PROXY 규칙이 없으면 NULL 허용
 * @param lookup_ctx  콜백에 넘길 값. NULL 허용
 * @param err         [out] 실패 메시지. errcap이 0이면 NULL 허용
 * @param errcap      err 크기
 * @return            새 라우터, NULL 실패
 *
 * 에러:    EINVAL (없는 upstream), ENOMEM
 * 소유권:  라우터를 할당해 돌려준다(router_free로 해제). cfg 문자열을 빌린다
 * 스레드:  [main-only]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-RT-02, U-RT-03, E-CF-02 · 추후 U-RT-01, U-RT-07
 */
router_t *router_new(const config_t *cfg, router_upstream_lookup_fn lookup, void *lookup_ctx,
                     char *err, size_t errcap);

/**
 * 라우터를 해제한다. 멱등이며 NULL 허용.
 *
 * @param r  라우터. NULL 허용
 *
 * 에러:    없음 (errno를 바꾸지 않음)
 * 소유권:  r을 해제. 빌린 config_t와 upstream 그룹은 해제하지 않음
 * 스레드:  [main-only]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-RT-02
 */
void router_free(router_t *r);

/**
 * 요청에 맞는 규칙을 고른다.
 *
 * 1. host에서 ':' 뒤 포트를 뗀다 (IPv6 리터럴 "[::1]:8080"이면 ']' 뒤의 포트만). host 비교는
 *    ASCII 대소문자 무시.
 * 2. prefix 일치: path가 prefix로 시작하고, prefix가 '/'로 끝나거나 path가 prefix와 같거나 path의
 *    prefix 다음 문자가 '/'이다. 예: "/api"는 "/api"·"/api/x"에 맞고 "/apix"에 맞지 않는다.
 *    "/api/"는 "/api/x"에 맞고 "/api"에는 맞지 않는다.
 * 3. host가 지정된 규칙 중 host가 같은 것들을 먼저 보고, 그중 prefix가 가장 긴 규칙이 이긴다.
 *    없으면 host 미지정 규칙 중 가장 긴 prefix. 길이가 같으면 설정에서 먼저 나온 규칙.
 * 4. 아무것도 맞지 않으면 NULL (conn이 404).
 *
 * @param r     라우터. NULL 불가
 * @param host  요청 Host (포트 포함 가능). 길이 0 허용 (host 미지정 규칙만 대상)
 * @param path  정규화된 경로, NUL 종료. NULL 불가
 * @return      규칙 (r 안을 가리킴, r 수명), NULL 불일치
 *
 * 에러:    없음
 * 소유권:  r을 빌린 포인터를 돌려준다
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-RT-02, U-RT-03, E-PX-15, E-ST-01 · 추후 U-RT-01, U-RT-04~08
 */
const route_t *router_match(const router_t *r, str_t host, const char *path);

#endif
