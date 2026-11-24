# 테스트

테스트의 입력과 기대값은 테스트 코드가 유일한 출처다. 이 문서는 층, ID, pending 규칙, 러너 사용법, fake·fixture, 추가 방법만 다룬다.

## 1. 테스트 층

| 층 | 범위 | ID | 위치 | 실행 |
| --- | --- | --- | --- | --- |
| 기능 (E2E) | **현재** | `E-<분류>-NN` (ST·CG·PX·KA·ER·SL·CF), 68개 | `tests/e2e/test_*.py` | `make e2e` |
| 계약 최소셋 | **현재** | `U-<영역>-NN` 중 56개 | `tests/unit/contract_min/test_<영역>.c` | `make unit` |
| 추가 계약 | **현재** | Stage 1의 `U-CORE-*` 8개와 `U-COM-*` 5개, Stage 3의 `U-HD-19`, Stage 5의 `U-EN-03` | `tests/unit/contract_min/` | `make unit` |
| 단위 (나머지) | 추후 | `U-<영역>-NN` | `tests/unit/` | `make unit` |
| 경계 | 추후 | `C-NN` | (추후 `tests/contract/`) | — |
| 비기능 | 추후 | `N-<분류>-NN` | (추후 `tests/nonfunc/`) | — |

영역 약어: `CORE`, `COM`(common), `DS`(downstream), `RT`(router), `HD`(handler: plan·static·mime·cgi), `PX`(handler/proxy), `US`(upstream), `EN`(engine).

계약 최소셋은 기획서의 단위 테스트 중 인터페이스 계약(반환값, 소유권, 경계값)을 고정하는 것만 고른 것이다. 기능 테스트가 모두 통과하는 것이 현재 개발 범위의 완료 기준이다.

## 2. pending 방식

`tests/pending.txt`에 아직 통과시키지 않은 테스트 ID를 한 줄에 하나씩 적는다. `#`부터 줄 끝은 주석, 빈 줄 허용. 영역별로 `# --- E-ST ---`처럼 묶는다.

| 테스트 결과 | pending.txt에 있음 | 없음 |
| --- | --- | --- |
| 통과 | `UNEXPECTED PASS` → 실패 (목록에서 지우라는 뜻) | `PASS` |
| 실패 | `PENDING` → 성공 취급 | `FAIL` → 실패 |

- 선택 테스트(현재 E-ST-12)는 pending에서 빠진 뒤에도 실패하면 `FAIL (optional)`로 출력하고 종료 코드에 반영하지 않는다.
- root로 실행하면 권한(모드 000) 테스트는 `SKIP (root)`.
- 구현 PR에서 통과시킨 ID를 이 파일에서 지운다. 이것이 진행 추적 수단이다. ID를 추가하지 않는다.

## 3. 러너

두 러너의 출력 형식은 같다.

```
PENDING          E-ST-01  GET / returns index.html
PASS             U-CORE-04  str_to_u64 accepts 0 and 123
FAIL             E-PX-03  hop-by-hop removed  (expected 200, got 502)
---
68 total: 1 pass, 67 pending, 0 fail, 0 unexpected, 0 skip
```

종료 코드는 `FAIL`(선택 제외)이나 `UNEXPECTED PASS`가 하나라도 있으면 1, 아니면 0.

| 명령 | 내용 |
| --- | --- |
| `make unit` | `build/run_unit --pending tests/pending.txt` |
| `make e2e` | `python3 tests/e2e/run_e2e.py --bin build/httpd --pending tests/pending.txt` |
| `make test` | 위 둘 + 합계 줄 |
| `make test ONLY=E-PX` | ID 접두사로 거르기 (unit·e2e 공통) |
| `build/run_unit --list`, `run_e2e.py --list` | 등록된 ID 목록 |
| `run_e2e.py -v`, `run_unit -v` | 실패·pending 사유 출력 |
| `make asan` | ASan + UBSan 빌드(`build-asan/`)로 `make test` |

- **run_unit** (C): 테스트마다 `fork()`한 자식에서 실행하고 자식은 `alarm(5)`를 건다. 시그널 종료(스텁이 돌려준 NULL 역참조 등)와 타임아웃은 실패다. 스텁 상태에서 크래시하는 것이 정상이므로 격리가 필수다.
- **run_e2e** (Python 표준 라이브러리만): 테스트당 30초 제한. 예외·타임아웃·`ServerStartError`는 실패다. 응답 파서는 `http.client`를 쓰지 않고 직접 구현했다(비정상 응답을 감추지 않도록).

