/* src/core/config.h
 *
 * 기본값 → 설정 파일(-c) → CLI 순으로 읽어 원시 타입만 담은 config_t를 만든다.
 * 디렉토리 책임과 관련 테스트: src/core/README.md
 *
 * core는 다른 영역을 include할 수 없으므로 config_t는 원시 타입만 담는다. 각 영역 타입
 * (http_limits_t, engine_kind_t 등)으로의 변환은 app이 한다.
 *
 * ── CLI ─────────────────────────────────────────────────────────────────────
 *   -p <port>          listen 포트 (1~65535)
 *   -r <root>          기본 STATIC 루트 (존재하는 디렉토리)
 *   -c <file>          설정 파일
 *   -w <workers>       워커 수 (1~1024)
 *   -e pool|loop       엔진 종류
 *   -h, --help         사용법 출력
 *   위치 인자는 허용하지 않는다. 같은 옵션이 반복되면 마지막 값.
 *
 * ── 설정 파일 문법 (D-009) ──────────────────────────────────────────────────
 *   file       = *line
 *   line       = *WS [ directive *WS ] [ comment ] LF       ; 줄 끝 CR은 무시
 *   comment    = "#" *<LF가 아닌 바이트>
 *   WS         = SP / HTAB          ; 토큰 구분자는 1개 이상의 WS
 *   directive  = "listen" WS listen-addr
 *              / "root" WS path
 *              / "engine" WS ( "pool" / "loop" )
 *              / "workers" WS uint
 *              / "upstream" WS name 1*( WS hostport )
 *              / route
 *              / "host" WS hostname WS route
 *              / knob WS uint
 *              / ( "access_log" / "error_log" ) WS path
 *   route      = "route" WS prefix WS ( ( "static" / "cgi" ) WS path
 *                                     / "proxy" WS name [ WS "strip" ] [ WS "keep_host" ]
 *                                     / "forward" )   ; 포워드 프록시 (D-041)
 *   listen-addr= port / host ":" port
 *   hostport   = host ":" port          ; host는 IPv4 주소 또는 이름 (시작 시 1회 해석)
 *   port       = 1~65535 10진수
 *   prefix     = "/" *<WS가 아닌 바이트>
 *   name       = 1*( ALPHA / DIGIT / "_" / "-" ), CFG_NAME_MAX-1자 이하
 *   path       = 1*<WS가 아닌 바이트>, CFG_PATH_MAX-1자 이하. 상대 경로는 실행 디렉토리 기준
 *   knob       = "backlog" / "queue_len" / "max_head" / "max_uri" / "max_headers" / "max_body"
 *              / "header_timeout_ms" / "body_timeout_ms" / "keepalive_timeout_ms"
 *              / "max_keepalive_requests" / "cgi_timeout_ms" / "cgi_max_head"
 *              / "upstream_connect_timeout_ms" / "upstream_response_timeout_ms"
 *              / "backend_fail_threshold" / "backend_cooldown_ms" / "pool_max_idle"
 *              / "shutdown_grace_ms" / "cache_max_bytes" / "cache_entry_max"
 *   knob 이름은 config_t 필드 이름과 같다. 시간은 ms, 크기는 바이트, 0은 허용하지 않는다.
 *   단, cache_max_bytes 0은 포워드 응답 캐시를 끈다 (D-042).
 *   단일 값 지시어가 반복되면 마지막 값. upstream·route·host는 나온 순서대로 누적한다.
 *
 * 예:
 *   listen 127.0.0.1:8080
 *   root ./www
 *   upstream app 127.0.0.1:8001 127.0.0.1:8002
 *   route /api/ proxy app strip
 *   route /cgi-bin/ cgi ./cgi-bin
 *   host admin.local route / proxy admin keep_host
 *   header_timeout_ms 1000
 */
#ifndef HTTPD_CORE_CONFIG_H
#define HTTPD_CORE_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CFG_MAX_UPSTREAMS 8 /* upstream 그룹 최대 개수 */
#define CFG_MAX_BACKENDS 16 /* 그룹당 백엔드 최대 개수 */
#define CFG_MAX_ROUTES 32   /* route 최대 개수 (host route 포함) */
#define CFG_NAME_MAX 32     /* upstream 이름 버퍼 크기 (NUL 포함) */
#define CFG_HOST_MAX 256    /* 호스트 이름 버퍼 크기 (NUL 포함) */
#define CFG_PATH_MAX 4096   /* 경로·prefix 버퍼 크기 (NUL 포함) */

/**
 * route의 처리 방식.
 *
 * 생성:   config_load가 route 지시어에서 채움
 * 소멸:   없음
 * 수명:   없음
 * 스레드: 값 타입
 */
typedef enum {
    ROUTE_STATIC, /* 디스크의 정적 파일 */
    ROUTE_CGI,    /* CGI 스크립트 실행 */
    ROUTE_PROXY,  /* upstream 그룹으로 전달 */
    ROUTE_FORWARD /* 포워드 프록시: absolute-form URI의 호스트로 전달 (D-041) */
} route_kind_t;

