# 결정 기록

결정을 바꿀 때는 기존 항목을 고치지 않는다. 새 항목을 추가하고 이전 항목의 상태를 "폐기 (D-0NN으로 대체)"로 바꾼다. 상태는 `확정`(팀 합의), `제안`(검토 대기), `폐기` 중 하나다.

형식:

```
## D-0NN 제목
- 상태: 확정 | 제안 | 폐기
- 날짜: YYYY-MM-DD
- 맥락: 왜 결정이 필요했는지 한두 문장
- 결정: 무엇을 하기로 했는지
- 대안: 검토했지만 택하지 않은 것
- 영향: 파일, 테스트 ID
```

## 확정

## D-001 traversal 응답
- 상태: 확정
- 날짜: 2026-11-20
- 맥락: 루트 이탈 시도의 응답을 파서 단계(400)와 정적 처리 단계(403)로 나눌지 정해야 했다 (기획서 15.4).
- 결정: 퍼센트 디코딩·정규화 후 루트 위로 벗어나면 파서에서 400. static은 심볼릭 링크로 루트 밖을 가리키는 경우만 403.
- 대안: 모두 403으로 통일, 모두 400으로 통일.
- 영향: `downstream/request_parser.h`, `downstream/uri.h`, `handler/static.c`. U-DS-19, U-DS-28, U-HD-14, E-ST-05, E-ST-06.

## D-002 %2F 처리
- 상태: 확정
- 날짜: 2026-11-20
- 맥락: 인코딩된 `/`(`%2F`)를 디코딩할지 거부할지 (기획서 15.4).
- 결정: 일반 `/`로 디코딩한 뒤 정규화와 traversal 검사를 한다. 프록시는 디코딩하지 않은 `raw_path`를 그대로 전달한다.
- 대안: `%2F`를 400으로 거부, 디코딩하지 않고 그대로 둠.
- 영향: `downstream/uri.h`, `upstream/request_writer.h`. U-DS-22, U-US-05.

## D-003 chunked 요청 본문
- 상태: 확정
- 날짜: 2026-11-20
- 맥락: 요청 본문의 chunked를 지원할지 (기획서 15.4).
- 결정: 미지원. `Transfer-Encoding: chunked` 요청은 501.
- 대안: 확장 단계에서 지원.
- 영향: `downstream/request_parser.h`. U-DS-14, E-ER-09.

## D-004 응답 타임아웃 시 GET 재시도
- 상태: 확정
- 날짜: 2026-11-20
- 맥락: 업스트림 응답 대기 시간을 넘겼을 때 멱등 요청을 재시도할지 (기획서 15.4).
- 결정: 하지 않는다. 504. 재시도는 요청을 한 바이트도 보내지 못한 연결 실패와, 재사용 연결의 첫 송신 실패에만 한다.
- 대안: GET·HEAD는 다른 백엔드로 1회 재시도.
- 영향: `handler/handler.h` (`proxy_on_fail`), `upstream/relay.h`. U-PX-08, E-PX-10.

## D-005 작업 큐 가득
- 상태: 확정
- 날짜: 2026-11-20
- 맥락: 스레드 풀 작업 큐가 가득 찼을 때 즉시 거부할지 기다릴지 (기획서 15.4).
- 결정: 503 응답 후 close (`engine_callbacks_t.on_busy`).
- 대안: accept를 멈추고 대기.
- 영향: `engine/engine.h`. U-EN-03.

## D-006 304 조건부 요청
- 상태: 확정
- 날짜: 2026-11-20
- 맥락: `If-Modified-Since` → 304를 범위에 넣을지 (기획서 15.4).
- 결정: 선택 기능. E-ST-12는 optional 테스트로 둔다.
- 대안: 필수 기능으로 포함, 범위에서 제외.
- 영향: `handler/static.c`, `tests/pending.txt`. U-HD-19, E-ST-12.

## 제안

