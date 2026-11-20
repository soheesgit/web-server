# 인터페이스 규약

이 문서는 저장소 전체에 적용되는 규약의 단일 출처다. 함수·타입 하나하나의 명세는 각 공개 헤더의 주석에, 디렉토리의 책임은 각 `src/<dir>/README.md`에 있다. 여기에는 그 둘에 공통으로 적용되는 규칙만 둔다.

## 1. 정보의 주인 (단일 출처)

같은 내용을 두 곳에 쓰지 않는다. 다른 곳에서는 이름이나 ID로만 가리킨다.

| 정보 | 유일한 위치 | 다른 곳에서는 |
| --- | --- | --- |
| 함수의 동작·인자·반환값·에러·소유권 | 공개 헤더의 선언 바로 위 `/** */` 주석 | 함수 이름만 |
| 구조체·필드·열거 | 헤더의 정의 위 주석과 필드 옆 주석 | 타입 이름만 |
| 파일·디렉토리의 책임, 파일 역할, 관련 테스트 | `src/<dir>/README.md` | 헤더 맨 위에는 한 줄 요약만 |
| 전역 규약 | 이 문서 | "CONVENTIONS.md를 따른다" |
| 동작 기대값 | 테스트 코드 (`tests/`) | 테스트 ID만 |
| 결정과 미결 사항 | `docs/DECISIONS.md` | 결정 번호(D-0NN)만 |

주석과 문서는 한국어, 식별자와 파일 이름은 영어로 쓴다.

## 2. 언어와 빌드

- C11 + POSIX.1-2008 (`-std=c11 -D_POSIX_C_SOURCE=200809L`). OS 확장(`-D_GNU_SOURCE`, `-D_DARWIN_C_SOURCE`)은 Makefile이 붙인다.
- OS마다 다른 API는 `core/platform.c`와 `engine/poller_*.c`에서만 쓴다.
- 경고 0 (`-Werror`). `-Wmissing-prototypes`가 켜져 있으므로 파일 내부 함수는 반드시 `static`.
- 외부 라이브러리 금지. 링크는 `-lpthread`만. 테스트 도구는 Python 3 표준 라이브러리, `curl`, `nc`만.

## 3. 이름

| 대상 | 규칙 | 예 |
| --- | --- | --- |
| 공개 함수 | `접두사_동사[_목적어]`, 접두사는 파일·모듈 이름 | `buf_append`, `router_match` |
| 타입 | `snake_case_t` | `http_request_t` |
| 열거값·매크로 | 대문자 + 모듈 접두사 | `HTTP_PARSE_DONE`, `CFG_MAX_ROUTES` |
| 출력 인자 | 매개변수 끝, `out_` 접두사. 입출력 겸용은 `inout_` | `out_status`, `inout_off` |
| include guard | `HTTPD_<DIR>_<FILE>_H` | `HTTPD_CORE_STR_H` |
| 디렉토리 내부 헤더 | `src/<dir>/internal_<이름>.h`, 다른 디렉토리에서 include 금지 | `internal_chunk.h` |

## 4. 반환값과 에러

| 함수 종류 | 성공 | 실패 |
| --- | --- | --- |
| 동작 (`int`) | `0` | `-1` + `errno` |
| 생성 (`*_new`, 조회 포인터) | 포인터 | `NULL` (+ 필요하면 `err` 버퍼에 메시지) |
| 갈래가 여럿인 결과 | 모듈 전용 enum (`http_parse_result_t`, `conn_status_t` 등) | enum의 오류 값 |
| 조회 (`const T *`) | 포인터 | `NULL` = 없음 |
| 읽기·쓰기 (`ssize_t`) | 처리한 바이트 수 (`0` = EOF) | `-1` + `errno` |

- HTTP 상태 코드로 끝나는 오류는 반환값과 분리해 `int *out_status`로 돌려준다.
- 사람이 읽을 메시지가 필요한 생성 함수는 `char *err, size_t errcap`을 받는다. `err`에는 NUL 종료 문자열을 쓰고, 넘치면 자른다.
- 정리 함수(`*_free`, `*_cleanup`, `*_close`)는 멱등이고 `NULL`이나 0으로 채운 객체를 받아도 안전하다. `errno`를 바꾸지 않는다.
- `assert`는 프로그래머 실수에만 쓴다. 네트워크 입력 오류에는 쓰지 않는다.
- 미구현 함수는 `core/stub.h`의 `STUB_RETURN`·`STUB_VOID`로 `errno = ENOSYS`를 남긴다.

