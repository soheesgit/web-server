# AGENTS.md — 공통 작업 규칙

이 저장소에서 일하는 모든 사람과 에이전트가 처음 읽는 파일이다. 명세는 여기에 다시 쓰지 않고 위치만 가리킨다.

## 1. 읽는 순서

1. [docs/CONVENTIONS.md](docs/CONVENTIONS.md) — 전역 규약
2. 작업할 `src/<dir>/README.md` — 디렉토리 책임, 파일, 대상 테스트 ID
3. 그 디렉토리의 `AGENTS.md` — 디렉토리 고유의 함정
4. 헤더 주석 — 함수·타입 명세의 유일한 출처
5. 대상 테스트 코드 — `tests/unit/contract_min/test_<dir>.c`, `tests/e2e/test_*.py`

흐름 전체가 필요하면 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), 테스트 규칙은 [docs/TESTING.md](docs/TESTING.md).

## 2. 작업 단위는 테스트 ID

목표는 "이 ID들을 통과시킨다"이다. 통과시킨 ID는 같은 PR에서 `tests/pending.txt`에서 지운다. 지우지 않으면 `UNEXPECTED PASS`로 CI가 실패한다.

## 3. 금지

- 공개 헤더의 시그니처·구조체 필드·enum 변경 (필요하면 CONVENTIONS.md 10절 절차로 따로 PR)
- 테스트 기대값 완화, `tests/pending.txt`에 ID 추가
- 다른 디렉토리의 `.c` 수정
- 허용되지 않은 방향의 include (CONVENTIONS.md 7절), 다른 디렉토리의 `internal_*.h` include
- 외부 라이브러리, 금지 함수 (CONVENTIONS.md 8절)
- `--no-verify`, force push

## 4. 허용

- 자기 디렉토리 안의 `static` 함수와 내부 헤더 `src/<dir>/internal_<이름>.h`
- 추가 단위 테스트 (기획서 7~11장의 ID를 그대로 사용, 한 함수 = 한 ID)

## 5. PR 전 확인

- `make lint && make test` 통과
- `make asan` 통과 (Linux. macOS는 CI에 맡김)
- 헤더 주석과 구현이 일치 (구현하다 명세를 바꿔야 하면 6번)
- 작업한 `src/<dir>/README.md`의 "구현 단계" 체크 갱신
- PR 템플릿(`.github/pull_request_template.md`)의 체크박스

## 6. 명세가 모호할 때

추측해서 구현하지 않는다. [docs/DECISIONS.md](docs/DECISIONS.md)에 "제안" 상태로 항목을 추가하고(형식은 파일 머리), PR에서 사람에게 묻는다. 기다리는 동안은 더 엄격하게 거부하는 쪽, 기능이 적은 쪽으로 구현한다.

## 7. 커밋 메시지

Conventional Commits, scope는 디렉토리 이름. 본문에 통과시킨 테스트 ID를 적는다.

```
feat(downstream): parse request line and headers

통과: U-DS-01, U-DS-06, U-DS-07, E-ER-01, E-ER-02
```
