# downstream/

## 책임
클라이언트 쪽 면 전체다: 대기 소켓과 연결 소켓, 연결 상태(`conn_t`), 요청 파싱(요청 라인, URI 정규화), 응답 직렬화(상태 줄, 헤더 조립), 본문 송신, 연결 유지 판정. 라우터·처리기를 불러 계획을 받고, 그 계획을 실행한다. 프록시면 upstream relay를 엮는다.

## 알아도 되는 것 / 몰라야 하는 것
| 알아도 되는 것 | 몰라야 하는 것 |
| --- | --- |
| 클라이언트 소켓, 라우터·처리기 호출 | 응답 내용을 어떻게 만드는지 |

## 파일
| 파일 | 역할 | 그림의 블록 | 공개 함수 |
| --- | --- | --- | --- |
| `listener.h` `listener.c` | 대기 소켓 | 대기 소켓 | `listener_open`, `listener_accept`, `listener_port`, `listener_close` |
| `conn.h` `conn.c` | 연결 상태 머신 (읽기, dispatch, 프록시 루프, 본문 송신, 연결 판정). 처리 순서는 conn.h 머리 주석 | 연결 소켓, 연결 상태, 본문 송신, 연결 판정 | `conn_new`, `conn_free`, `conn_serve_blocking`, `conn_on_event`, `conn_on_timeout`, `conn_fd`, `conn_deadline_ms` |
| `request_parser.h` `request_parser.c` | 요청 라인 + 요청 헤더 묶음 | 요청 파싱 | `request_parse_head` |
| `uri.h` `uri.c` | 대상 분리, absolute-form, 퍼센트 디코딩, 경로 정규화 | URI 정규화 | `uri_split`, `uri_parse_absolute`, `uri_decode_path`, `path_normalize` |
| `response_writer.h` `response_writer.c` | 상태 줄 + 헤더 조립, chunked 프레이밍 | 응답 직렬화 | `response_write_head`, `response_write_chunk`, `response_write_last_chunk` |

## 의존
- include 가능: `core`, `common`, `router`, `handler`, `upstream`, `engine`
- 이 디렉토리를 쓰는 곳: `app`

## 블로킹·스레드 규칙
- `request_parser`, `uri`, `response_writer`: I/O 금지, `[thread-safe]`.
- `conn`: 소켓·파일·pipe I/O 실행. v1은 타임아웃 있는 blocking(모든 대기는 cfg의 마감으로 상한), v2는 non-blocking. `conn_t`는 `[per-object]`.
- `listener`: `listener_open`·`listener_close`는 `[main-only]`.

## 테스트
| 범위 | ID |
| --- | --- |
| 현재(기능) | E-ER-01~14, E-KA-01~07, E-SL-01~04, 그리고 E-ST·E-CG·E-PX의 전송 부분 |
| 현재(계약 최소셋) | U-DS-01, 06, 07, 12, 16, 22, 27, 28, 30, 36, 40, 44 |
| 추후 | U-DS-01~54 중 나머지 |

## 구현 단계
2단계. 분담: A `request_parser` · B `uri`, `response_writer` · C `listener`, `conn`(blocking 경로). 7단계(선택) B `conn` 이벤트 경로.
- [x] request_parser
- [x] uri
- [x] response_writer
- [x] listener
- [x] conn — blocking 경로 (v1)
- [x] conn — 이벤트 경로 (v2): CGI pipe·업스트림 fd를 engine_watch로 기다리는 단계 상태 머신
