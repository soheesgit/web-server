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

## D-032 U-CORE-31에 닫힌 상대 회귀 검사를 포함
- 상태: 제안
- 날짜: 2026-11-21
- 맥락: 기획서의 U-CORE-31은 구간 전송(1 MB 파일의 offset 1000부터 500,000바이트)만 정의한다. Linux `sendfile`이 일부를 보낸 뒤 상대가 끊기면 양수를 돌려주면서 SIGPIPE를 대기시켜, 첫 구현이 프로세스를 죽였다(`073a934`에서 수정). "한 함수 = 한 ID" 규칙상 같은 함수의 회귀 검사에 새 ID를 만들 수 없다.
- 결정: U-CORE-31 안에서 구간 전송 검사 뒤에, SIGPIPE 기본 처분 + 닫힌 상대로 보내 프로세스가 살아 있고 결국 -1(EPIPE, ECONNRESET, macOS는 ENOTCONN)을 돌려주는지 확인한다.
- 대안: 기획서 정의만 검사하고 회귀는 임시 프로그램으로만 확인, U-CORE-30에 sendfile 검사를 섞기.
- 영향: `tests/unit/contract_min/test_core.c`. U-CORE-31.

## D-033 conn v1의 송신·중계 정체 마감과 종료 판정
- 상태: 제안
- 날짜: 2026-11-21
- 맥락: `downstream/conn.h`는 헤더·본문 수신, CGI, 업스트림 응답 머리의 마감은 정했지만, v1에서 클라이언트로 보내는 중이나 업스트림 본문을 중계하는 중에 진행이 멈췄을 때의 상한과, 처리 순서 11의 "서버 종료 중"을 conn이 어떻게 아는지는 정하지 않았다. `conn_env_t`에는 정지 플래그가 없다.
- 결정: 클라이언트 송신(머리, 메모리·파일·CGI 본문)은 마지막 진행 뒤 `body_timeout_ms`, 업스트림 본문 읽기는 마지막 진행 뒤 `upstream_response_timeout_ms`가 지나면 연결을 닫는다(응답 머리가 이미 나갔으므로 에러 응답 없음). v1 conn은 서버 종료를 직접 판정하지 않고, engine이 유예 뒤 `shutdown(SHUT_RDWR)`로 연결을 끝내는 것(`engine_run` 명세)에 맡긴다.
- 대안: 송신 정체 전용 설정값 추가 (config 변경 필요). `conn_env_t`에 정지 플래그 추가 (공개 헤더 변경 필요).
- 영향: `downstream/conn.c`. E-SL-*, E-KA-*.

## D-034 HTTP/1.0에 업스트림 chunked 응답 전달
- 상태: 제안
- 날짜: 2026-11-23
- 맥락: `proxy_on_head` 명세는 HTTP/1.0에서 chunked 응답을 CLOSE로 표시한다. 공용 본문 추적기의 소비 범위에는 청크 구분자도 포함된다.
- 결정: 청크의 데이터만 추출하는 공용 계약이 생기기 전에는 응답 머리를 보내기 전에 502로 처리한다. 업스트림 연결은 재사용하지 않는다.
- 대안: 공용 파서와 다운스트림을 확장해 청크의 데이터만 중계한다.
- 영향: `src/downstream/conn.c`, `src/common/http_framing.h`, `src/handler/handler.h`.

## D-035 머리 한도를 넘긴 바이너리 시작 줄
- 상태: 제안
- 날짜: 2026-11-23
- 맥락: E-ER-12는 바이너리 입력에 400 또는 즉시 종료를 허용한다. 공용 파서는 머리 한도에 먼저 도달하면 431로 판단할 수 있다.
- 결정: 431로 분류된 입력의 시작 줄에 NUL이 있으면 연결을 응답 없이 닫는다. 일반적인 머리 길이 초과에는 431로 응답한다.
- 대안: E-ER-12의 허용 상태에 431을 포함한다.
- 영향: `src/downstream/conn.c`, E-ER-12.

## D-039 빈 응답 계획에서 union의 초기 상태

