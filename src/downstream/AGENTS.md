# downstream/ AGENTS

- 대상 테스트: 계약 U-DS-01, 06, 07, 12, 22, 27, 28, 30, 36, 40, 44 · 기능 E-ER-*, E-KA-*, E-SL-* (E-ST·E-CG·E-PX 전송 포함).
- include 가능한 다른 디렉토리 헤더: `core/*`, `common/*`, `router/router.h`, `handler/handler.h`, `handler/plan.h`, `upstream/upstream.h`, `upstream/relay.h`, `engine/engine.h`.
- 함정:
  - TCP는 바이트 스트림이다. 헤더가 여러 read에 나뉘어 오고, 한 read에 요청 두 개가 올 수도 있다.
  - 파이프라이닝으로 버퍼에 다음 요청이 남는다. `head_len + 본문`만 consume하고, 남은 바이트가 있으면 읽기를 기다리지 말고 바로 파싱한다.
  - `str_t`는 수신 버퍼를 빌린다. 요청 처리 중 수신 버퍼를 재할당(append)하면 `req`가 깨진다.
  - 에러 응답 직후 close하면 남은 입력 때문에 RST가 나가 응답이 유실된다. shutdown(SHUT_WR) 후 잠깐 drain.
  - 헤더 마감은 절대 시각이다(slowloris, E-SL-01). 바이트가 올 때마다 연장하지 않는다.
- 다른 디렉토리 `.c`는 고치지 않는다. fake 처리기를 끼울 수단은 없다 — 계약 테스트는 실제 STATIC 처리기를 쓴다 (D-018).
- 작업을 나눌 때는 파일 단위로.
