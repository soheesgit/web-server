# 아키텍처

httpd는 앞쪽(downstream)에서는 서버로, 뒤쪽(upstream)에서는 클라이언트로 동작하는 HTTP/1.1 웹서버 겸 리버스 프록시다. 가운데의 router와 handler가 두 면을 잇는다. 각 영역은 디렉토리 하나이며, 영역의 책임과 파일 목록은 해당 `src/<dir>/README.md`에 있다. 영역 사이 규약은 [CONVENTIONS.md](CONVENTIONS.md)를 따른다.

## 1. 두 면 구조

```
            downstream (클라이언트 쪽)                          upstream (백엔드 쪽)
 client ──▶ listener ─▶ conn ──────────────────────────────▶ relay ─▶ pool/lb ─▶ backend
            (accept)    │ 읽기·request_parse_head              ▲       (connect·재사용)
                        │ 본문 수신                            │ proxy_job_t
                        ▼                                      │
                     router_match ─▶ handler_dispatch ─────────┘
                        (route_t)     static / cgi / proxy
                                         │
                                         ▼ response_plan_t
            conn ◀───────────────────────┘
             │ response_write_head + 본문 송신 (mem · file · pipe · upstream)
             ▼
          client          engine: 모든 fd(클라이언트·pipe·업스트림)를 스레드 풀(v1) 또는 이벤트 루프(v2)로 돌림
                          core: str · buf · log · config · platform     common: 경계 타입 + 양쪽 면 공용 HTTP 규칙
```