E2E 공통 판정: `harness.Server`가 끝날 때 서버가 살아 있는지, 좀비 자식이 없는지, (Linux에서) 열린 fd 수가 시작 때로 돌아왔는지 확인하고 어기면 그 테스트는 실패다. 프록시 테스트는 백엔드당 유휴 연결 상한만큼 fd 여유를 둔다.

Stage 5 프로세스 검증은 `python3 tests/bench/worker_verify.py`로 실행한다. 스레드 풀 과부하, 시그널 종료, FD 한도 복구를 루프백에서 재현한다.

## 4. fake와 fixture

| 이름 | 역할 |
| --- | --- |
| `tests/fakes/fake_backend.py` | E2E용 백엔드. 모드 `echo`·`id`·`slow MS`·`silent`·`broken`·`drop`·`chunked`·`close`·`big N`·`truncate N`. 모든 모드가 `GET /__stats`, `GET /__status/NNN`에 응답. 사용법은 파일 머리 주석 |
| `tests/fakes/fake_handler.{h,c}` | C 계약 테스트 도우미: 파서를 거치지 않고 `http_request_t`를 채움, 임시 root를 가진 `config_t`, 가짜 시계 |
| `tests/fixtures/www/` | 정적 파일 (index.html, style.css, app.js, logo.png, photo.jpg, dir/, noindex/, 한글.txt) |
| `tests/fixtures/cgi-bin/` | CGI 스크립트 (echo.py, status.sh, broken.sh, exit1.sh, sleep.sh, big.sh, noexec.sh) |
| `tests/fixtures/conf/` | 설정 파일 (basic, proxy, bad_directive, bad_port, bad_upstream_ref) |

테스트 시작 시 `harness.prepare_fixtures()`가 만드는 것 (커밋 금지, `.gitignore`에 있음): `www/big.bin`(100MB, 시드 고정, 이미 맞으면 건너뜀), `www/secret.txt`(모드 000), `www/escape`(루트 밖 심볼릭 링크), `www/fifo`.

## 5. 테스트 추가 방법

- **C**: `tests/unit/contract_min/test_<영역>.c`(또는 추후 `tests/unit/<영역>/`)에 `static void t_<id 소문자·밑줄>(void)`를 쓰고 `register_<영역>_tests()`에서 `test_register("<ID>", "<이름>", fn)`. 한 함수 = 한 ID. 파일시스템은 `mkdtemp` 임시 디렉토리만, 네트워크는 `127.0.0.1:0`만 쓴다. 공개 헤더만 include한다.
- **Python**: `tests/e2e/test_<분류>.py`에 `@test("<ID>", "<이름>")` 함수를 쓴다. 타이밍 테스트는 설정의 기본값 지시어(`header_timeout_ms 1000` 등)로 시간을 줄이고, 허용 오차는 ±50% 또는 최소 500ms.
- 에러만 확인하는 테스트는 스텁(항상 실패를 돌려줌)에서도 통과할 수 있다. 그런 ID에는 같은 함수의 정상 입력 대조 검사를 함께 둔다. 새 테스트를 쓰면 스텁 상태에서 실패하는지 확인한다.
- 기획서에 있는 ID만 쓴다. 테스트가 틀렸다고 판단되면 구현으로 맞추지 말고 테스트 수정 PR을 따로 올린다.

## 6. 추후 테스트 도입 순서

기능 테스트가 모두 통과한 뒤(M2) 아래 순서로 넣는다.

1. 메모리 검사: N-MEM-01~03, 05
2. common 단위 테스트: U-COM-01~45
3. 부하 테스트: N-LOAD-01~05, 07
4. 시그널·동시성: N-SIG-01~05, N-CON-01~04
5. 나머지 단위 테스트: U-DS, U-HD, U-RT, U-PX, U-US, U-EN, U-CORE
6. 경계 테스트: C-01~10
7. 퍼징·이식성·v2 비교: N-MEM-04, N-PORT-01~05, N-LOAD-06

회귀 테스트 규칙: 기능 테스트 도중 버그를 고치면 그 버그를 재현하는 단위 테스트를 기획서의 해당 ID로 같은 PR에 넣는다 (예: E-ER-13을 고치면 U-COM-10).
