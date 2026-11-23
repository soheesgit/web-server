# 담당 파일

이 이슈의 범위와 현재 구현 파일을 연결한 목록이다. 프로젝트의 include 경로와 모듈 규칙을 유지하기 위해 소스 파일은 `src/`의 기존 구조에 둔다.

## GREEN: upstream 연결과 릴레이

- backend registry/state: `src/upstream/backend.c`, `src/upstream/internal_state.h`
- 로드밸런싱·연결 풀: `src/upstream/lb.c`, `src/upstream/pool.c`
- 요청 직렬화·응답 파싱: `src/upstream/request_writer.c`, `src/upstream/response_parser.c`
- blocking 릴레이: `src/upstream/relay.c`, `src/upstream/relay.h`
- 모듈 진행 상태: `src/upstream/README.md`

v1 blocking과 v2 relay 상태 머신을 모두 반영했다. v2는 event loop·downstream과 연결되어 E2E를 통과한다.
Docker Linux 환경에서 v1 기준 빌드·린트·단위·필수 E2E를 실행했다. v2 relay,
ASan 최종 결과, macOS 빌드는 미확인이다.
