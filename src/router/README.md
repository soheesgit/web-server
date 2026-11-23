# router/

## 책임
정규화된 경로와 Host로 요청에 맞는 `route_t`(STATIC·CGI·PROXY)를 고른다. 설정의 route 규칙을 실행용 표로 바꾸고, PROXY 규칙의 upstream 이름은 콜백으로 그룹 포인터로 바꾼다. 아키텍처 그림의 "라우팅" 블록이다.

## 알아도 되는 것 / 몰라야 하는 것
| 알아도 되는 것 | 몰라야 하는 것 |
| --- | --- |
| 경로·Host 규칙 | 소켓, 파일 |

## 파일
| 파일 | 역할 | 그림의 블록 | 공개 함수 |
| --- | --- | --- | --- |
| `router.h` `router.c` | 규칙 표 생성, prefix·Host 매칭 | 라우팅 | `router_new`, `router_free`, `router_match` |

## 의존
- include 가능: `core`, `common`
- 이 디렉토리를 쓰는 곳: `handler`(route_t 타입), `downstream`, `app`

## 블로킹·스레드 규칙
I/O 금지. `router_new`·`router_free`는 `[main-only]`, `router_match`는 `[thread-safe]`(생성 후 읽기 전용).

## 테스트
| 범위 | ID |
| --- | --- |
| 현재(기능) | E-PX-15 (Host 라우팅), E-ST-01·E-CG-01·E-PX-01 (prefix 라우팅) |
| 현재(계약 최소셋) | U-RT-02, 03 |
| 추후 | U-RT-01, U-RT-04~08 |

## 구현 단계
3단계 (`router` + `handler` STATIC·CGI). 분담: A `router`.
- [x] router