아래는 initial commit 작업 중 지시문과 기획서만으로 정할 수 없었던 점이다. 보수적인 쪽(더 엄격하게 거부, 더 적은 기능, 인터페이스를 늘리지 않음)을 헤더와 테스트에 반영해 두었다. 팀 검토 후 확정 또는 폐기한다.

## D-007 E-CF-01은 initial commit에서 이미 통과
- 상태: 제안
- 날짜: 2026-11-20
- 맥락: 지시문 7.3절은 `main.c`의 `--help`(사용법 출력, 종료 0)와 `config_print_usage`를 실제로 구현하라고 하고, 12.3절은 모든 테스트가 PENDING이기를 요구한다. E-CF-01의 기대 결과(사용법 출력, 종료 코드 0)는 7.3절 구현만으로 충족되므로 pending에 두면 UNEXPECTED PASS가 되어 `make test`가 실패한다.
- 결정: E-CF-01을 `tests/pending.txt`에 넣지 않는다. pending은 122줄, 요약 줄은 `123 total: 1 pass, 122 pending, 0 fail, 0 unexpected`.
- 대안: E-CF-01에 기획서에 없는 검사를 덧붙여 억지로 실패시키기, `--help`를 스텁으로 두기(7.3절 위반).
- 영향: `tests/pending.txt`, E-CF-01.

## D-008 fake_backend에 truncate 모드와 /__status 경로 추가
- 상태: 제안
- 날짜: 2026-11-20
- 맥락: E-PX-12는 "본문 중간 끊김"을, E-PX-16은 "백엔드가 404·500 응답"을 요구하지만 지시문 9.7절의 모드로는 만들 수 없다 (`drop`은 아무것도 보내지 않고 닫으므로 E-PX-13용이다).
- 결정: `truncate N`(Content-Length N을 알리고 본문 절반만 보낸 뒤 닫음) 모드와, 모든 모드에서 `GET /__status/NNN`(그 상태 코드로 응답) 경로를 추가한다. `drop`은 지시문 정의를 그대로 둔다.
- 대안: `drop`을 기획서 정의("헤더 보내고 끊기")로 바꾸기 — 그러면 E-PX-13을 만들 수 없다.
- 영향: `tests/fakes/fake_backend.py`. E-PX-12, E-PX-16.

## D-009 설정 파일 문법 세부
- 상태: 제안
- 날짜: 2026-11-20
- 맥락: 지시문 4.5절은 지시어 목록만 주고 기본값 지시어의 이름·단위, 상대 경로 기준, 반복 지시어 처리는 정하지 않았다.
- 결정: 정확한 BNF는 `core/config.h` 주석. 요점: 기본값 지시어 이름은 `config_t` 필드 이름과 같다(`header_timeout_ms 1000`), 시간은 ms·크기는 바이트 정수, 상대 경로는 실행 디렉토리 기준, `listen`은 `port` 또는 `host:port`, 같은 단일 값 지시어가 반복되면 마지막 값, root와 static·cgi 루트는 존재하는 디렉토리여야 함, 정의되지 않은 upstream을 가리키는 route는 오류.
- 대안: 상대 경로를 설정 파일 위치 기준으로 해석, 반복 지시어를 오류로 처리.
- 영향: `core/config.h`. U-CORE-17, U-CORE-21, E-CF-02, 모든 E2E 설정.

## D-010 프록시 재시도 시 직렬화한 요청을 그대로 다시 보냄
- 상태: 제안
- 날짜: 2026-11-20
- 맥락: `proxy_on_fail`은 원래 `http_request_t`를 받지 않으므로 다른 백엔드로 재시도할 때 요청을 다시 직렬화할 수 없다. keep_host가 false이면 Host 헤더에 첫 백엔드 주소가 들어가 있다.
- 결정: 재시도는 `job->request` 바이트를 바꾸지 않고 다시 보낸다. Host는 첫 시도 백엔드의 `host:port`로 남는다.
- 대안: `proxy_job_t`에 Host 재작성 정보를 추가 (인터페이스 변경), Host에 upstream 그룹 이름 사용.
- 영향: `handler/handler.h`, `upstream/request_writer.h`. U-PX-05, E-PX-08.

