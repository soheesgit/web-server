# [PURPLE] downstream 클라이언트 연결 구현

## 목적

PNG의 보라 영역인 클라이언트 면을 완성한다. 연결을 받고 요청 바이트를 파싱해 처리 계층에 전달하며, 응답 계획을 클라이언트에 보내고 연결을 유지하거나 정리한다.

## 범위

- `src/downstream/`: listener, conn, request parser, URI 정규화, response writer
- v1 blocking 연결 처리와 v2 event 기반 연결 처리
- downstream이 보유한 파일·pipe·upstream 응답 본문 실행 경로
- `src/downstream/README.md` 구현 단계 갱신

라우터·처리기의 응답 결정과 업스트림 풀·릴레이 자체 구현은 각 담당 영역 이슈에서 맡는다. 이 이슈에서는 공개 인터페이스를 통해 연결하고, 누락된 통합 경로를 검증한다.

## 작업 항목

- [ ] README에서 완료로 표시된 parser·URI·response writer·listener·v1 blocking 경로는 계약 테스트로 확인하고, 누락된 부분만 고친다.
- [ ] 나뉘어 도착하는 요청, 한 번에 이어 도착하는 다음 요청, 파이프라이닝 버퍼 보존을 처리한다.
- [ ] 헤더·본문 절대 마감, 부분 읽기·쓰기, 오류 후 연결 종료, keep-alive를 처리한다.
- [ ] 요청 파서의 메서드·버전·Host·본문 framing 검증과 URI 디코딩/정규화를 구현한다.
- [ ] 응답 상태 줄·헤더·chunked 쓰기 규칙을 구현한다.
- [ ] 프로젝트가 선택형 7단계 v2를 진행하기로 하면 CGI pipe와 proxy 보조 fd의 이벤트 진행, 타임아웃·취소·정리 전이를 연결한다. v2를 선택하지 않으면 미완료 상태를 문서에 남긴다.
- [ ] 각 상태 전이와 예외 경로를 실제 테스트로 검증하고 README 상태를 갱신한다.

## 완료 조건

- [ ] downstream 계약 테스트 `U-DS-*`가 통과한다.
- [ ] 오류, keep-alive, 느린/끊기는 클라이언트, STATIC·CGI·PROXY 송수신 E2E 테스트가 통과한다.
- [ ] v2를 범위에 포함한 경우, 경로가 미지원 응답으로 종료하지 않고 블로킹 호출 없이 등록된 fd 이벤트와 마감으로 진행한다.
- [ ] `make lint`, `make test`, Linux의 `make asan` 결과를 PR에 기록한다.
- [ ] 구현한 테스트 ID만 `tests/pending.txt`에서 제거한다.
- [ ] 다른 디렉토리의 `.c` 파일은 수정하지 않는다. 경계 변경이 필요하면 결정·인터페이스 절차로 분리한다.

## 대상 테스트 ID

- 계약: `U-DS-01`, `06`, `07`, `12`, `22`, `27`, `28`, `30`, `36`, `40`, `44`
- 기능/통합: `E-ER-01~14`, `E-KA-01~07`, `E-SL-01~04`, `E-ST-*`, `E-CG-*`, `E-PX-*` 중 downstream 송수신 경로

## 의존성 및 참고

- 선행: [RED core + common](issue-1-red-core-common.md), 처리기·라우터 인터페이스와 엔진 콜백 계약
- 기준 규약: `src/downstream/AGENTS.md`, `src/downstream/README.md`, `src/HANDOFF.md`, `docs/CONVENTIONS.md`, `docs/TESTING.md`
- `src/HANDOFF.md`는 v2 CGI와 proxy event 연동을 미완료로 기록한다. 이를 완료 조건에 포함한다.

