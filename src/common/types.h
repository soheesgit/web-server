/* src/common/types.h
 *
 * 영역 사이를 오가는 경계 타입 (요청, 응답, 응답 계획, 프록시 작업).
 * 디렉토리 책임과 관련 테스트: src/common/README.md
 *
 * 수명 규칙 (모든 경계 타입 공통):
 *   - http_request_t·http_response_t의 모든 str_t는 파싱에 쓴 수신 버퍼를 빌린다. 그 버퍼를
 *     buf_consume·buf_append·buf_read_fd(재할당 가능)하기 전까지만 유효하다. path만 구조체가
 *     직접 소유한다. conn은 요청 하나를 끝까지 처리한 뒤에만 수신 버퍼를 consume한다.
 *   - response_plan_t는 fd·pid·업스트림 연결·버퍼를 소유하고, conn_t가 계획을 소유한다.
 *     plan_cleanup이 전부 회수한다.
 *   - proxy_job_t는 직렬화한 업스트림 요청(request 버퍼)을 소유한다. group·backend는
 *     upstream 레지스트리(서버 수명)를 빌린다. proxy_job_t가 common에 있는 이유: upstream/relay가
 *     이 타입을 쓰지만 upstream은 handler를 include할 수 없다. proxy_begin이 만들고
 *     proxy_job_cleanup이 정리하며, 그 사이 relay가 attempts 외의 진행 필드(sent_any)를 갱신한다.
 */
#ifndef HTTPD_COMMON_TYPES_H
#define HTTPD_COMMON_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "core/str.h"
#include "core/buf.h"

#define HTTP_MAX_HEADERS 64 /* http_headers_t가 담는 최대 헤더 수 */
#define HTTP_PATH_MAX 4096  /* http_request_t.path 버퍼 크기 (NUL 포함) */

struct upstream_conn; /* upstream/upstream.h에서 정의 */
struct upstream_group;
struct backend;

/**
 * 요청 메서드. 지원하지 않는 메서드는 HTTP_OTHER (파서는 501로 거부).
 * 역할: 요청에서 받은 메서드를 서버가 지원하는 종류로 구분한다.
 *
 * 생성:   request_parse_head
 * 소멸:   없음
 * 수명:   없음
 * 스레드: 값 타입
 */
typedef enum { HTTP_GET, HTTP_HEAD, HTTP_POST, HTTP_OTHER } http_method_t;

/**
 * 파서 결과. DONE 완료, INCOMPLETE 더 받아야 함, ERROR 오류(out_status에 상태 코드).
 * 역할: 파싱을 계속할지, 성공으로 끝낼지, 오류 응답을 만들지 호출자에게 알려준다.
 *
 * 생성:   각 *_parse_* 함수
 * 소멸:   없음
 * 수명:   없음
 * 스레드: 값 타입
 */
typedef enum { HTTP_PARSE_DONE, HTTP_PARSE_INCOMPLETE, HTTP_PARSE_ERROR } http_parse_result_t;

/**
 * 본문 길이 방식. NONE 본문 없음, LENGTH Content-Length, CHUNKED chunked, CLOSE 연결 종료까지.
 * 역할: 다음 본문을 어떤 규칙으로 읽고 끝을 판정할지 나타낸다.
 *
 * 생성:   http_resolve_framing, 처리기
 * 소멸:   없음
 * 수명:   없음
 * 스레드: 값 타입
 */
typedef enum { FRAMING_NONE, FRAMING_LENGTH, FRAMING_CHUNKED, FRAMING_CLOSE } http_framing_kind_t;

/**
 * 본문 경계 추적 결과. MORE 아직 남음, DONE 본문 끝, ERROR 형식 오류.
 * 역할: 본문 추적기가 입력 조각을 처리한 뒤의 진행 결과를 나타낸다.
 *
 * 생성:   http_body_track
 * 소멸:   없음
 * 수명:   없음
 * 스레드: 값 타입
 */
typedef enum { BODY_MORE, BODY_DONE, BODY_ERROR } http_body_result_t;

