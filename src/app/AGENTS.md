# app/ AGENTS

- 대상 테스트: 기능 E-CF-01~04 (E-CF-01은 initial commit에서 이미 통과, D-007), 서버를 띄우는 모든 E-*.
- include 가능한 다른 디렉토리 헤더: 전부.
- 함정:
  - 시그널 핸들러에서는 플래그만 쓴다 (printf, malloc, log_msg 금지).
  - `app_new`가 중간에 실패하면 만든 것을 역순으로 정리한다. 포트 사용 중 메시지는 E-CF-03이 `Address already in use`를 확인한다.
  - 종료 코드: 0 정상, 1 설정 오류(`--help`는 0), 2 기동 실패. E2E 하네스가 기동 실패를 이 코드로 구분한다.
  - SIGPIPE 무시는 log_open 뒤, 스레드를 만들기 전에.
- 작업을 나눌 때는 파일 단위로.
