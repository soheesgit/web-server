# 담당 파일

이 이슈의 범위와 현재 구현 파일을 연결한 목록이다. 프로젝트의 include 경로와 모듈 규칙을 유지하기 위해 소스 파일은 `src/`의 기존 구조에 둔다.

## RED: core + common

- `src/core/str.c`, `src/core/str.h`
- `src/core/buf.c`, `src/core/buf.h`
- `src/core/config.c`, `src/core/config.h`
- `src/core/log.c`, `src/core/log.h`
- `src/core/platform.c`, `src/core/platform.h`
- 공통 HTTP 구현: `src/common/` (기존 구현을 참조)
- 모듈 진행 상태: `src/core/README.md`, `src/common/README.md`

구현을 반영했다. Docker Linux 환경에서 빌드·린트·단위·필수 E2E를 실행했다.
ASan 최종 결과와 macOS 빌드는 미확인이다.