### HTTP 상태 코드를 정하는 곳

상태 코드마다 그 값을 결정하는 함수가 하나씩 있다. 조건은 그 함수의 헤더 주석에 있다.

| 상태 | 결정하는 함수 |
| --- | --- |
| 400, 414, 431, 501, 505, 413 (요청 머리) | `request_parse_head` (헤더 끝은 `http_find_head_end`, 헤더 줄은 `http_parse_headers`, 길이는 `http_resolve_framing`) |
| 408 (헤더·본문 수신 마감) | `conn_on_timeout`, `conn_serve_blocking` |
| 404 (라우팅 실패) | `conn` (`router_match`가 NULL일 때) |
| 301, 403, 404, 405 (정적) | `static_handle` |
| 403, 404 (CGI 시작), 502·504 (CGI 출력) | `cgi_handle`, `cgi_parse_head`, `conn` (CGI 마감) |
| 502, 503, 504 (프록시) | `proxy_begin`, `proxy_on_fail` |
| 503 (작업 큐 가득) | `engine_callbacks_t.on_busy`를 구현하는 쪽 (app/downstream) |
| 500 (그 밖의 내부 오류) | 오류를 만난 처리기·conn이 `plan_error(…, 500)` |

에러 본문은 항상 `http_error_body`가 만든다.

## 5. 소유권

| 용어 | 뜻 |
| --- | --- |
| 빌림 | 호출자가 가진 메모리를 잠시 본다. 해제하지 않고, 수명은 원래 주인을 따른다 |
| 소유 | 이 객체가 할당했고 이 객체의 정리 함수가 해제한다 |
| 이전 | 호출이 성공하면 소유가 넘어간다. 실패하면 넘어가지 않는다 (헤더에 따로 적은 경우 제외) |

- `str_t`는 항상 빌림이다. NUL 종료를 가정하지 않는다. `http_request_t`·`http_response_t`의 `str_t`는 파싱에 쓴 수신 버퍼를 빌리며, 그 버퍼를 `buf_consume`하거나 재할당하기 전까지만 유효하다. `path`만 구조체가 직접 소유한다.
- `_new`/`_free`, `_init`/`_cleanup`은 짝이다.
- fd·자식 pid·업스트림 연결은 `response_plan_t`가 소유하고, 계획은 `conn_t`가 소유한다.
- fd의 "없음"은 `-1`, pid의 "없음"은 `0`. 서버가 여는 모든 fd는 `FD_CLOEXEC`.
- 제한값과 시각은 전역에서 읽지 않고 인자로 받는다 (`http_limits_t *`, `time_t now`, `int64_t now_ms`).

## 6. 스레드·블로킹 표기

공개 함수 주석의 `스레드:` 항목에 다음 중 하나를 적는다.

| 표기 | 뜻 |
| --- | --- |
| `[thread-safe]` | 여러 스레드가 같은 객체로 동시에 불러도 된다 |
| `[per-object]` | 객체가 다르면 동시에 불러도 된다. 같은 객체는 한 번에 한 스레드 |
| `[main-only]` | 시작·종료 시 메인 스레드에서만 |

`블로킹:` 항목에는 멈출 수 있는 호출과 그 상한을 적는다 (`없음`이면 없음).

| 영역 | 허용되는 I/O |
| --- | --- |
| `core/str`, `common/`, `router/`, `downstream/`의 파서·직렬화(`request_parser`, `uri`, `response_writer`), `upstream/`의 직렬화·파서(`request_writer`, `response_parser`) | 없음 (I/O 자체 금지) |
| `handler/` | 짧게 끝나는 로컬 호출만: `open`, `fstat`, `realpath`, `pipe`, `fork`, `execve`. 소켓 I/O 금지 |
| `upstream/pool`·`lb`·`backend` | non-blocking `connect` 시작까지. 완료를 기다리지 않음 |
| `upstream/relay`, `downstream/conn` | 소켓 I/O 실행 (v1은 타임아웃 있는 blocking, v2는 non-blocking) |
| `engine/` | poller 대기, 스레드 동기화. v2 콜백 안에서 블로킹 금지 |
| `core/log`, `core/config`, `core/platform` | 각 헤더 주석에 적은 것만 |

