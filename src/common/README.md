# common/

## 책임
영역 사이를 오가는 경계 타입(`http_request_t`, `http_response_t`, `response_plan_t`, `proxy_job_t` 등)과, 요청(downstream)과 응답(upstream) 양쪽 면이 같은 RFC 7230 규칙으로 쓰는 블록(헤더 끝 탐색, 헤더 파싱, 본문 길이 판정, 본문 경계 추적, 헤더 쓰기, keep-alive 판단, 상태 문구)을 한 번만 구현한다.

## 알아도 되는 것 / 몰라야 하는 것
| 알아도 되는 것 | 몰라야 하는 것 |
| --- | --- |
| 바이트와 타입 | 모든 I/O |

## 파일
| 파일 | 역할 | 그림의 블록 | 공개 함수 |
| --- | --- | --- | --- |
| `types.h` | 경계 타입과 수명 규칙 (.c 없음) | 경계 타입 | (타입만) |
| `http_headers.h` `http_headers.c` | 헤더 끝 탐색, 헤더 파싱·조회, token 검사, hop-by-hop, 헤더 쓰기 | 헤더 끝 탐색, 헤더 파싱, 헤더 쓰기 | `http_find_head_end`, `http_parse_headers`, `http_header_get`, `http_header_count`, `http_token_valid`, `http_is_hop_by_hop`, `http_headers_write` |
| `http_framing.h` `http_framing.c` | 본문 길이 판정, 본문 경계 추적, keep-alive | 본문 길이, 본문 경계 추적, keep-alive 판단 | `http_resolve_framing`, `http_body_tracker_init`, `http_body_track`, `http_keep_alive` |
| `http_status.h` `http_status.c` | 사유 문구, 에러 페이지 | 상태 문구 | `http_reason`, `http_error_body` |

## 의존
- include 가능: `core`
- 이 디렉토리를 쓰는 곳: `router`, `upstream`, `handler`, `downstream`, `app`

## 블로킹·스레드 규칙
I/O 금지. 모든 함수는 순수 함수이며 `[thread-safe]`(출력 버퍼 기준 `[per-object]`).
이 디렉토리를 바꾸는 PR은 세 명 모두 승인한다 (docs/CONVENTIONS.md 10절).

## 테스트
| 범위 | ID |
| --- | --- |
| 현재(기능) | E-ER-06, E-ER-08, E-ER-13, E-ER-14, E-KA-03~05, E-PX-03, E-PX-05 |
| 현재(계약 최소셋) | U-COM-01, 02, 04, 07, 10, 18, 21, 26, 29, 32, 38, 40 |
| 추후 | U-COM-01~45 중 나머지 |

## 구현 단계
1단계 (`core` + `common`). 분담: A `http_headers` · B `http_framing`(본문 길이) · C `http_framing`(경계 추적, keep-alive), `http_status`.
- [ ] http_headers
- [ ] http_framing — 본문 길이
- [ ] http_framing — 경계 추적, keep-alive
- [ ] http_status
