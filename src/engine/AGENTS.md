# engine/ AGENTS

- 대상 테스트: 계약 U-EN-01, 02, 05 · 기능 E-* 전체(v1), E-SL-01, 02.
- include 가능한 다른 디렉토리 헤더: `core/*` (downstream을 include하지 않는다 — accept는 직접 부른다).
- 함정:
  - v2 콜백 안에서 블로킹 금지. 콜백 하나가 막히면 모든 연결이 멈춘다.
  - `<sys/event.h>`의 `EV_ERROR` 매크로가 engine.h의 `EV_ERROR`와 충돌한다. `poller_kqueue.c`는 engine.h를 include하지 않는다 (D-021).
  - stop 플래그는 시그널 핸들러가 쓴다. accept·poll 대기가 플래그를 놓치지 않게 짧은 타임아웃으로 깨어난다.
  - 종료 시 워커가 blocking read에 걸려 있으면 join이 끝나지 않는다. 유예 후 fd를 shutdown해 깨운다.
  - 큐가 가득일 때는 on_accept 전에 판단해 on_busy(503)로 넘긴다 (D-005).
  - `EPOLLRDHUP`·읽기 필터의 `EV_EOF`는 상대의 반쪽 닫기다. 오류(EV_ERROR)로 알리면 conn이 받은 요청을 읽기 전에 닫는다. 읽기(EV_READ)로만 알리고, `EPOLLRDHUP`은 레벨 트리거라 읽기를 감시할 때만 건다 (tests/robust R-10).
- 작업을 나눌 때는 파일 단위로.
