# app/

## 책임
그림 밖의 조립 영역이다. 설정을 읽어 각 영역 타입으로 바꾸고(limits, engine kind), upstream 레지스트리·라우터·대기 소켓·엔진을 만들어 conn 콜백으로 연결한다. `main`은 인자 처리, 로그, 시그널, 실행, 종료 코드를 맡는다.

## 알아도 되는 것 / 몰라야 하는 것
| 알아도 되는 것 | 몰라야 하는 것 |
| --- | --- |
| 전부 | — |

## 파일
| 파일 | 역할 | 그림의 블록 | 공개 함수 |
| --- | --- | --- | --- |
| `app.h` `app.c` | 조립: config → upstream·router·listener → engine | (그림 밖) | `app_new`, `app_run`, `app_free` |
| `main.c` | 인자, 시그널, 실행, 종료. 동작과 종료 코드는 파일 머리 주석 | (그림 밖) | `main` |

## 의존
- include 가능: 전부 (`core`, `common`, `router`, `handler`, `upstream`, `engine`, `downstream`)
- 이 디렉토리를 쓰는 곳: 없음 (`main.c`는 `build/run_unit`에서 빠진다)

## 블로킹·스레드 규칙
모두 `[main-only]`. 시그널 핸들러는 `volatile sig_atomic_t` 플래그만 설정한다.

## 테스트
| 범위 | ID |
| --- | --- |
| 현재(기능) | E-CF-01~04, 그리고 서버를 띄우는 모든 E-* |
| 현재(계약 최소셋) | 없음 |
| 추후 | N-SIG-01~05, N-MEM-02 |

## 구현 단계
5단계 `engine` v1 + `app`. 분담: A `app`, `main`(시그널) · C E2E 스위트 연결.
- [x] main — `--help`와 스텁 종료 (initial commit, 7.3절 예외)
- [x] main — 전체 흐름
- [x] app
