# handler/

## 책임
요청에 대해 "무엇을 보낼지"를 정해 `response_plan_t`를 만든다. STATIC(파일 확인·열기), CGI(스크립트 확인·fork·exec), PROXY의 판단 단계(백엔드 선택, 재시도 판단, 응답 머리 변환)를 맡는다. 소켓에는 아무것도 쓰지 않는다. 아키텍처 그림의 STATIC 6단계, CGI 6단계, PROXY 판단 블록, 응답 계획 블록이다.

## 알아도 되는 것 / 몰라야 하는 것
| 알아도 되는 것 | 몰라야 하는 것 |
| --- | --- |
| 파일시스템, CGI 프로세스, 업스트림 API | 클라이언트 소켓 |

## 파일
| 파일 | 역할 | 그림의 블록 | 공개 함수 |
| --- | --- | --- | --- |
| `handler.h` | 처리기 진입점 선언 (static·cgi·proxy), 처리기 공통 계약 | — | `handler_dispatch`, `static_handle`, `cgi_handle`, `cgi_parse_head`, `proxy_begin`, `proxy_on_fail`, `proxy_on_head`, `proxy_job_cleanup` |
| `plan.h` `plan.c` | 응답 계획 초기화·메모리 본문·에러 계획·헤더 추가·정리. `handler_dispatch`도 여기 둔다 | 응답 계획 | `plan_init`, `plan_set_mem`, `plan_error`, `plan_add_header`, `plan_cleanup`, `handler_dispatch` |
| `mime.h` `mime.c` | 확장자 → Content-Type | STATIC: MIME | `mime_for_path` |
| `static.c` | 정적 파일 계획 | STATIC 6단계 | `static_handle` |
| `cgi.c` | CGI 시작, CGI 응답 머리 파싱 | CGI 6단계 | `cgi_handle`, `cgi_parse_head` |
| `proxy.c` | 프록시 판단 | PROXY: 그룹 선택, 재시도 판단, 중계 준비 | `proxy_begin`, `proxy_on_fail`, `proxy_on_head`, `proxy_job_cleanup` |

## 의존
- include 가능: `core`, `common`, `router`, `upstream`
- 이 디렉토리를 쓰는 곳: `downstream`, `app`

## 블로킹·스레드 규칙
짧게 끝나는 로컬 호출만 허용: `open`, `fstat`, `realpath`, `access`, `pipe`, `fork`, `execve`, 그리고 계획 정리의 `kill`·`waitpid`. 소켓 I/O 금지. 처리기 함수는 객체가 다르면 `[thread-safe]`. fork 뒤 자식에서는 async-signal-safe 함수만 쓴다.

## 테스트
| 범위 | ID |
| --- | --- |
| 현재(기능) | E-ST-01~12, E-CG-01~10, E-PX-01~17 (판단 부분) |
| 현재(계약 최소셋) | U-HD-02, 04, 09, 11, 14, 21, 26 · U-PX-01, 05, 07, 10 |
| 추가 계약 테스트 | U-HD-19 (조건부 304) |
| 추후 | U-HD-01~30, U-PX-01~15 중 나머지 |

## 구현 단계
3단계 STATIC·CGI: B `cgi` · C `plan`, `static`, `mime`. 4단계 PROXY: B `proxy`.
- [x] plan (+ handler_dispatch)
- [x] mime
- [x] static
- [x] cgi
- [ ] proxy

빈 응답 계획에서 본문 자원을 초기화하는 기준은 D-039, CGI 응답의 길이와 상태 검사 기준은 D-040을 따른다.