- **downstream/**: 대기 소켓, 연결 상태(`conn_t`), 요청 파싱, URI 정규화, 응답 직렬화, 본문 송신, 연결 유지 판정. 응답 *내용*을 어떻게 만드는지는 모른다.
- **router/**: 경로·Host 규칙으로 `route_t`를 고른다. 소켓과 파일을 모른다.
- **handler/**: 무엇을 보낼지 정해 `response_plan_t`를 만든다. 클라이언트 소켓을 모른다.
- **upstream/**: 백엔드 레지스트리·로드 밸런싱·연결 풀, 업스트림 요청 직렬화, 응답 머리 파싱, 프록시의 기다림이 있는 I/O(연결 획득·요청 송신·응답 머리 수신).
- **common/**: 경계 타입(`http_request_t`, `http_response_t`, `response_plan_t`, `proxy_job_t`)과 양쪽 면이 같이 쓰는 RFC 7230 규칙(헤더 끝 탐색, 헤더 파싱, 본문 길이, 본문 경계 추적, keep-alive, 상태 문구).
- **engine/**: fd와 콜백만 안다. HTTP를 모른다.
- **core/**: 기반. 다른 영역을 모른다.
- **app/**: 위 전부를 조립하고 `main`을 둔다.

## 2. 처리기는 계획을 돌려주고, 전송은 downstream이 한다

처리기(static·cgi·proxy)는 소켓에 아무것도 쓰지 않는다. 대신 "무엇을 보낼지"를 `response_plan_t`로 돌려준다.

| 계획 종류 | 내용 | 누가 만드나 |
| --- | --- | --- |
| `BODY_MEM` | 메모리 버퍼 (에러 페이지 등) | `plan_set_mem`, `plan_error` |
| `BODY_FILE` | 열린 파일 fd + 구간 | `static_handle` |
| `BODY_PIPE` | CGI 자식의 stdin·stdout pipe, pid, 마감 시각. 응답 머리는 CGI 출력에서 확정 | `cgi_handle` |
| `BODY_UPSTREAM` | 업스트림 연결 + 본문 경계 추적기 | `proxy_on_head` |

conn이 계획을 실행한다: `response_write_head`로 머리를 쓰고 종류에 맞게 본문을 보낸 뒤 `plan_cleanup`으로 자원을 회수한다. 이렇게 하면 처리기는 소켓 없이 테스트할 수 있고, v1(blocking)과 v2(non-blocking) 전송 코드가 conn 한곳에만 있다.

## 3. 한 요청의 흐름

1. 커널이 3-way handshake를 마친 연결을 대기 소켓의 accept 큐에 넣는다. engine이 `listener_accept`로 받아 `on_accept` 콜백(conn_new)에 넘긴다.
2. conn이 수신 버퍼를 채우고 `request_parse_head`로 `http_request_t`를 만든다. 본문이 있으면 Content-Length만큼 더 받는다.
3. `router_match`가 `route_t`를 고른다. 없으면 404.
4. `handler_dispatch`가 route 종류에 따라 처리기를 부른다.
   - **정적**: 루트 + 정규화 경로 → realpath 접두사 검사 → open·fstat → `BODY_FILE`.
   - **CGI**: 스크립트 확인 → pipe 2개 → fork·execve → `BODY_PIPE`. conn이 요청 본문을 stdin에 쓰면서 stdout을 읽고, `cgi_parse_head`로 머리를 확정한다.
   - **프록시**: v1 conn이 그룹에서 백엔드 시도를 예약하고 선택 대상에 맞는 요청을 직렬화한다. 이후는 4절.
5. conn이 `response_write_head`로 상태 줄·헤더를 쓰고 본문을 보낸다. 접근 로그를 한 줄 남긴다.
6. keep-alive면 처리한 만큼 수신 버퍼를 비우고 2로, 아니면 닫고 자원을 회수한다.

## 4. 프록시: 판단과 기다림을 나눈다

나누는 기준은 "판단인가, 기다림이 있는 I/O인가"다.

- **판단**은 `handler/proxy.c`: 실패 시 재시도할지(`proxy_on_fail`), 응답 머리를 어떻게 클라이언트 머리로 바꿀지(`proxy_on_head`). 소켓 없이 테스트할 수 있다.
- **기다림**은 `upstream/relay.c`: 백엔드 시도 예약, 연결 획득, 요청 송신, 응답 머리 수신. v1 conn은 relay 단계마다 업스트림과 클라이언트 fd를 함께 poll한다. 같은 대상의 유휴 연결에서 0바이트 송신 실패가 난 경우 새 연결을 한 번 연다.
- **응답 본문 중계**는 conn: `BODY_UPSTREAM` 계획을 받아 업스트림 fd에서 읽은 바이트를 경계 추적기가 끝을 알릴 때까지 클라이언트에 보낸다. 끝나면 연결을 풀에 반납할지 닫을지 정한다.

conn이 이 셋을 엮는 정확한 순서는 `downstream/conn.h`의 `conn_serve_blocking` 명세에 있다.

## 5. 동시성 엔진 v1 / v2

| | v1 스레드 풀 (`engine_pool.c`) | v2 이벤트 루프 (`engine_loop.c`) |
| --- | --- | --- |
| accept | 메인 스레드가 accept해 작업 큐에 넣음 | 루프가 대기 소켓 읽기 이벤트에서 accept |
| 연결 처리 | 워커가 `serve_blocking`으로 연결 하나를 끝까지 (타임아웃 있는 blocking I/O) | 루프가 fd 이벤트마다 `on_event`, 마감마다 `on_timeout` |
| 추가 fd (pipe·업스트림) | 워커가 직접 기다림 | conn이 `engine_watch`로 등록하고 `CONN_WAIT_FD` |
| 큐 가득 | `on_busy` → 503 후 close (D-005) | 해당 없음 |
| poller | 쓰지 않음 | `poller_epoll.c` / `poller_kqueue.c` / `poller_poll.c` 중 빌드 대상 OS에 맞는 하나 |

엔진은 연결 객체를 `void *`로만 다루며 HTTP를 모른다. conn은 두 엔진에 같은 객체를 쓴다(`conn_env_t.nonblocking`으로 경로 선택).

## 6. 시작과 종료

`main` → `config_load` → `log_open` → SIGPIPE 무시 → SIGINT·SIGTERM 핸들러(플래그만 설정) → `app_new`(limits 변환 → upstream 레지스트리 → router → listener → engine) → `app_run` → `app_free` → `log_close`. 종료 플래그가 서면 engine은 새 연결을 받지 않고 진행 중 연결을 유예 시간만큼 기다린다. 종료 코드는 `src/app/main.c` 머리 주석에 있다.

## 7. 운영 문서

Stage 4의 설정, 복구 상태, 연결 소유권 및 Forward Proxy 범위는 [Stage 4 프록시 구현 및 검증](PROXY_STAGE4.md)에서 확인할 수 있다.
