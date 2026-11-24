# engine/

## 책임
동시성 엔진이다. v1은 메인 스레드가 accept하고 워커 스레드 풀이 연결 하나씩을 blocking으로 처리한다. v2(선택)는 단일 이벤트 루프가 poller(epoll·kqueue·poll)로 모든 fd를 감시하고 마감을 관리한다. HTTP를 모르며 연결 객체는 `void *`와 콜백으로만 다룬다. 아키텍처 그림의 "동시성 엔진"과 poller 블록이다.

## 알아도 되는 것 / 몰라야 하는 것
| 알아도 되는 것 | 몰라야 하는 것 |
| --- | --- |
| fd와 콜백 | HTTP, 라우팅, 처리기 |

## 파일
| 파일 | 역할 | 그림의 블록 | 공개 함수 |
| --- | --- | --- | --- |
| `engine.h` | 엔진 공통 API, 콜백, 이벤트 비트 | 동시성 엔진 | `engine_new`, `engine_run`, `engine_watch`, `engine_unwatch`, `engine_free` |
| `engine_pool.c` | v1 스레드 풀 + 작업 큐, 그리고 kind에 따라 v2 구현으로 넘기는 공통 진입점 (v2 내부 함수는 `internal_loop.h`로 공유) | 스레드 풀 | `engine_new`, `engine_run`, `engine_free` |
| `engine_loop.c` | v2 이벤트 루프와 추가 fd 등록 | 이벤트 루프 | `engine_watch`, `engine_unwatch` |
| `poller.h` | poller API | poller | `poller_new`, `poller_add`, `poller_mod`, `poller_del`, `poller_wait`, `poller_free`, `poller_backend_name` |
| `poller_epoll.c` | Linux epoll | poller | (poller.h) |
| `poller_kqueue.c` | macOS·FreeBSD kqueue | poller | (poller.h) |
| `poller_poll.c` | 그 밖의 OS 또는 `-DHTTPD_FORCE_POLL` | poller | (poller.h) |

## 의존
- include 가능: `core`
- 이 디렉토리를 쓰는 곳: `downstream`(engine_watch), `app`

## 블로킹·스레드 규칙
- `engine_new`·`engine_run`·`engine_free`는 `[main-only]`.
- v1 워커 안의 `serve_blocking`은 블로킹해도 된다(연결 하나 = 워커 하나).
- v2 콜백 안에서는 블로킹 금지. 모든 fd는 non-blocking.
- poller 세 파일 중 정확히 하나만 심볼을 정의한다 (D-022). OS 분기는 poller 파일에만.

## 테스트
| 범위 | ID |
| --- | --- |
| 현재(기능) | E-* 전체 (v1 엔진으로), E-SL-01~02 (동시 처리) |
| 현재(계약 최소셋) | U-EN-01, 02, 05 |
| 추가 계약 | U-EN-03 (작업 큐 포화 시 on_accept 없이 on_busy 호출) |
| 추후 | U-EN-01~12 중 나머지 |

## 구현 단계
5단계 `engine` v1: B `engine_pool`. 7단계(선택) v2: A `poller_*` · C `engine_loop`.
- [x] engine_pool (v1)
- [ ] poller_epoll
- [ ] poller_kqueue
- [ ] poller_poll
- [ ] engine_loop (v2)

Stage 5의 종료·과부하 검증과 워커 수·큐 크기 비교는 [스레드 풀 검증 및 측정](../../docs/benchmarks/stage5-workers.md)을 참고한다.
