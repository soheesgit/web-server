# upstream/

## 책임
백엔드 쪽 면이다: 백엔드 레지스트리·그룹·상태(연속 실패, 제외), 라운드로빈 선택, 연결 획득·반납(유휴 연결 풀), 업스트림 요청 직렬화(origin-form, hop-by-hop 제거, X-Forwarded-* 추가), 응답 머리 파싱, 그리고 프록시의 기다림이 있는 I/O(연결 획득, 요청 송신, 응답 머리 수신) 실행. 재시도 판단과 응답 본문 중계는 하지 않는다.

## 알아도 되는 것 / 몰라야 하는 것
| 알아도 되는 것 | 몰라야 하는 것 |
| --- | --- |
| 백엔드 소켓 | 클라이언트 소켓, 라우팅 규칙 |

## 파일
| 파일 | 역할 | 그림의 블록 | 공개 함수 |
| --- | --- | --- | --- |
| `upstream.h` | 레지스트리·그룹·백엔드·연결 API | — | (아래 세 파일의 함수) |
| `backend.c` | 레지스트리·그룹·백엔드 상태 | 업스트림 상태 | `upstream_registry_new`, `upstream_registry_free`, `upstream_registry_get`, `upstream_lookup_cb`, `upstream_backend_count`, `upstream_report`, `backend_hostport` |
| `lb.c` | 백엔드 선택 | LB | `upstream_pick` |
| `pool.c` | 연결 획득·반납 | 연결 풀, 업스트림 소켓 | `upstream_acquire`, `upstream_connect_result`, `upstream_release` |
| `request_writer.h` `request_writer.c` | 업스트림 요청 직렬화 | 요청 직렬화 4단계 | `request_write_upstream` |
| `response_parser.h` `response_parser.c` | 상태 줄 + 응답 헤더 | 응답 파싱 | `response_parse_head` |
| `relay.h` `relay.c` | 프록시 I/O 실행 (v1 blocking, v2 상태 머신) | PROXY I/O 단계 | `relay_exchange_blocking`, `relay_start`, `relay_step`, `relay_fd`, `relay_deadline_ms`, `relay_take`, `relay_free` |

## 의존
- include 가능: `core`, `common`
- 이 디렉토리를 쓰는 곳: `handler`, `downstream`, `app`

## 블로킹·스레드 규칙
- `request_writer`, `response_parser`: I/O 금지, `[thread-safe]`.
- `backend`, `lb`, `pool`: non-blocking `connect` 시작까지만. 공유 상태(카운터, 실패 횟수, 유휴 목록)는 mutex로 보호해 `[thread-safe]`.
- `relay`: 소켓 I/O 실행. v1은 poll로 마감을 지키는 blocking, v2는 non-blocking.
- `upstream_registry_new`의 getaddrinfo는 시작 시 1회 (`[main-only]`).

## 테스트
| 범위 | ID |
| --- | --- |
| 현재(기능) | E-PX-01~17 |
| 현재(계약 최소셋) | U-US-01, 09, 12, 17, 24 |
| 추후 | U-US-01~36 중 나머지 |

## 구현 단계
4단계 (`upstream` + `handler` PROXY). 분담: A `request_writer`, `response_parser` · B `pool`, `lb` · C `backend`, `relay`. 7단계(선택) C `relay` 이벤트 경로.
- [ ] request_writer
- [x] response_parser
- [ ] backend
- [ ] lb
- [ ] pool
- [ ] relay — blocking (v1)
- [ ] relay — 상태 머신 (v2)