/**
 * 헤더 한 줄 (이름과 값). 값은 앞뒤 OWS를 뗀 상태.
 * 역할: HTTP 메시지의 헤더 이름 하나와 값 하나를 함께 보관한다.
 *
 * 생성:   http_parse_headers, fake_request_add_header
 * 소멸:   없음
 * 수명:   name·value가 빌린 버퍼의 수명
 * 스레드: 값 타입
 */
typedef struct {
    str_t name;  /* 헤더 이름 (원래 대소문자 유지), 길이 ≥ 1 */
    str_t value; /* 값. 앞뒤 OWS 제거. 빈 값은 길이 0 */
} http_header_t;

/**
 * 헤더 목록 (받은 순서 유지, 같은 이름 중복 보존).
 * 역할: 메시지에 포함된 헤더들을 받은 순서대로 담는다.
 *
 * 생성:   http_parse_headers
 * 소멸:   없음 (내장 배열)
 * 수명:   각 http_header_t의 수명
 * 스레드: 값 타입
 */
typedef struct {
    http_header_t v[HTTP_MAX_HEADERS]; /* v[0..n) 유효 */
    size_t n;                          /* 0~HTTP_MAX_HEADERS */
} http_headers_t;

/**
 * 본문 길이 방식과 길이.
 * 역할: 본문을 읽을 때 적용할 방식과 필요한 바이트 수를 한데 담는다.
 *
 * 생성:   http_resolve_framing, 처리기
 * 소멸:   없음
 * 수명:   없음
 * 스레드: 값 타입
 */
typedef struct {
    http_framing_kind_t kind; /* 방식 */
    uint64_t length;          /* LENGTH일 때만 본문 바이트 수. 그 밖에는 0 */
} http_framing_t;

/**
 * 요청 파싱 제한값. app이 config_t에서 만든다.
 * 역할: 요청 헤더, 헤더 개수, URI, 본문이 넘을 수 없는 상한을 전달한다.
 *
 * 생성:   app (config_t 변환), 테스트
 * 소멸:   없음
 * 수명:   app 수명
 * 스레드: 읽기 전용 공유 가능
 */
typedef struct {
    size_t max_head;    /* 헤더 총량 상한 (바이트, 앞 빈 줄 포함). 기본 8192 */
    size_t max_uri;     /* request-target 길이 상한 (바이트). 기본 2048 */
    size_t max_headers; /* 헤더 개수 상한. 1~HTTP_MAX_HEADERS, 기본 64 */
    uint64_t max_body;  /* 요청 본문 상한 (바이트). 기본 10485760 */
} http_limits_t;

/**
 * 본문 경계 추적기. 바이트를 복사하지 않고 본문의 끝이 어디인지만 센다.
 * 역할: 여러 번 나뉘어 도착하는 본문 조각을 이어서 읽고 본문 끝을 판정한다.
 *
 * 생성:   http_body_tracker_init
 * 소멸:   없음 (값 타입)
 * 수명:   본문 하나
 * 스레드: [per-object]
 */
typedef struct {
    http_framing_t f; /* 추적 중인 방식 */
    uint64_t remaining; /* LENGTH: 남은 바이트, CHUNKED: 현재 청크의 남은 데이터 바이트 */
    int state; /* CHUNKED 파서 상태. 값의 의미는 http_framing.c가 정의 (0 = 시작) */
} http_body_tracker_t;

/**
 * 파싱된 요청 하나.
 * 역할: 요청 줄과 헤더를 파싱한 결과를 보관하고, 본문 수신 결과도 연결한다.
 *
 * 생성:   request_parse_head (body 제외), conn (body), fake_request (테스트)
 * 소멸:   없음 (값 타입. 빌린 버퍼는 conn이 관리)
 * 수명:   str_t 필드는 수신 버퍼를 빌림 (파일 머리의 수명 규칙)
 * 스레드: [per-object]
 */