- 상태: 제안
- 날짜: 2026-11-23
- 맥락: `plan_init` 주석의 fd 초기값과 `body.upstream.conn`의 NULL은 같은 union 저장 공간에 동시에 둘 수 없다.
- 결정: `BODY_NONE`은 본문 자원을 소유하지 않는다. 이 상태에서는 fd·마감 초기값을 기록하고, 활성 멤버는 처리기가 종류를 지정할 때 초기화한다.
- 대안: 본문 저장 공간을 구조체로 바꾸어 모든 필드를 동시에 초기화.
- 영향: `handler/plan.c`. U-HD-01, U-HD-04.

## D-040 CGI 출력의 잘못된 길이 및 중복 상태

- 상태: 제안
- 날짜: 2026-11-23
- 맥락: CGI 응답에 `Content-Length`가 없을 때의 규칙은 D-029에 있으나 잘못된 길이, 서로 다른 길이가 반복되는 경우, 상태가 중복되는 경우는 명시되지 않았다.
- 결정: 잘못된 길이, 충돌하는 길이, 중복 `Status`는 CGI 응답 오류(502)로 처리한다.
- 대안: 오류가 있는 항목을 버리고 나머지 헤더만 사용.
- 영향: `handler/cgi.c`. U-HD-26~30, E-CG-05.

## D-041 프록시 시도 예약과 재시도 명세 정합성

- 상태: 제안
- 날짜: 2026-11-24
- 맥락: D-010처럼 직렬화한 요청을 그대로 재사용하면 다음 백엔드가 첫 시도의 Host를 받는다. 또한 응답 머리에서 성공을 보고하면 이후 본문 미달과 잘못된 청크로 끝난 시도를 구분할 수 없다. 기준 develop에는 D-036이 없고, D-004의 재시도 범위 문구와 U-PX-07의 GET 부분 송신 기대값이 다르다.
- 결정: 현재 v1은 기존 relay 호출에 빈 요청 버퍼를 넘겨 먼저 대상을 예약하고, 선택 대상이 확정된 뒤 conn에서 원래 요청을 직렬화한다. 재시도 시 이 순서를 반복한다. 풀 연결의 같은 대상 재연결은 첫 송신 0바이트 실패에 한정한다. 응답을 기다리기 시작한 시도는 재시도하지 않는다. GET·HEAD 부분 송신은 현재 U-PX-07과 처리기 계약을 따른다. 본문 완료 후 성공을 보고하고, 클라이언트 취소는 상태에 반영하지 않는다. 팀 검토 후 예약 및 결과 보고의 계약을 공개 API로 명시하고 D-004의 범위를 정리한다.
- 대안: 첫 백엔드의 Host를 재사용하거나 응답 머리 수신만으로 회복 판단.
- 영향: `downstream/conn.c`, `handler/proxy.c`, `upstream/relay.c`, `upstream/pool.c`, U-PX-05, U-PX-07, E-PX-08~14. 공개 서명·타입 변경은 별도 계약 PR에서 세 명의 승인을 받는다.

## D-042 이벤트 기반 복구 대기열

- 상태: 제안
- 날짜: 2026-11-24
- 맥락: v1 워커는 연결을 끝까지 소유한다. 백엔드 선택을 기다리는 요청을 워커에서 재우면 정적·CGI 요청 처리까지 막힐 수 있다. v2 poller와 연결 상태 머신은 기준 develop에서 스텁이다.
- 결정: 현행 v1은 즉시 503으로 종료한다. 독립 v2 작업에서 이벤트 루프가 그룹별 FIFO의 conn 참조와 서버 전체 대기 상한 100개를 소유한다. 최초 enqueue에 단조 시계로 3초 절대 마감을 저장하고 대체 백엔드나 실패한 시험 요청 이후에도 유지한다. 가장 빠른 cooldown 종료가 마감 이후면 즉시 503을 반환한다. 동시 수용량에 자리가 날 때 FIFO 머리부터 예약하고 해당 요청만 재개한다. 마감, 클라이언트 종료, 서버 종료, 예약 성공은 동일한 루프에서 한 번만 노드를 제거한다. 재개 시에는 대기 FD 감시를 제거하고 새 relay를 등록하며 실패하면 예약을 반환한다.
- 대안: blocking 워커의 sleep, 요청당 대기 스레드, 복구 시 전체 대기 요청을 일괄 깨우기.
- 영향: `engine/` 이벤트 경로, `downstream/conn.c`, `upstream/`. 시계가 앞서는 백엔드, 포화, 취소, 종료 및 타임아웃 경합을 독립 엔진 테스트로 검증한다.

