# [GRAY] router + handler + engine + app 구현 및 조립

## 목적

PNG의 회색 처리·라우팅·엔진 영역을 구현하고, 색상 영역 밖에서 서버를 조립하는 `app/`을 연결한다. 파싱한 요청을 경로 규칙에 따라 처리기로 보내고, 응답 계획을 만들며, 엔진에서 실제 서버를 시작·종료한다.

## 범위

- `src/router/`: 설정 규칙 구성, Host·prefix 매칭
- `src/handler/`: response plan, MIME, STATIC, CGI, PROXY 판단·헤더 변환
- `src/engine/`: v1 worker pool 및 선택형 v2 poller/event loop
- `src/app/`: 설정에서 upstream·router·listener·engine을 구성하고 실행·정리
- 각 디렉토리 README 구현 상태 갱신

`handler/proxy`는 백엔드 선택·재시도 판단과 응답 계획 변환까지 맡는다. 백엔드 registry·pool·relay I/O는 GREEN 이슈 범위다. 프록시 전체 통합 완료는 두 이슈가 모두 끝난 뒤 확인한다.

## 작업 항목

- [ ] 공개 헤더 계약을 바꾸지 않고 router와 처리기 스텁을 구현한다.
- [ ] STATIC의 메서드·경로 보안·파일·디렉터리·MIME 처리 흐름을 구현한다.
- [ ] CGI 환경·pipe·fork/exec·응답 헤더 검증·자원 회수 흐름을 구현한다.
- [ ] PROXY 판단 경로에서 그룹 선택, 재시도 조건, 응답 헤더 변환을 구현한다.
- [ ] 엔진 v1의 accept·bounded queue·worker 처리·graceful shutdown을 구현한다.
- [ ] 엔진 v2 poller/event loop는 선택형 7단계로 둔다. 진행하기로 하면 플랫폼별 backend와 콜백 내 블로킹 금지 규칙을 구현하고, 보류하면 미완료 범위를 문서에 남긴다.
- [ ] `app`에서 설정과 모듈을 조립하고 오류·종료 경로의 소유권을 정리한다.
- [ ] 테스트 ID와 README 완료 표시를 실제 결과와 맞춘다.

## 완료 조건

- [ ] `U-RT-*`, `U-HD-*`, `U-PX-*`, `U-EN-*` 중 대상 계약 테스트가 통과한다.
- [ ] STATIC·CGI·설정/실행 E2E 테스트가 통과한다.
- [ ] 엔진 v1 큐 포화, 동시 처리, graceful shutdown 및 자원 정리 경로를 테스트한다. v2를 선택한 경우 poller별 계약도 테스트한다.
- [x] upstream 담당 이슈 완료 후 `E-PX-*` 전체 프록시 왕복을 통합 확인한다.
- [ ] `E-PX-*` 수정 책임은 GRAY의 proxy 판단과 GREEN의 upstream relay로 나누며, PURPLE은 클라이언트 송수신 통합을 담당한다.
- [ ] `make lint`, `make test`, Linux의 `make asan` 결과를 PR에 기록한다.
- [ ] 구현한 테스트 ID만 `tests/pending.txt`에서 제거한다.
- [ ] 공개 인터페이스 변경은 구현 PR에서 분리해 결정 및 별도 승인 절차를 거친다.

## 대상 테스트 ID

- 계약: `U-RT-02`, `03`; `U-HD-02`, `04`, `09`, `11`, `14`, `21`, `26`; `U-PX-01`, `05`, `07`, `10`; `U-EN-01`, `02`, `05`
- 기능/통합: `E-ST-01~12`, `E-CG-01~10`, `E-CF-02~04`, 엔진으로 실행되는 E2E 전체, 프록시 판단 경로

## 의존성 및 참고

- 선행: [RED core + common](issue-1-red-core-common.md)
- GREEN과 협업: 프록시 왕복 완료에는 [GREEN upstream](issue-4-green-upstream.md)이 필요하다.
- 기준 규약: 각 `src/<dir>/AGENTS.md`와 `README.md`, `docs/CONVENTIONS.md`, `docs/TESTING.md`, `docs/DECISIONS.md`
- 요청에 따라 회색 파트는 `router`, `handler`, `engine`을 묶었다. `app`은 그림에서 색이 지정되지 않은 조립부여서 서버 실행을 끝내기 위해 함께 묶었다.

