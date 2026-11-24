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
| 현재(계약 최소셋) | U-EN-01, 02(v1·v2), 03, 05, 06, 09 |
| 추후 | U-EN-01~12 중 나머지 |

## 구현 단계
5단계 `engine` v1: B `engine_pool`. 7단계(선택) v2: A `poller_*` · C `engine_loop`.
- [x] engine_pool (v1)
- [x] poller_epoll
- [x] poller_kqueue
- [x] poller_poll
- [x] engine_loop (v2)

v1과 v2를 모두 구현했다. v2는 `-e loop`(또는 설정 `engine loop`)로 고른다. 스레드 하나가 poller로 대기 소켓·클라이언트·CGI pipe·업스트림 fd를 함께 감시한다 (D-040). WSL Ubuntu에서 `make lint`, `make test`, `make asan`이 통과하고, 같은 E2E 68개를 `HTTPD_ENGINE=loop`로 돌려도 일반·ASan 빌드 모두 통과한다 (2026-11-23).

2026-11-24: epoll·kqueue poller가 상대의 반쪽 닫기(FIN)를 오류(4)로 알려, v2에서 요청을 보낸 뒤 쓰기 쪽만 닫은 클라이언트가 응답을 받지 못하던 문제를 고쳤다. 지금은 `poller.h` 계약("1 읽기 가능(또는 EOF)")대로 읽기(1)로만 알리고, 레벨 트리거인 `EPOLLRDHUP`은 읽기를 감시할 때만 등록한다. 회귀 테스트는 `tests/robust`의 R-10 (`make robust`).

## 측정 (v1 pool vs v2 loop)

재현: `make && python3 tests/bench/bench.py --idle 0,100,1000,5000 --conns 32,256,1000`

환경: WSL2 Ubuntu (커널 6.18.33.2-microsoft-standard-WSL2), AMD Ryzen 7 250 (16 스레드), 루프백. 클라이언트(파이썬 4 프로세스)도 같은 기계. 응답은 `index.html` 48바이트. 서버 설정은 기본값(워커 32 = 16×2, 큐 1024, 헤더 마감 10초, keep-alive 5초, 연결당 요청 100). 측정일 2026-11-23, 각 칸 1회 측정.

**실험 1 — 아무것도 보내지 않는 연결 K개가 붙어 있을 때 새 GET 5번의 응답 시간**

| 유휴 K | pool 중앙값 / 최대 (ms) | pool 상태 | loop 중앙값 / 최대 (ms) | loop 상태 |
|---|---|---|---|---|
| 0 | 0.3 / 1.5 | 200 ×5 | 0.2 / 0.7 | 200 ×5 |
| 100 | 0.4 / 9519.1 | 200 ×5 | 0.2 / 0.5 | 200 ×5 |
| 1000 | 0.5 / 9498.6 | 200 ×5 | 0.2 / 0.4 | 200 ×5 |
| 5000 | 0.2 / 0.3 | 503 ×5 | 0.2 / 0.4 | 200 ×5 |

pool은 유휴 연결이 워커 32개를 붙잡아, 새 요청이 헤더 마감(10초)이 지날 때까지 큐에서 기다린다(첫 요청 약 9.5초). 워커+큐(32+1024)를 넘으면 503이다. loop는 유휴 연결 수와 상관없이 1ms 안에 응답한다.

**실험 2 — 동시 연결 C개가 keep-alive로 GET을 반복 (5초, 200 응답만 셈)**

| 동시 C | pool 200/초 | pool p50 / p99 (ms) | loop 200/초 | loop p50 / p99 (ms) |
|---|---|---|---|---|
| 32 | 94,645 | 0.28 / 0.65 | 19,173 | 1.56 / 3.67 |
| 256 | 44,147 | 0.66 / 62.66 | 17,554 | 13.92 / 18.32 |
| 1000 | 73,526 | 0.36 / 923.28 | 21,462 | 52.23 / 102.06 |

읽는 법:
- 처리량은 pool이 높다. pool은 16코어를 쓰고 loop는 코어 하나만 쓴다.
- 연결이 워커 수보다 많으면 pool은 나머지 연결을 큐에 세워 둔다. 그래서 꼬리 지연(p99)이 커진다(1000개에서 923ms). loop는 모든 연결을 번갈아 처리해 p99가 102ms다.
- 동시 연결이 수천 개면 이 클라이언트(블로킹 재연결)가 먼저 병목이 되어 표에 넣지 않았다.

**TCP_NODELAY 전후 (같은 실험 2, C=32, pool)**: 744/초, p50 44.00ms → 94,645/초, p50 0.28ms. 응답 머리와 본문(sendfile)을 따로 보내서 Nagle과 클라이언트의 지연 ACK(약 40ms)가 겹치던 문제였다 (`conn_new`에서 TCP_NODELAY).