typedef struct {
    http_method_t method; /* 메서드 */
    str_t method_raw;     /* 원본 메서드 토큰 */
    str_t target;         /* 원본 request-target (디코딩 안 함) */
    str_t raw_path; /* target의 경로 부분 (디코딩 안 함, '?' 앞, 프록시 전달용).
                       absolute-form이면 authority 뒤 경로 */
    char path[HTTP_PATH_MAX]; /* 디코딩·정규화된 경로, NUL 종료. 이 구조체가 소유 */
    str_t query;              /* '?' 뒤, '#' 앞, 디코딩 안 함. 없으면 len 0 */
    int minor;                /* HTTP/1.minor (0 또는 1) */
    http_headers_t headers; /* 받은 헤더 전부 (순서 유지) */
    str_t host; /* absolute-form이면 URI의 authority, 아니면 Host 헤더 값. 없으면 len 0 */
    http_framing_t framing; /* NONE 또는 LENGTH (요청 chunked는 501) */
    bool keep_alive;        /* http_keep_alive 결과 */
    size_t head_len; /* 수신 버퍼에서 요청 라인+헤더가 차지한 바이트(앞 빈 줄 포함) */
    str_t body; /* 본문 수신 완료 후 conn이 채움. 본문 없으면 len 0 */
} http_request_t;

/**
 * 파싱된 업스트림 응답 머리 하나.
 * 역할: 프록시 대상 서버가 보낸 상태 줄과 헤더를 보관한다.
 *
 * 생성:   response_parse_head
 * 소멸:   없음
 * 수명:   str_t 필드는 업스트림 수신 버퍼(upstream_conn_t.rbuf)를 빌림
 * 스레드: [per-object]
 */
typedef struct {
    int status;             /* 100~599 */
    str_t reason;           /* 사유 문구. 비어 있을 수 있음 */
    int minor;              /* HTTP/1.minor */
    http_headers_t headers; /* 응답 헤더 (순서 유지) */
    http_framing_t framing; /* 본문 방식 (http_resolve_framing, is_response = true) */
    bool keep_alive;        /* 업스트림 연결을 재사용할 수 있는가 */
    size_t head_len;        /* rbuf에서 상태 줄+헤더가 차지한 바이트 */
} http_response_t;

/**
 * 클라이언트에 보낼 응답 머리.
 * 역할: 응답 상태, 헤더, 본문 전달 방식을 응답 계획에 제공한다.
 *
 * 생성:   plan_init이 초기화, 처리기(plan_set_mem, plan_error, plan_add_header 등)가 채움
 * 소멸:   plan_cleanup (fields 해제)
 * 수명:   response_plan_t와 같음
 * 스레드: [per-object]
 */
typedef struct {
    int status;             /* 응답 상태 코드. 100~599. plan_init 직후 0 */
    buf_t fields;           /* "Name: value\r\n" 누적. Date·Server·Content-Length·Connection·
                               Transfer-Encoding은 넣지 않음(writer가 추가). 이 구조체가 소유 */
    http_framing_t framing; /* 본문 방식. LENGTH면 writer가 Content-Length를 붙임 */
    bool close;             /* 응답 후 연결을 닫는다 */
} http_head_out_t;

/**
 * 응답 본문의 출처.
 * 역할: 응답 본문을 메모리, 파일, CGI, 업스트림 중 어디서 가져올지 고른다.
 *
 * 생성:   처리기
 * 소멸:   없음
 * 수명:   없음
 * 스레드: 값 타입
 */
typedef enum {
    BODY_NONE,    /* 본문 없음 (HEAD, 204, 304, 301 등) */
    BODY_MEM,     /* body.mem.data */
    BODY_FILE,    /* body.file 구간 */
    BODY_PIPE,    /* CGI 자식의 stdout */
    BODY_UPSTREAM /* 업스트림 연결의 응답 본문 */
} body_kind_t;

/**
 * 처리기가 만드는 응답 계획: 무엇을 보낼지만 담고 실제 송신은 conn이 한다.
 * 역할: 응답 머리와 본문 자원을 묶어 두고, 연결 처리기가 이를 실제로 전송하게 한다.
 *
 * 생성:   plan_init 후 처리기 (static_handle, cgi_handle, proxy_on_head, plan_error 등)
 * 소멸:   plan_cleanup (멱등)
 * 수명:   conn_t가 요청 하나 동안 소유. fd·pid·업스트림 연결·버퍼를 이 계획이 소유
 * 스레드: [per-object]
 */