/* FORWARD route가 쓰는 upstream 이름. upstream 이름 규칙에 없는 '@'로 시작해 설정과 겹치지 않는다.
 */
#define CFG_FORWARD_UPSTREAM "@forward"

/**
 * 설정에서 고른 동시성 엔진. app이 engine_kind_t로 바꾼다.
 *
 * 생성:   config_defaults, -e, engine 지시어
 * 소멸:   없음
 * 수명:   없음
 * 스레드: 값 타입
 */
typedef enum {
    CFG_ENGINE_POOL, /* v1 스레드 풀 */
    CFG_ENGINE_LOOP  /* v2 이벤트 루프 */
} cfg_engine_t;

/**
 * 백엔드 하나의 주소 (해석 전).
 *
 * 생성:   config_load (upstream 지시어)
 * 소멸:   없음 (config_t에 내장)
 * 수명:   config_t와 같음
 * 스레드: 읽기 전용 공유 가능
 */
typedef struct {
    char host[CFG_HOST_MAX]; /* IPv4 주소 또는 호스트 이름, NUL 종료 */
    uint16_t port;           /* 1~65535 */
} backend_conf_t;

/**
 * 이름 붙은 백엔드 그룹 (upstream 지시어 한 줄).
 *
 * 생성:   config_load
 * 소멸:   없음 (config_t에 내장)
 * 수명:   config_t와 같음
 * 스레드: 읽기 전용 공유 가능
 */
typedef struct {
    char name[CFG_NAME_MAX]; /* 그룹 이름, NUL 종료, 설정 안에서 유일 */
    backend_conf_t backends[CFG_MAX_BACKENDS]; /* 설정에 나온 순서 (라운드로빈 순서) */
    size_t n_backends;                         /* 1~CFG_MAX_BACKENDS */
} upstream_conf_t;

/**
 * 라우팅 규칙 하나 (route 또는 host … route 지시어 한 줄).
 *
 * 생성:   config_load
 * 소멸:   없음 (config_t에 내장)
 * 수명:   config_t와 같음. router의 route_t가 문자열 필드를 빌린다
 * 스레드: 읽기 전용 공유 가능
 */
typedef struct {
    route_kind_t kind;         /* 처리 방식 */
    char prefix[CFG_PATH_MAX]; /* "/api/" 등 경로 접두사, '/'로 시작 */
    char host[CFG_HOST_MAX]; /* host 지시어의 호스트 이름. 빈 문자열이면 모든 Host */
    char root[CFG_PATH_MAX]; /* STATIC·CGI 디스크 디렉토리 (존재 확인됨). PROXY면 빈 문자열 */
    char upstream[CFG_NAME_MAX]; /* PROXY 대상 upstream 이름 (정의 확인됨). 그 밖에는 빈 문자열 */
    bool strip_prefix; /* PROXY: 전달 전에 prefix를 경로에서 뗀다 */
    bool keep_host;    /* PROXY: 원래 Host 유지 (false면 백엔드 host:port) */
} route_conf_t;

/**
 * 서버 전체 설정. 원시 타입만 담는다.
 *
 * 생성:   config_defaults, config_load
 * 소멸:   없음 (할당 없음, 값 타입)
 * 수명:   app이 서버 수명 동안 보관. router·handler·conn이 const 포인터로 빌림
 * 스레드: 로드 후 읽기 전용으로 공유 가능
 */
typedef struct {
    char listen_host[CFG_HOST_MAX]; /* 기본 "0.0.0.0" */
    uint16_t port;                  /* 기본 8080. 1~65535 */
    int backlog;                    /* listen backlog. 기본 128 */
    char root[CFG_PATH_MAX];  /* 기본 "./www". 규칙이 하나도 없을 때의 STATIC 루트 */
    cfg_engine_t engine;      /* 기본 CFG_ENGINE_POOL */
    int workers;              /* 기본 CPU 코어 × 2 (sysconf 실패 시 4). 1~1024 */
    int queue_len;            /* v1 작업 큐 길이. 기본 1024 */
    size_t max_head;          /* 요청 헤더 총량 상한 (바이트). 기본 8192 */
    size_t max_uri;           /* request-target 길이 상한 (바이트). 기본 2048 */
    size_t max_headers;       /* 헤더 개수 상한. 기본 64 (HTTP_MAX_HEADERS 이하) */
    uint64_t max_body;        /* 요청 본문 상한 (바이트). 기본 10 MiB (10485760) */
    int header_timeout_ms;    /* 헤더 수신 절대 마감 (ms). 기본 10000 */
    int body_timeout_ms;      /* 본문 수신 마감 (ms). 기본 30000 */
    int keepalive_timeout_ms; /* keep-alive 유휴 마감 (ms). 기본 5000 */
    int max_keepalive_requests;      /* 연결당 최대 요청 수. 기본 100 */
    int cgi_timeout_ms;              /* CGI 실행 마감 (ms). 기본 10000 */
    size_t cgi_max_head;             /* CGI 출력 헤더 상한 (바이트). 기본 8192 */
    int upstream_connect_timeout_ms; /* 업스트림 connect 마감 (ms). 기본 2000 */
    int upstream_response_timeout_ms; /* 업스트림 응답 머리 대기 마감 (ms). 기본 15000 */
    int backend_fail_threshold;       /* 연속 실패 몇 번이면 제외. 기본 3 */
    int backend_cooldown_ms;          /* 제외 유지 시간 (ms). 기본 10000 */
    int pool_max_idle;                /* 백엔드당 유휴 연결 상한. 기본 8 */
    int shutdown_grace_ms;            /* 종료 시 진행 중 연결 유예 (ms). 기본 5000 */
    int cache_max_bytes; /* 포워드 응답 캐시의 총 바이트 상한. 0이면 끔. 기본 8388608 (D-042) */
    int cache_entry_max; /* 캐시 항목 하나의 본문 상한 (바이트). 기본 1048576 */
    char access_log[CFG_PATH_MAX]; /* 접근 로그 경로. 빈 문자열이면 stderr */
    char error_log[CFG_PATH_MAX];  /* 에러 로그 경로. 빈 문자열이면 stderr */
    upstream_conf_t upstreams[CFG_MAX_UPSTREAMS]; /* 설정 순서 */
    size_t n_upstreams;                           /* 0~CFG_MAX_UPSTREAMS */
    route_conf_t routes[CFG_MAX_ROUTES];          /* 설정 순서 */
    size_t n_routes;                              /* 0~CFG_MAX_ROUTES */
} config_t;