## D-011 재사용 연결의 첫 송신 실패는 relay가 같은 백엔드로 1회 다시 연결
- 상태: 제안
- 날짜: 2026-11-20
- 맥락: 지시문은 `upstream_conn_t.reused`에 "첫 송신 실패 시 새 연결로 1회 재시도 허용"이라 적었지만, relay는 재시도 여부를 판단하지 않는다고도 적었다. 풀의 유휴 연결은 백엔드가 이미 닫았을 수 있다.
- 결정: 이 경우는 "재시도 판단"이 아니라 연결 상태 문제로 보고 relay가 처리한다: 재사용 연결에서 요청 송신이 실패하거나 응답 바이트를 하나도 받기 전에 EOF·리셋이면, 그 연결을 `upstream_release(false)`하고 같은 백엔드에 새 연결로 한 번 더 보낸다. `attempts`를 늘리지 않고 `upstream_report`도 하지 않는다.
- 대안: `PFAIL_SEND`로 돌려주고 `proxy_on_fail`이 판단.
- 영향: `upstream/relay.h`. U-US-26, E-PX-14.

## D-012 accept한 소켓의 blocking 모드는 conn_new가 정함
- 상태: 제안
- 날짜: 2026-11-20
- 맥락: 지시문 5.1절은 `listener_accept`가 cloexec만 보장한다고 적었고 기획서 U-DS-38은 non-blocking도 기대한다. macOS는 accept한 소켓이 대기 소켓의 O_NONBLOCK을 물려받고 Linux는 물려받지 않는다.
- 결정: `listener_accept`는 cloexec만 보장하고 O_NONBLOCK 상태는 정하지 않는다. `conn_new`가 `conn_env_t.nonblocking`에 따라 O_NONBLOCK을 켜거나 끄고, v1이면 SO_RCVTIMEO·SO_SNDTIMEO를 설정한다.
- 대안: `listener_accept`가 항상 non-blocking으로 돌려줌.
- 영향: `downstream/listener.h`, `downstream/conn.h`. U-DS-36, U-DS-40.

## D-013 CGI 스크립트 경로와 환경 변수
- 상태: 제안
- 날짜: 2026-11-20
- 맥락: 지시문은 "환경변수는 필요한 것만"이라고만 적었다.
- 결정: 스크립트 = route root + (정규화 경로에서 prefix를 뗀 첫 세그먼트), 나머지는 `PATH_INFO`. 환경 변수 목록은 `handler/handler.h`의 `cgi_handle` 주석. `Proxy` 요청 헤더는 `HTTP_PROXY`로 옮기지 않는다(httpoxy). 서버 환경은 물려주지 않고 `PATH=/usr/bin:/bin`만 준다.
- 대안: 서버 환경 전체를 물려줌.
- 영향: `handler/handler.h`. U-HD-21, E-CG-01, E-CG-02.

## D-014 생성 fixture는 tests/fixtures/www/ 에 두고 재사용
- 상태: 제안
- 날짜: 2026-11-20
- 맥락: 지시문 2장은 생성 fixture를 "임시 디렉토리에 만들고 끝나면 지운다"고, 9.6절은 `www/big.bin` 등을 만들고 `.gitignore`에 넣으며 이미 맞으면 건너뛴다고 적었다. `escape` 링크가 `../conf/basic.conf`를 가리키려면 www가 fixtures 안에 있어야 한다.
- 결정: 9.6절을 따른다. `tests/fixtures/www/`에 만들고 지우지 않는다(다음 실행에서 재사용). git에는 들어가지 않는다.
- 대안: 매 실행 fixtures 전체를 임시 디렉토리로 복사.
- 영향: `tests/e2e/harness.py`, `.gitignore`.