## D-043 운영 설정과 로컬 Forward Proxy

- 상태: 제안
- 날짜: 2026-11-24
- 맥락: LB와 복구 정책을 선택하고 absolute-form을 명시적으로 허용할 수 있어야 한다. 새 config 필드와 route kind는 공개 계약 변경에 해당한다.
- 결정: 현재 작업은 시작 환경 변수와 기존 upstream 및 Host route 설정을 사용한다. Forward Proxy는 로컬 리스너와 로컬 클라이언트, 운영자가 지정한 한 포트, 숫자 loopback 주소의 단일 백엔드가 모두 일치할 때만 연다. 운영 설정을 config 문법과 타입으로 옮기거나 허용 호스트·포트 목록을 늘리는 작업에는 별도 계약 PR과 팀 승인을 적용한다.
- 대안: absolute-form의 DNS를 요청별로 조회해 임의의 공인 주소를 허용.
- 영향: `upstream/backend.c`, `downstream/conn.c`, `downstream/uri.c`, `downstream/request_parser.c`, Forward Proxy 회귀 스크립트.

## D-044 프록시 청크 응답 재직렬화

- 상태: 제안
- 날짜: 2026-11-24
- 맥락: 업스트림 청크 경계를 원본 그대로 전달하면 받은 줄 끝과 트레일러도 노출된다. 다운스트림이 프레이밍을 직접 만들면 검증 단계와 클라이언트에 전달하는 경계를 맞출 수 있다.
- 결정: HTTP/1.1 프록시는 공용 추적기로 청크를 검증한 뒤 데이터만 정규 CRLF 청크로 송신한다. 확장과 트레일러는 검증하되 중계하지 않고 메타데이터 크기는 응답 머리 상한으로 제한한다. 본문 중 오류에는 마지막 청크를 보내지 않는다. HTTP/1.0 청크 응답은 D-034에 따라 머리 전 502를 사용한다.
- 대안: 원본 청크와 트레일러 중계.
- 영향: `downstream/conn.c`, `upstream/response_parser.c`, E-PX-05, E-PX-12 및 프레이밍 회귀 스크립트.

