# core/

## 책임
다른 모든 영역이 쓰는 기반이다: 빌린 문자열 뷰, 상한 있는 버퍼, 접근·에러 로그, CLI와 설정 파일 읽기, OS 차이(fd 플래그, SIGPIPE 없는 송신, sendfile, 시계). 아키텍처 그림 밖의 바닥 블록이다.

## 알아도 되는 것 / 몰라야 하는 것
| 알아도 되는 것 | 몰라야 하는 것 |
| --- | --- |
| OS API | 다른 영역 전부 (HTTP, 라우팅, 처리기, 소켓의 의미) |

## 파일
| 파일 | 역할 | 그림의 블록 | 공개 함수 |
| --- | --- | --- | --- |
| `str.h` `str.c` | 문자열 뷰 비교·트림·숫자 변환 | 기반 | `str_from_cstr`, `str_eq`, `str_eq_ci`, `str_eq_ci_str`, `str_trim_ows`, `str_to_u64`, `str_to_cstr` |
| `buf.h` `buf.c` | 가변 버퍼, fd 읽기·쓰기 | 기반 | `buf_init`, `buf_cleanup`, `buf_append`, `buf_appendf`, `buf_consume`, `buf_shrink`, `buf_reset`, `buf_read_fd`, `buf_write_fd` |
| `log.h` `log.c` | 접근·에러 로그 (스레드 안전) | 기반 | `log_open`, `log_close`, `log_access`, `log_msg` |
| `config.h` `config.c` | CLI + 설정 파일 → `config_t` (문법은 config.h 머리 주석) | 기반 | `config_defaults`, `config_load`, `config_print_usage` |
| `platform.h` `platform.c` | nonblock, cloexec, SIGPIPE, sendfile, 시계, rlimit | 기반 | `platform_set_nonblock`, `platform_set_cloexec`, `platform_ignore_sigpipe`, `platform_send`, `platform_sendfile`, `platform_now_ms`, `platform_wall_time`, `platform_raise_nofile` |
| `stub.h` | 미구현 몸체 매크로 (구현 후 include 제거) | — | `STUB_RETURN`, `STUB_VOID`, `UNUSED` (매크로) |

## 의존
- include 가능: 없음 (표준 C·POSIX 헤더만)
- 이 디렉토리를 쓰는 곳: 모든 디렉토리

## 블로킹·스레드 규칙
- `str`은 I/O 금지. `buf_read_fd`·`buf_write_fd`·`platform_send`·`platform_sendfile`은 fd 한 번 호출만(fd가 blocking이면 그 fd의 타임아웃까지).
- `log_access`·`log_msg`는 `[thread-safe]`(한 줄 = write 한 번). `log_open`·`log_close`·`config_*`는 `[main-only]`.
- OS 분기(`#ifdef __linux__` 등)는 `platform.c`에만 둔다.

## 테스트
| 범위 | ID |
| --- | --- |
| 현재(기능) | E-CF-01, E-CF-02, E-CF-04 (config), 그 밖의 E-* 전체가 간접 사용 |
| 현재(계약 최소셋) | U-CORE-04, 05, 08, 10, 11, 13, 17, 21, 28, 29, 30 |
| 추후 | U-CORE-01~33 중 나머지 |

## 구현 단계
1단계 (`core` + `common`). 분담: A `str`, `buf` · B `config`, `log` · C `platform`.
- [x] str
- [x] buf
- [x] config
- [x] log
- [x] platform

소스 구현은 반영했다. WSL Ubuntu에서 `make`, `make lint`, `make test`를 실행해 통과했다 (2026-11-23). `make asan`도 통과했다 (131/131, 2026-11-24).