## D-015 프록시 응답에서 Date·Server·Content-Length를 복사하지 않음
- 상태: 제안
- 날짜: 2026-11-20
- 맥락: `http_head_out_t.fields`에는 writer가 붙이는 Date·Server·Content-Length·Connection을 넣지 않는다. 업스트림 응답을 그대로 복사하면 중복된다.
- 결정: `proxy_on_head`는 hop-by-hop과 함께 `Date`, `Server`, `Content-Length`를 건너뛴다. 클라이언트는 프록시의 Date·Server를 본다.
- 대안: 업스트림 Date를 보존하는 별도 플래그 (인터페이스 변경).
- 영향: `handler/handler.h`. U-PX-10, E-PX-16.

## D-016 E2E는 빈 포트를 쓰고, proxy.conf는 백엔드 4대
- 상태: 제안
- 날짜: 2026-11-20
- 맥락: 기획서는 고정 포트 18001~18003을, 지시문은 `free_port()`와 `{BACKEND1}…` 치환을 적었다. E-PX-15는 Host 전용 백엔드가 하나 더 필요하다.
- 결정: 모든 포트는 `free_port()`. `proxy.conf`의 upstream은 `app`(`{BACKEND1}`~`{BACKEND3}`)과 `admin`(`{BACKEND4}`) 2개.
- 대안: 고정 포트 (CI에서 충돌 위험).
- 영향: `tests/fixtures/conf/proxy.conf`, `tests/e2e/test_proxy.py`.

## D-017 에러만 보는 계약 테스트에 정상 대조 검사 추가
- 상태: 제안
- 날짜: 2026-11-20
- 맥락: U-CORE-05(모두 -1), U-US-12(모두 ERROR) 같은 테스트는 항상 실패를 돌려주는 스텁에서도 통과한다. 지시문 12.3절은 스텁에서 통과하는 테스트가 없어야 한다고 요구한다.
- 결정: 해당 테스트에 같은 함수의 정상 입력 한 개를 먼저 검사한다. 기대값은 약해지지 않는다.
- 대안: 스텁의 errno(ENOSYS)를 검사 — 구현 후 의미가 없다.
- 영향: U-CORE-05, U-CORE-21, U-DS-28, U-US-12.

## D-018 conn 계약 테스트는 실제 router와 static 처리기를 씀
- 상태: 제안
- 날짜: 2026-11-20
- 맥락: 기획서는 conn 테스트에 "항상 200 ok를 돌려주는 fake_handler"를 쓰지만, 확정 인터페이스에는 conn이 부르는 처리기를 바꿔 끼울 수단이 없다(conn은 `handler_dispatch`를 직접 링크).
- 결정: U-DS-40·U-DS-44는 임시 디렉토리의 `ok.txt`(내용 `ok`)를 서빙하는 실제 STATIC 라우트로 테스트한다.
- 대안: `conn_env_t`에 처리기 함수 포인터 추가 (인터페이스 변경).
- 영향: `tests/unit/contract_min/test_downstream.c`.

## D-019 MIME 표
- 상태: 제안
- 날짜: 2026-11-20
- 맥락: 확장자별 Content-Type과 charset 규칙이 정해지지 않았다.
- 결정: 표는 `handler/mime.h` 주석. `text/*`에만 `; charset=utf-8`를 붙인다.
- 대안: json·svg에도 charset.
- 영향: `handler/mime.h`. E-ST-01, E-ST-02.

## D-020 big.sh 출력은 10MB
- 상태: 제안
- 날짜: 2026-11-20
- 맥락: 기획서 E-CG-08은 50MB, 지시문 9.6절은 10MB다.
- 결정: 지시문을 따라 10MB (10485760바이트).
- 대안: 50MB (CI 시간 증가).
- 영향: `tests/fixtures/cgi-bin/big.sh`, E-CG-08.