## D-045 AI 라우팅: 그룹별 picker와 고정 키 해싱
- 상태: 제안
- 날짜: 2026-11-24
- 맥락: LLM 벤치(`bench/`)에서 라운드로빈은 요청 수는 고르게 나누지만, 같은 대화가 매번 다른 백엔드로 가서 prefix 캐시를 살리지 못한다(보정: 캐시 적중 시 prefill 약 99% 감소). 기획서 FR-LB-01·03, FR-LLM-08.
- 결정: 설정에 `picker <그룹> rr | hash | bounded [<pct>]`를 추가한다(`upstream_conf_t.picker`, `bound_pct`). `upstream_pick_key(g, key, key_len, exclude, now_ms)`를 새 파일 `upstream/lb_key.c`에 추가해 `X-Session-Id` 헤더를 키로 rendezvous hashing으로 고르고(키는 `proxy_job_t.affinity_key`로 relay까지 전달해 첫 시도의 연결 예약 때 쓴다), bounded면 처리 중 수가 상한(평균 × pct/100 올림) 이상인 백엔드를 건너뛴다. 처리 중 수는 기존 `backend.active`(연결 예약~반납)를 그대로 쓴다. 일반 프록시의 선택(`lb.c`, D-041~D-043의 rr·least_requests·p2c)은 바꾸지 않고, picker 줄이 있는 그룹만 이 경로를 탄다(수평 확장). 재시도(`proxy_on_fail`)는 키를 모르므로 기존 경로 그대로.
- 대안: 모듈로 해싱(백엔드 수가 바뀌면 키 대부분이 옮겨 감), 링 consistent hashing(백엔드가 적어 가상 노드가 필요 없는 rendezvous로 충분), 요청 본문에서 키 추출(FR-LLM-09, 본문 파싱 필요).
- 영향: `core/config.h`·`config.c`, `common/types.h`(`proxy_job_t.affinity_key`), `upstream/upstream.h`·`lb_key.c`(새 파일)·`internal_state.h`(그룹 필드 2개)·`backend.c`(필드 채움)·`relay.c`(첫 시도 선택), `downstream/conn.c`(키 채움), `handler/proxy.c`. 기존 테스트 변화 없음. 검증은 bench 시나리오 C.
- 기본 pct: `bounded`만 쓰면 200(`CFG_BOUND_PCT_DEFAULT`). 기본 picker는 계속 rr.
- 근거 (2026-11-24, bench 시나리오 C, 백엔드 4대, 0.3 req/s, 대화 8개, 240초 × 3회, 이벤트 루프):
  | 분포 | picker | TTFT p50 | TTFT p99 | 캐시 적중 | 넘침 |
  | --- | --- | --- | --- | --- | --- |
  | 균등 | rr | 3.8초 | 11.7초 | 31% | 80% |
  | 균등 | hash | 1.8초 | 10.4초 (8.2~20.7) | 62% | 0% |
  | 균등 | bounded 125 / 150 / 200 | 3.8 / 1.8 / 1.8초 | 15.1 / 14.6 / 12.9초 | 48 / 53 / 54% | 29 / 21 / 15% |
  | Zipf 1.0 | rr | 3.8초 | 12.2초 | 27% | 75% |
  | Zipf 1.0 | hash | 3.8초 | 29.1초 | 58% | 0% |
  | Zipf 1.0 | bounded 125 / 150 / 200 | 3.8 / 3.8 / 3.2초 | 13.8 / 15.3 / 19.5초 | 36 / 39 / 45% | 39 / 31 / 19% |
  - hash는 키가 적거나 치우치면 한 백엔드에 몰려(최대 동시 8, 슬롯 4) p99가 rr의 2.4배가 된다.
  - bounded는 상한이 낮으면(125) 한가할 때 올림 때문에 상한이 1이 되어 자주 넘기고 캐시를 잃는다. 200에서 넘침이 가장 적고 균등 분포 p50이 hash와 같다.
  - p99는 rr보다 낫다고 할 수 없다 (실행당 요청 약 55개라 p99 ≈ 최댓값, 3회 편차 큼). 처리 능력 탐색(속도마다 1회)에서는 1.2 req/s에서 bounded 125가 완료 0.44 req/s로 rr 0.31·hash 0.39보다 높았다.
  - fake_llm의 처리 시간은 Ollama llama3.2:1b(M4)로 보정한 입력값이라 절대값이 아니라 상대 비교다.
  - 위 수치는 통합 전 용민 구현(4단계·이벤트 루프, #45) 위에서 쟀다. 선택 규칙과 상한 계산은 같고 처리 중 수를 세는 시점(연결 예약~반납)도 같지만, 통합본에서는 다시 재지 않았다.

## D-046 포워드 프록시는 `route … forward`로 켠다
- 상태: 제안
- 날짜: 2026-11-23 (ririzu 님 원안), 2026-11-25 (통합본에 맞춰 옮김)
- 맥락: 과제 README는 Forward·Reverse 중 하나 이상을 요구한다. 리버스 프록시는 구현돼 있고, 포워드 프록시는 기획서 범위 밖이라 테스트 ID도 없다. 목적지가 요청마다 달라서 설정 시점에 백엔드를 만들 수 없다. D-043의 loopback 한정 포워드는 운영자가 정한 단일 포트만 연다.
- 결정: 설정 `route <prefix> forward`로 켠다(기본 꺼짐, 열린 프록시는 내부망 접근 위험이 있다). absolute-form 요청(`GET http://host[:port]/…`)만 받고 origin-form이면 400. 목적지 백엔드는 `upstream_forward_backend`가 처음 볼 때 이름을 해석해 `"@forward"` 그룹(`CFG_FORWARD_UPSTREAM`)에 더하고, 같은 목적지는 유휴 연결 풀을 함께 쓴다. 목적지 상한은 그룹 배열 크기(`CFG_MAX_BACKENDS`)다. 목적지가 하나라 다른 백엔드로 재시도하지 않는다(실패 502, 응답 지연 504). conn의 `setup_proxy_job`이 목적지를 정하고 relay가 첫 시도에 그 백엔드를 예약한다. 전송·relay·v1·v2 경로는 리버스 프록시와 같다. forward route가 없으면 D-043의 동작은 그대로다.
- 대안: 별도 포워드 전용 연결 경로 (relay·풀·v2 처리를 다시 만들어야 함), 기본으로 켬 (열린 프록시), 목적지 동적 배열 (원안은 256개. 통합본 그룹 구조는 고정 배열이라 그 상한을 그대로 쓴다).
- 영향: `core/config.{h,c}`(`ROUTE_FORWARD`, `CFG_FORWARD_UPSTREAM`), `router/router.c`, `handler/{plan,proxy}.c`, `upstream/{upstream.h,backend.c,relay.c}`, `downstream/conn.c`. 이름 해석은 블로킹이라 v2에서는 처음 보는 목적지마다 루프가 잠깐 멈춘다. IPv6 리터럴 미지원. 테스트 E-FW-01은 기획서 밖 ID다.

## D-047 포워드 프록시의 응답 캐시
- 상태: 제안
- 날짜: 2026-11-24 (ririzu 님 원안), 2026-11-25 (통합본에 맞춰 옮김)
- 맥락: 과제 README는 포워드 프록시의 캐싱을 선택 항목으로 둔다. D-046으로 포워드 프록시가 생겼고 기획서에는 캐시 규칙이 없다. 응답 본문은 conn이 업스트림 버퍼에서 클라이언트로 바로 흘리므로(복사 없음), 캐시는 그 바이트를 따로 모아야 한다.
- 결정: (1) 포워드 경로(`route … forward`)의 GET만 다룬다. 리버스 프록시·정적·CGI는 캐시하지 않는다. (2) 저장 조건: 상태 200, Content-Length로 끝이 정해진 응답, 본문이 `cache_entry_max` 이하, 신선도가 있음(`s-maxage` → `max-age`, `Age`만큼 뺌, 상한 하루). `no-store`·`no-cache`·`private`·`Vary`·`Set-Cookie`가 있으면 저장하지 않는다. `Expires`와 휴리스틱 신선도는 쓰지 않는다. (3) 요청에 `Authorization`·`Range`·`no-cache`·`no-store`(또는 `Pragma: no-cache`)가 있으면 저장본을 쓰지 않고, `Authorization`·`Range`·`no-store`면 응답도 저장하지 않는다. (4) 같은 URL로 POST가 오면 그 항목을 지운다. (5) 적중 응답은 `Age`와 `X-Cache: HIT`를, 원본까지 간 포워드 GET은 `X-Cache: MISS`를 붙인다. (6) 저장소는 프로세스 하나에 하나, 뮤텍스로 보호하고 총 바이트 상한(`cache_max_bytes`, 기본 8 MiB, 0이면 끔)을 넘으면 가장 오래 안 쓴 항목부터 지운다. 키는 absolute-form 대상(스킴·호스트는 소문자). (7) 조회는 conn의 `setup_proxy_job`이 첫 시도 전에 `proxy_cache_begin`으로 하고(적중이면 이름 해석·연결 없이 응답), 복사는 conn의 업스트림 본문 전달 두 곳(v1 `upstream_send_body`, v2 `nb_upstream_body`)에서 `proxy_body_tee`·`proxy_body_done`으로 한다.
- 대안: 캐시 미스 때 handler가 원본에서 블로킹으로 통째로 받아 오기 (v2 루프가 응답 시간만큼 멈춤), 캐시를 서버 시작 때 `upstream` 그룹에 묶기 (handler가 upstream 내부 상태를 알아야 함), 해시 표 (항목이 수백 개인 규모에서는 연결 리스트로 충분하다. 코드에 `shortcut:` 주석).
- 영향: `handler/{cache.c,internal_cache.h,proxy.c,handler.h}`, `downstream/conn.c`, `common/types.h`(`proxy_job_t.fill`), `core/config.{h,c}`(`cache_max_bytes`, `cache_entry_max`). E-FW-02~E-FW-07 (두 엔진).