## 7. include 방향

다른 디렉토리는 공개 헤더만 include한다. 표에 없는 방향은 `scripts/lint_includes.py`가 실패로 처리한다. 같은 디렉토리 안은 자유다. include 경로는 `src/` 기준(`#include "common/types.h"`)으로 쓴다.

| 디렉토리 | include 가능 |
| --- | --- |
| `core` | 없음 |
| `common` | `core` |
| `router` | `core`, `common` |
| `upstream` | `core`, `common` |
| `handler` | `core`, `common`, `router`, `upstream` |
| `engine` | `core` |
| `downstream` | `core`, `common`, `router`, `handler`, `upstream`, `engine` |
| `app` | 전부 |

모든 헤더는 자기충족이다: 그 헤더 하나만 include해도 컴파일된다 (`scripts/check_headers.sh`).

## 8. 금지 함수

`gets`, `sprintf`, `strcpy`, `strcat`, `system`, `popen`, `strtok`, `atoi`, `gmtime`, `strerror`, 그리고 `<curl/…>`, `<openssl/…>`, `<event2/…>`, `<uv.h>`. 대신 `snprintf`, `memcpy` + 길이, `strtoull`, `gmtime_r`, `strerror_r`, `fork` + `execve`를 쓴다. `scripts/lint_forbidden.sh`가 `src/`를 검사한다.

## 9. 명세 주석 템플릿

모든 공개 함수에 아래 항목을 빠짐없이 적는다. 해당 없는 항목은 `없음`이라고 적는다.

```c
/**
 * <한 줄 요약>
 *
 * <동작 설명: 입력 조건별로 무엇을 하는가>
 *
 * @param <이름>      <의미>. 포인터면 NULL 허용 여부
 * @param out_<이름>  [out] 성공 시 무엇이 채워지고, 실패 시 무엇이 남는가
 * @return <값별 의미>
 *
 * 에러:    <errno 또는 out_status 값 목록과 조건>
 * 소유권:  <할당하는가, 무엇을 빌리고 무엇을 가져가는가, 누가 해제하는가>
 * 스레드:  [thread-safe] | [per-object] | [main-only]
 * I/O:     없음 | 파일 | 소켓 | 프로세스
 * 블로킹:  없음 | <어떤 호출이 얼마나 멈출 수 있는가>
 * 테스트:  <이 함수를 검증하는 테스트 ID>
 */
```

구조체·열거에는 다음을 적고, 모든 필드에 한 줄 주석(범위, 단위, "없음" 값, 소유 여부)을 단다.

```c
/**
 * <무엇을 나타내는 타입인가>
 *
 * 생성:   <어떤 함수가 만들거나 채우는가>
 * 소멸:   <어떤 함수가 정리하는가>
 * 수명:   <언제까지 유효한가, 무엇을 빌리는가>
 * 스레드: <공유 가능 여부>
 */
```

헤더 파일 맨 위에는 경로, 한 줄 요약, "디렉토리 책임과 관련 테스트: src/<dir>/README.md"만 둔다.

## 10. 공개 인터페이스 변경 절차

공개 헤더(`src/**/*.h`)의 시그니처·구조체 필드·열거값을 바꿔야 하면:

1. `docs/DECISIONS.md`에 "제안" 상태 항목을 추가한다.
2. 헤더만 바꾼 PR을 따로 열고 **세 명 모두** 승인을 받는다.
3. 영향을 받는 호출부·스텁·테스트는 같은 PR에서 고친다.
4. 머지 후 다른 작업 브랜치는 `develop`에 rebase한다.

구현 PR과 섞지 않는다. 같은 규칙으로 `docs/CONVENTIONS.md`, `src/common/`, 공개 헤더를 바꾸는 PR은 세 명 모두 승인해야 한다 (GitHub 설정으로 강제하지 않고 팀 규칙으로 지킨다).

## 11. 커밋과 브랜치

- 기능 브랜치: `feat/<dir>-<주제>`, `fix/<dir>-<주제>`. 대상은 항상 `develop`, 머지는 squash.
- 커밋 메시지: Conventional Commits, scope는 디렉토리 이름 (`feat(downstream): …`). 본문에 통과시킨 테스트 ID를 적는다.
- 훅을 건너뛰지 않는다 (`--no-verify` 금지). force push 금지.