## D-021 engine.h의 EV_ERROR와 kqueue 매크로 충돌
- 상태: 제안
- 날짜: 2026-11-20
- 맥락: `<sys/event.h>`(macOS·FreeBSD)는 `EV_ERROR`를 매크로로 정의한다. `engine/engine.h`의 `enum { EV_READ, EV_WRITE, EV_ERROR }`와 같은 파일에서 함께 include하면 컴파일되지 않는다.
- 결정: 확정 인터페이스는 바꾸지 않는다. `poller.h`는 `engine.h`를 include하지 않고 같은 비트 값(1·2·4)을 쓴다고 명시하며, `poller_kqueue.c`는 `engine.h`를 include하지 않는다.
- 대안: engine.h의 열거값 이름을 `ENGINE_EV_*`로 변경 (인터페이스 변경, 세 명 승인 필요).
- 영향: `engine/engine.h`, `engine/poller.h`, `engine/poller_kqueue.c`.

## D-022 poller 구현 파일 선택 조건
- 상태: 제안
- 날짜: 2026-11-20
- 맥락: 지시문 7.4절대로면 Linux에서 `-DHTTPD_FORCE_POLL`을 주면 epoll과 poll이 둘 다 컴파일된다.
- 결정: `poller_epoll.c`는 `__linux__ && !HTTPD_FORCE_POLL`, `poller_kqueue.c`는 `(__APPLE__ || __FreeBSD__) && !HTTPD_FORCE_POLL`, `poller_poll.c`는 그 밖의 OS 또는 `HTTPD_FORCE_POLL`. 어느 경우든 정확히 하나만 심볼을 정의한다.
- 대안: 없음.
- 영향: `engine/poller_*.c`.

## D-023 금지 함수 목록에 기획서 4.1절 항목 포함
- 상태: 제안
- 날짜: 2026-11-20
- 맥락: 지시문 8.3절 목록에는 `strtok`, `atoi`, `gmtime`, `strerror`가 없지만 기획서 4.1절은 금지한다.
- 결정: 더 엄격한 쪽으로 둘을 합친다.
- 대안: 지시문 목록만.
- 영향: `scripts/lint_forbidden.sh`, `docs/CONVENTIONS.md`.

## D-024 http_find_head_end의 out_head_len은 앞 빈 줄을 포함
- 상태: 제안
- 날짜: 2026-11-20
- 맥락: 지시문은 `out_skip`(앞 빈 줄)과 `out_head_len`(종료자까지)의 기준점을 정하지 않았다. `http_request_t.head_len`은 "앞 빈 줄 포함"이다.
- 결정: `out_head_len`은 `p`부터 종료자 끝까지의 바이트 수(=`out_skip` 포함). 시작 줄은 `p + out_skip`에서 시작한다. `out_head_len > max_head`이면 431.
- 대안: `p + out_skip` 기준.
- 영향: `common/http_headers.h`. U-COM-01, U-COM-02, U-COM-04, U-DS-06.

## D-025 nonblocking conn에 engine이 없으면 CGI·프록시는 500
- 상태: 제안
- 날짜: 2026-11-20
- 맥락: 계약 테스트는 engine 없이 conn을 non-blocking으로 돌린다. 이때 추가 fd를 등록해야 하는 경로의 동작이 정해지지 않았다.
- 결정: `conn_env_t.nonblocking`이 true이고 `engine`이 NULL이면 CGI·PROXY 경로는 `plan_error(500)`으로 응답한다.
- 대안: 정의되지 않은 동작으로 둠.
- 영향: `downstream/conn.h`. U-DS-40.

## D-026 E-ST-09의 브라우저 확인은 자동 테스트에서 제외
- 상태: 제안
- 날짜: 2026-11-20
- 맥락: 기획서 E-ST-09는 "curl 병렬 20개 + 브라우저 수동 확인"이다.
- 결정: 자동 테스트는 fixture 에셋 20개 병렬 GET(전부 200, 바이트 일치)만 검사한다. 브라우저 확인은 3단계 시연에서 사람이 한다.
- 대안: 없음.
- 영향: `tests/e2e/test_static.py`.