/**
 * config_t를 기본값으로 채운다.
 *
 * 모든 바이트를 0으로 지운 뒤 config_t 필드 주석의 기본값을 넣는다. upstream·route는 0개.
 * workers는 sysconf(_SC_NPROCESSORS_ONLN) × 2 (실패하거나 1 미만이면 4, 1024 초과면 1024).
 *
 * @param out_cfg  [out] 채울 설정. NULL 불가
 *
 * 에러:    없음
 * 소유권:  할당 없음
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-CORE-17 (config_load를 통해)
 */
void config_defaults(config_t *out_cfg);

/**
 * 기본값 → -c 파일 → CLI 순으로 덮어써 설정을 만든다.
 *
 * 1. config_defaults.
 * 2. argv를 훑어 -h/--help가 있으면 다른 검사 없이 1을 돌려준다 (사용법 출력은 호출자 몫).
 * 3. -c <file>이 있으면 그 파일을 파일 머리 주석의 문법대로 읽는다.
 * 4. 나머지 CLI 옵션을 적용한다 (CLI가 파일보다 우선).
 * 5. 검증: root와 모든 STATIC·CGI route의 root가 존재하는 디렉토리, route의 upstream이 정의됨,
 *    upstream 이름 중복 없음, 개수 상한 이내.
 * 실패하면 err에 한 줄 메시지를 쓰고 -1. *out_cfg 내용은 정의되지 않는다.
 * 메시지 형식:
 *   - 파일 오류: "<파일 경로>:<줄 번호>: <설명>"  (예: "conf/bad_directive.conf:7: unknown
 * directive 'rout'")
 *   - CLI 오류:  "<옵션>: <설명>"                 (예: "-p: invalid port '70000'")
 *   - 검증 오류: 관련 경로·이름 포함 (예: "root: not a directory: ./nope")
 * -p 값은 str_to_u64로 1~65535만 허용 (0, 70000, "abc"는 오류).
 *
 * @param argc     argv 개수
 * @param argv     main의 argv. argv[0]은 프로그램 이름. NULL 불가
 * @param out_cfg  [out] 결과 설정. NULL 불가
 * @param err      [out] 실패 시 NUL 종료 메시지. errcap이 0이면 NULL 허용
 * @param errcap   err 크기
 * @return         0 성공, 1 --help 요청, -1 오류
 *
 * 에러:    -1일 때 errno = EINVAL(문법·값 오류), ENOENT·ENOTDIR(경로), 그 밖의 open errno
 * 소유권:  argv를 빌린다. 할당한 것은 반환 전에 모두 해제한다 (config_t는 값 타입)
 * 스레드:  [main-only]
 * I/O:     파일 (설정 파일 읽기, root stat)
 * 블로킹:  파일 읽기
 * 테스트:  U-CORE-17, U-CORE-21, E-CF-02, E-CF-04 · 추후 U-CORE-18~25
 */
int config_load(int argc, char **argv, config_t *out_cfg, char *err, size_t errcap);

/**
 * 사용법을 stdout에 출력한다.
 *
 * 첫 줄은 "Usage: <prog> [-p port] [-r root] [-c file] [-w workers] [-e pool|loop] [-h]",
 * 이어서 옵션마다 한 줄 설명과 기본값.
 *
 * @param prog  프로그램 이름 (argv[0]). NULL이면 "httpd"
 *
 * 에러:    없음
 * 소유권:  prog를 빌림
 * 스레드:  [main-only]
 * I/O:     stdout
 * 블로킹:  stdout 쓰기
 * 테스트:  E-CF-01
 */
void config_print_usage(const char *prog);

#endif
