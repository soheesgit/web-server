# router/ AGENTS

- 대상 테스트: 계약 U-RT-02, 03 · 기능 E-PX-15 (그 밖에 모든 E-*가 라우팅을 거친다).
- include 가능한 다른 디렉토리 헤더: `core/config.h`, `core/str.h`, `common/types.h`.
- 함정:
  - prefix 경계: `/api`는 `/apix`에 맞으면 안 된다 (U-RT-03).
  - Host는 포트를 떼고 대소문자 무시로 비교한다. `[::1]:8080` 같은 IPv6 리터럴의 ':'에 주의.
  - upstream 이름을 그룹으로 바꾸는 일은 콜백으로만 한다 (router는 upstream을 include할 수 없다).
  - `route_t`의 문자열은 config_t를 빌린다. 복사하지 않아도 되지만 config_t보다 오래 살면 안 된다.
- 작업을 나눌 때는 파일 단위로.