## D-027 지시문 7.2절의 PROXY_FAIL_CONNECT는 PFAIL_CONNECT로 읽음
- 상태: 제안
- 날짜: 2026-11-20
- 맥락: 스텁 반환값 표가 `PROXY_FAIL_CONNECT`를 쓰지만 확정 인터페이스(4.7절)의 열거값은 `PFAIL_CONNECT`다.
- 결정: 확정 인터페이스의 이름 `PFAIL_CONNECT`를 쓴다.
- 대안: 없음.
- 영향: `upstream/relay.c`.

## D-028 루트 README에 기존 과제 요구사항을 부록으로 보존
- 상태: 폐기 (D-031로 대체)
- 날짜: 2026-11-20
- 맥락: `main`의 첫 커밋 README에는 과제 요구사항이 있었고, 지시문 10.1절은 루트 README의 내용을 새로 정한다.
- 결정: 10.1절 순서로 쓰고, 기존 과제 요구사항은 맨 끝 "부록: 과제 요구사항"에 그대로 둔다.
- 대안: 삭제하고 `main`의 첫 커밋으로만 남김.
- 영향: `README.md`.

## D-029 CGI 응답의 본문 길이 방식
- 상태: 제안
- 날짜: 2026-11-20
- 맥락: CGI 출력에 Content-Length가 없을 때 클라이언트 버전별 전송 방식을 정해야 했다 (E-CG-08).
- 결정: CGI 헤더에 유효한 Content-Length가 있으면 LENGTH, 없으면 HTTP/1.1 클라이언트에는 CHUNKED, HTTP/1.0 클라이언트에는 close로 끝낸다 (`response_write_head`의 1.0 + CHUNKED 규칙).
- 대안: 항상 close.
- 영향: `handler/handler.h` (`cgi_parse_head`), `downstream/conn.h`. E-CG-08.

## D-030 CI의 clang-format을 18.1.8로 고정
- 상태: 제안
- 날짜: 2026-11-20
- 맥락: 지시문 8.5절은 Linux CI에서 `apt-get install clang-format`을 쓰라고 했지만, `ubuntu-latest`가 Ubuntu 26.04로 바뀌면서 apt의 clang-format 메이저 버전이 올라갔고, 한글 줄끝 주석의 정렬 결과가 18과 달라 v0.1-interfaces 태그 커밋의 Linux CI가 Lint에서 실패했다.
- 결정: CI는 venv에 `clang-format==18.1.8`(PyPI 바이너리 휠)을 설치해 `CLANG_FORMAT`으로 넘긴다. 로컬에서도 같은 버전을 쓴다 (`python3 -m venv .venv && .venv/bin/pip install clang-format==18.1.8`, `make lint CLANG_FORMAT=.venv/bin/clang-format`). 태그는 옮기지 않고(force push 금지) 후속 커밋으로 고친다.
- 대안: apt 버전에 맞춰 재포맷 (이미지가 바뀔 때마다 다시 깨짐), clang-format 검사를 CI에서 빼기.
- 영향: `.github/workflows/ci.yml`, `README.md`.

## D-031 루트 README는 main의 과제 요구사항 문서를 그대로 유지
- 상태: 확정
- 날짜: 2026-11-20
- 맥락: 지시문 10.1절대로 루트 README를 프로젝트 안내로 새로 썼으나(D-028), 저장소 주인이 main의 README를 덮어쓰지 말라고 요청했다.
- 결정: develop의 `README.md`를 main과 같은 내용(과제 요구사항)으로 되돌린다. 빌드·실행·테스트 방법은 `docs/TESTING.md`와 `tests/README.md`, 디렉토리 안내는 `docs/ARCHITECTURE.md`와 각 `src/<dir>/README.md`, 협업 규칙은 `AGENTS.md`와 `docs/CONVENTIONS.md`에서 본다.
- 대안: 프로젝트 안내를 README 앞에 두고 과제 요구사항을 부록으로 둠 (D-028).
- 영향: `README.md`.

