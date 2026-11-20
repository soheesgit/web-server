# tests/

테스트 층, ID, pending 규칙, 러너 사용법은 [docs/TESTING.md](../docs/TESTING.md)에 있다. 이 파일은 디렉토리 안내만 한다.

| 경로 | 내용 |
| --- | --- |
| `pending.txt` | 아직 통과시키지 않은 테스트 ID. 구현 PR에서 통과시킨 ID를 지운다 |
| `unit/testlib.{h,c}` | C 테스트 등록, `CHECK*` 매크로, 임시 디렉토리 도우미 |
| `unit/run_unit.c` | C 러너 (`build/run_unit`). 테스트마다 fork·alarm(5) |
| `unit/contract_min/test_<영역>.c` | 계약 최소셋 (영역별 `register_<영역>_tests`) |
| `e2e/harness.py` | 서버·백엔드 프로세스, 원시 소켓 클라이언트, 독립 응답 파서, fixture 준비 |
| `e2e/run_e2e.py` | E2E 러너 (`make e2e`). 테스트당 30초 |
| `e2e/test_<분류>.py` | 기능 테스트 (E-ST, E-CG, E-PX, E-KA, E-ER, E-SL, E-CF) |
| `fakes/fake_backend.py` | 가짜 백엔드 (모드는 파일 머리 주석) |
| `fakes/fake_handler.{h,c}` | C 테스트용 요청·설정·시계 도우미 |
| `fixtures/www/`, `cgi-bin/`, `conf/` | 정적 파일, CGI 스크립트, 설정 파일. 생성 fixture(`big.bin`, `secret.txt`, `escape`, `fifo`)는 테스트 시작 시 만들어지고 커밋하지 않는다 |

```sh
make unit                      # C 계약 최소셋
make e2e                       # 기능 테스트
python3 tests/e2e/run_e2e.py --bin build/httpd --pending tests/pending.txt --only E-ST -v
build/run_unit --pending tests/pending.txt --only U-CORE -v
```