typedef struct {
    http_head_out_t head; /* 응답 머리 */
    bool head_pending; /* BODY_PIPE: CGI 출력에서 헤더를 읽어야 확정 (cgi_parse_head) */
    body_kind_t kind;  /* 본문 출처. 아래 union에서 이 kind의 멤버만 유효 */
    union {
        struct {
            buf_t data; /* 본문 바이트. 소유 */
        } mem;
        struct {
            int fd;    /* 열린 일반 파일 (cloexec). 없음 -1. 소유 */
            off_t off; /* 보낼 시작 오프셋 (바이트) */
            off_t len; /* 보낼 바이트 수 */
        } file;
        struct {
            int out_fd;          /* CGI stdout을 읽는 쪽 (cloexec). 없음 -1. 소유 */
            int in_fd;           /* CGI stdin에 쓰는 쪽 (cloexec). 없음 -1. 소유 */
            pid_t pid;           /* CGI 자식 pid. 없음 0. 회수(waitpid) 책임 */
            buf_t in_data;       /* stdin에 쓸 요청 본문 사본. 쓴 만큼 consume. 소유 */
            int64_t deadline_ms; /* CGI 실행 마감 (platform_now_ms 기준). 없음 -1 */
        } pipe;
        struct {
            struct upstream_conn *conn; /* 응답 본문을 읽을 업스트림 연결. 없음 NULL. 소유 */
            http_body_tracker_t tracker; /* 업스트림 응답 본문의 끝 판정 */
        } upstream;
    } body;
} response_plan_t;

/**
 * 프록시 시도 한 번의 실패 이유. relay가 돌려주고 proxy_on_fail이 판단한다.
 * 역할: 프록시 연결이나 응답 수신이 실패한 지점을 재시도 판단에 전달한다.
 *
 * 생성:   relay
 * 소멸:   없음
 * 수명:   없음
 * 스레드: 값 타입
 */
typedef enum {
    PFAIL_CONNECT,     /* connect 실패·타임아웃 (요청을 1바이트도 보내지 않음) */
    PFAIL_SEND,        /* 요청 송신 실패 */
    PFAIL_TIMEOUT,     /* 응답 머리 대기 마감 초과 */
    PFAIL_BAD_RESPONSE /* 응답 머리 형식 오류, 머리 전에 EOF */
} proxy_fail_t;

/**
 * 프록시 실패 후 다음 행동.
 * 역할: 실패한 백엔드를 바꿔 재시도할지, 오류 응답으로 끝낼지 나타낸다.
 *
 * 생성:   proxy_on_fail
 * 소멸:   없음
 * 수명:   없음
 * 스레드: 값 타입
 */
typedef enum {
    PROXY_RETRY,  /* job->backend가 바뀌었으니 다시 relay */
    PROXY_GIVE_UP /* out_plan에 에러 계획이 들어 있음 */
} proxy_next_t;

/**
 * 프록시 요청 하나의 진행 상태.
 * 역할: 업스트림으로 보낼 요청과 현재 백엔드, 재시도 진행 상황을 묶어 관리한다.
 *
 * 생성:   proxy_begin
 * 소멸:   proxy_job_cleanup (멱등)
 * 수명:   conn이 요청 하나 동안 소유. group·backend는 레지스트리(서버 수명)를 빌림
 * 스레드: [per-object]
 */
typedef struct {
    struct upstream_group *group; /* 대상 그룹. 빌림 */
    struct backend *backend;      /* 현재 시도 대상. 빌림. 없음 NULL */
    buf_t request;                /* 직렬화된 업스트림 요청 (헤더 + 본문). 소유 */
    http_method_t method; /* 원래 요청 메서드 (응답 framing·재시도 판단용) */
    int attempts;         /* 지금까지 시도한 백엔드 수 (proxy_begin 직후 1) */
    int max_attempts;     /* 최대 시도 수 = 그룹의 백엔드 수 */
    bool sent_any; /* 현재 시도에서 요청을 1바이트라도 보냈는가 (relay가 갱신) */
    int client_minor;       /* 클라이언트 HTTP/1.minor */
    bool client_keep_alive; /* 클라이언트 연결 유지 여부 */
} proxy_job_t;

#endif