## D-033 chunked 요청 본문 지원
- 상태: 제안
- 날짜: 2026-11-21
- 맥락: 공통 프레이밍과 추적기는 chunked 경계를 인식하지만 D-003과 요청 파서 계약은 chunked를 501로 처리한다.
- 결정: 팀 합의 후 요청 프레이밍에 CHUNKED를 허용하고, 연결 모듈이 청크를 디코딩해 body를 채우도록 계약을 갱신한다. 수신 한도는 디코딩된 길이와 청크 메타데이터에 적용하고, 다음 요청을 보존하기 위해 소비할 원본 바이트 수를 별도로 추적한다. 업스트림에 디코딩된 본문을 보낼 때는 실제 길이로 Content-Length를 다시 만든다.
- 대안: D-003의 501 정책 유지.
- 영향: `common/types.h`, `downstream/request_parser.h`, `downstream/conn.h`, `upstream/request_writer.h`, U-DS-14, E-ER-09.

## D-034 응답 계획의 활성 union 멤버 초기화
- 상태: 제안
- 날짜: 2026-11-21
- 맥락: plan_init 주석은 fd의 -1과 업스트림 포인터의 NULL을 동시에 요구한다. 두 값은 body union의 같은 저장 공간을 사용한다.
- 결정: BODY_NONE에서는 body에 접근하지 않고, 처리기가 kind에 맞는 멤버를 통째로 초기화하도록 주석을 명확히 한다.
- 대안: 본문별 생성 함수 추가.
- 영향: `common/types.h`, `handler/plan.h`, `handler/static.c`, `handler/cgi.c`, `handler/proxy.c`, U-HD-01.

## D-035 업스트림 전송 코딩 중계
- 상태: 제안
- 날짜: 2026-11-21
- 맥락: 공통 프레이밍은 마지막 코딩이 chunked인 목록의 본문 경계를 추적할 수 있다. 다운스트림 직렬화 계약은 Transfer-Encoding에 chunked만 쓴다.
- 결정: 중계하는 원본 바이트에 앞선 전송 코딩이 남아 있다면, 변환을 지원하기 전까지 프록시가 해당 응답을 헤더 송신 전에 502로 처리하도록 합의한다. HTTP/1.0으로 chunked 응답을 중계할 때는 청크 프레이밍을 제거한 바이트만 전송한다.
- 대안: 원래의 전송 코딩 목록을 보존하거나 각 코딩을 해제한 다음 본문을 다시 프레이밍한다.
- 영향: `handler/handler.h`, `handler/proxy.c`, `downstream/response_writer.h`, E-PX-05, E-PX-16.

## D-036 listener의 원자적 cloexec 설정
- 상태: 제안
- 날짜: 2026-11-23
- 맥락: listener 계약은 Linux에서 accept4를 지정하고 전역 규약은 OS별 API를 core/platform에 둔다. 현재 core에는 accept 래퍼가 없다.
- 결정: Linux의 원자적 accept를 core/platform에 둘지 합의한다. 현 구현은 POSIX accept 직후 fcntl로 FD_CLOEXEC를 설정한다. 동시에 다른 워커가 fork할 때는 그 사이의 fd 상속을 막기 위해 원자적 accept가 필요하다.
- 대안: listener에서 Linux 분기를 허용한다.
- 영향: `core/platform.h`, `downstream/listener.h`, `downstream/listener.c`, U-DS-36, U-DS-38.

## D-037 v1 송신 정체 및 서버 종료 전달
- 상태: 제안
- 날짜: 2026-11-23
- 맥락: v1은 대기 상한을 확인해야 하지만 송신 정체 전용 한도와 연결에 서버 종료를 알리는 값이 공개 계약에 없다.
- 결정: 별도 설정이 합의되기 전까지 다운스트림 송신 정체에 body_timeout_ms를, 업스트림 본문 읽기 정체에 upstream_response_timeout_ms를 적용한다. 서버 종료 여부는 app과 engine의 종료 계약을 먼저 정한다.
- 대안: 전송별 설정과 연결 종료 통지를 공개 인터페이스에 추가한다.
- 영향: `downstream/conn.h`, `core/config.h`, `engine/engine.h`, E-SL-02, E-SL-04.
