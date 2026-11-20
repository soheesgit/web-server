# handler/ AGENTS

- 대상 테스트: 계약 U-HD-02, 04, 09, 11, 14, 21, 26 · U-PX-01, 05, 07, 10 · 기능 E-ST-*, E-CG-*, E-PX-*(판단).
- include 가능한 다른 디렉토리 헤더: `core/*`, `common/*`, `router/router.h`, `upstream/upstream.h`, `upstream/request_writer.h`.
- 함정:
  - traversal·심볼릭 링크 탈출: 경로 정규화만 믿지 말고 realpath 결과가 루트 안인지 '/' 경계로 확인한다 (`/www`와 `/www2` 구분).
  - FIFO를 그냥 open하면 막힌다. O_NONBLOCK으로 열고 fstat으로 일반 파일인지 본 뒤 끈다.
  - 처리기가 연 fd는 모두 cloexec여야 CGI 자식에 새지 않는다.
  - CGI 환경 변수에 `HTTP_PROXY`를 만들지 않는다 (httpoxy). 서버 환경을 물려주지 않는다.
  - `plan_cleanup`은 자식을 반드시 회수한다(좀비). 업스트림 연결은 정상 중계가 아니면 release(false).
- `handler_dispatch`는 `plan.c`에 둔다 (트리에 dispatch 전용 파일이 없음).
- 작업을 나눌 때는 파일 단위로.
