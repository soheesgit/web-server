# 담당 파일

이 이슈의 범위와 현재 구현 파일을 연결한 목록이다. 프로젝트의 include 경로와 모듈 규칙을 유지하기 위해 소스 파일은 `src/`의 기존 구조에 둔다.

## PURPLE: downstream 클라이언트 연결

- 연결 수신: `src/downstream/listener.c`, `src/downstream/listener.h`
- 연결 상태와 요청 처리: `src/downstream/conn.c`, `src/downstream/conn.h`
- 요청 해석·URI: `src/downstream/request_parser.c`, `src/downstream/uri.c` 및 대응 헤더
- 응답 직렬화: `src/downstream/response_writer.c`, `src/downstream/response_writer.h`
- 모듈 진행 상태: `src/downstream/README.md`

기존 소스를 이슈 폴더로 옮기지 않고 해당 모듈 경로에서 작업한다. Docker Linux 환경에서
빌드·린트·단위·필수 E2E를 실행했다. ASan 최종 결과와 macOS 빌드는 미확인이다.
