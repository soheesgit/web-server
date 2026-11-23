# [GREEN] upstream 백엔드 연결·프록시 릴레이 구현

## 목적

PNG의 초록 영역인 백엔드 면을 구현한다. 설정된 백엔드 중 하나를 고르고, 연결을 획득해 요청을 전달하고, 응답을 파싱·릴레이한 뒤 안전하게 연결 풀에 반환하거나 폐기한다.

## 범위

- `src/upstream/`: backend registry/group/state, load balancing, connection pool
- 업스트림 요청 직렬화와 응답 헤더 파싱
- v1 blocking relay; 선택형 v2 상태 머신 relay
- `src/upstream/README.md` 구현 단계 갱신

클라이언트 연결 실행은 `downstream/`, proxy 판단과 response plan 구성은 `handler/`가 소유한다. 공개 인터페이스로 연결한다.

## 작업 항목

- [ ] 설정에서 백엔드 registry와 group을 만들고 상태를 보고한다.
- [ ] 동시 접근을 잠금으로 보호하며 라운드로빈·장애 제외 규칙을 구현한다.
- [ ] non-blocking connect 결과와 유휴 keep-alive 연결 획득·반납 규칙을 구현한다.
- [ ] origin-form 요청 라인, hop-by-hop 헤더 제거, Host 및 X-Forwarded-* 헤더를 직렬화한다.
- [ ] 응답 상태 줄·헤더와 본문 framing을 파싱한다.
- [ ] v1 relay의 연결·송신·응답 머리 수신·마감 처리를 구현한다.
- [ ] 프로젝트가 선택형 7단계 v2를 진행하기로 하면 relay의 상태 전이·fd 조회·deadline·결과 인계 및 실패 정리를 구현한다. 선택하지 않으면 미완료 범위를 README에 남긴다.
- [ ] 응답 본문이 완전히 소비된 경우에만 연결을 재사용하고, 잘린 본문이나 framing 오류 연결은 폐기한다.
- [x] 회색 파트의 proxy handler 및 보라 파트의 downstream과 통합 검증한다.

`E-PX-*` 책임 경계: GRAY는 proxy 판단·재시도 정책, GREEN은 backend pool·relay I/O,
PURPLE은 클라이언트 응답 송신과 연결 종료를 담당한다.

## 완료 조건

- [ ] upstream 계약 `U-US-*`가 통과한다.
- [ ] 백엔드 실패·지연·장애 제외·재시도·연결 재사용·요청/응답 framing 관련 `E-PX-*`가 통과한다.
- [ ] v1 relay가 정해진 I/O 규칙과 timeout 계약을 만족한다. v2를 범위에 넣은 경우에도 해당 계약을 확인한다.
- [ ] `make lint`, `make test`, Linux의 `make asan` 결과를 PR에 기록한다.
- [ ] 구현한 테스트 ID만 `tests/pending.txt`에서 제거한다.
- [ ] 다른 디렉토리 `.c`를 수정하지 않으며 인터페이스 변경은 결정/별도 PR 절차를 따른다.

## 대상 테스트 ID

- 계약: `U-US-01`, `09`, `12`, `17`, `24`
- 기능/통합: `E-PX-01~17` (handler 판단·downstream 송신과의 통합 포함)

## 의존성 및 참고

- 선행: RED `core/common`의 공용 HTTP framing과 설정 계약
- 통합 상대: GRAY `handler/proxy`, PURPLE `downstream/conn`
- 기준 규약: `src/upstream/AGENTS.md`, `src/upstream/README.md`, `docs/CONVENTIONS.md`, `docs/TESTING.md`, `docs/DECISIONS.md`
- 색상 진행 메모에서 네 번째 파트로 배치했다. 프록시 E2E는 앞선 gray 구현과 함께 확인한다.

