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
- 상태: 폐기 (D-036로 대체)
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

## D-036 재사용 연결이 응답 전에 끊기면 멱등한 요청만 다시 보낸다
- 상태: 제안
- 날짜: 2026-11-23
- 맥락: D-011은 응답 바이트를 받기 전 EOF면 메서드와 상관없이 새 연결로 다시 보냈다. 이 시점에는 요청을 이미 다 보냈으므로 백엔드가 처리한 뒤 끊었을 수 있다. POST를 다시 보내면 같은 일이 두 번 실행되고, `proxy_on_fail`의 "보낸 POST는 재시도하지 않는다" 규칙과도 어긋난다. RFC 9112 §9.3.1은 자동 재시도를 멱등한 요청으로 제한한다.
- 결정: 송신 단계에서 아무것도 보내지 못한 실패는 D-011대로 메서드와 상관없이 1회 새 연결. 응답을 기다리다 끊긴 경우는 GET·HEAD일 때만 1회 새 연결하고, 그 외는 `PFAIL_BAD_RESPONSE`(502)로 끝낸다.
- 대안: D-011 유지.
- 영향: `src/upstream/relay.c` `relay_step`. E-PX-14(GET만 사용)는 영향 없음.

## D-037 공개 헤더의 타입 주석에 "역할" 줄을 더한다
- 상태: 제안
- 날짜: 2026-11-23
- 맥락: 구현 중 `common/types.h`와 `downstream/conn.h`의 타입 설명에 그 타입이 전체 흐름에서 맡는 일을 적은 `역할:` 줄을 추가했다. 공개 헤더 변경은 기록하라는 규칙(AGENTS.md)에 따라 남긴다.
- 결정: 주석 한 줄 추가만 허용한다. 선언, 필드, 열거값, 함수 시그니처는 바꾸지 않는다.
- 대안: 각 디렉토리 README에만 설명을 둔다.
- 영향: `src/common/types.h`, `src/downstream/conn.h` (주석만). 컴파일 결과와 테스트 ID 영향 없음.

## D-038 upstream 내부 상태는 `internal_state.h`에 둔다
- 상태: 제안
- 날짜: 2026-11-23
- 맥락: `backend.c`, `lb.c`, `pool.c`가 `struct backend`, `struct upstream_group`, 유휴 연결 목록을 함께 쓴다. 공개 헤더 `upstream.h`는 이 구조체를 불투명 타입으로만 선언한다.
- 결정: 공유 정의를 `src/upstream/internal_state.h`에 두고 upstream 디렉토리 안의 .c에서만 include한다. 공개 헤더는 바꾸지 않는다.
- 대안: 정의를 `upstream.h`에 공개 (다른 디렉토리가 내부 필드에 의존하게 됨), 세 파일을 하나로 합침.
- 영향: `src/upstream/internal_state.h` (새 파일), `backend.c`, `lb.c`, `pool.c`.

## D-039 plan_init은 union에서 fd의 -1을 지킨다
- 상태: 제안
- 날짜: 2026-11-23
- 맥락: `plan_init` 명세는 fd들의 -1과 `upstream.conn`의 NULL을 함께 요구하지만 `body`는 union이라 둘이 같은 저장 공간을 쓴다. 구현이 fd를 -1로 쓴 뒤 `conn = NULL`을 써서 `file.fd`, `pipe.out_fd`, `pipe.in_fd`가 0(stdin)이 되었다.
- 결정: `plan_init`은 fd 쪽 기본값(-1)을 지키고 `conn = NULL`을 쓰지 않는다. `upstream.conn`은 BODY_UPSTREAM으로 바꿀 때 처리기가 채운다(`proxy_on_head`가 이미 그렇게 한다). feat/stage1-sohee의 D-034(제안)와 같은 방향이다.
- 대안: 본문 종류별 생성 함수 추가, union을 struct로 바꿈 (공개 구조체 변경).
- 영향: `src/handler/plan.c`, `src/handler/plan.h` (주석). U-HD-01.

## D-040 v2 이벤트 루프의 연결·추가 fd 처리
- 상태: 제안
- 날짜: 2026-11-23
- 맥락: 7단계(선택) v2를 구현하면서 engine.h·conn.h가 정하지 않은 운영 규칙이 필요했다. 연결이 무엇을 기다릴지 엔진에 알리는 방법, CGI·프록시를 기다리는 동안 클라이언트 fd를 어떻게 감시할지, relay가 연결을 새로 열 때의 등록 처리다.
- 결정: (1) 엔진은 accept한 fd를 EV_READ로 처음 등록하고, 그 뒤 감시 이벤트는 app 콜백이 conn 상태(`CONN_WANT_READ`/`WRITE`/`WAIT_FD`)를 보고 `engine_watch`로 바꾼다. (2) CGI·프록시를 기다리는 동안 클라이언트는 읽기를 감시해 끊김(EOF)을 알아채고, 다음 요청 바이트가 미리 오면 그 응답이 끝날 때까지 읽기 감시를 멈춘다(레벨 트리거 반복 방지). (3) relay가 실패 후 새 연결을 열면 같은 fd 번호가 재사용될 수 있어, `relay_step`마다 등록을 풀고 다시 건다. 닫힌 fd의 `poller_del`은 기록만 지운다. (4) 업스트림 본문은 업스트림 rbuf에서 바로 보내고(복사 없음), 클라이언트가 막히면 출처 읽기를 멈춘다. (5) 엔진 내부 상태는 `src/engine/internal_loop.h`에 둔다(README가 정한 이름).
- 대안: CGI·프록시를 v2에서도 블로킹 도우미 스레드로 넘김 (단일 루프 스레드 원칙과 어긋남), 엔진이 연결의 읽기·쓰기 요구를 직접 추측함 (콜백 반환값에 정보가 없음).
- 영향: `src/engine/engine_loop.c`, `src/engine/engine_pool.c`, `src/engine/internal_loop.h`, `src/engine/poller_epoll.c`, `src/engine/poller_kqueue.c`, `src/app/app.c`, `src/downstream/conn.c`. U-EN-02(두 엔진), E-* 68개(`HTTPD_ENGINE=loop`).

## D-041 포워드 프록시는 `route … forward`로 켠다
- 상태: 제안
- 날짜: 2026-11-23
- 맥락: 과제 README는 Forward·Reverse 중 하나 이상을 요구한다. 리버스 프록시는 구현돼 있고, 포워드 프록시는 기획서 범위 밖이라 테스트 ID도 없다. 목적지가 요청마다 달라서 설정 시점에 백엔드를 만들 수 없다.
- 결정: 설정 `route <prefix> forward`로 켠다(기본 꺼짐, 열린 프록시는 내부망 접근 위험이 있다). absolute-form 요청(`GET http://host[:port]/…`)만 받고 origin-form이면 400. 목적지 백엔드는 `upstream_forward_backend`가 처음 볼 때 이름을 해석해 만들고(최대 256개), 같은 목적지는 keep-alive 풀을 함께 쓴다. 목적지가 하나라 다른 백엔드로 재시도하지 않는다(실패 502, 응답 지연 504). 전송·relay·v1·v2 경로는 리버스 프록시와 같다. 공개 헤더 변경: `core/config.h`에 `ROUTE_FORWARD`·`CFG_FORWARD_UPSTREAM`, `upstream/upstream.h`에 `upstream_forward_backend`.
- 대안: 별도 포워드 전용 연결 경로 (relay·풀·v2 처리를 다시 만들어야 함), 기본으로 켬 (열린 프록시).
- 영향: `src/core/config.{h,c}`, `src/router/router.c`, `src/handler/{plan,proxy}.c`, `src/upstream/{upstream.h,backend.c,internal_state.h}`. 이름 해석은 블로킹이라 v2에서는 처음 보는 목적지마다 루프가 잠깐 멈춘다. IPv6 리터럴 미지원. 테스트 E-FW-01은 기획서 밖 ID다.

## D-042 포워드 프록시의 응답 캐시
- 상태: 제안
- 날짜: 2026-11-24
- 맥락: 과제 README는 포워드 프록시의 캐싱을 선택 항목으로 둔다. D-041로 포워드 프록시가 생겼고 기획서에는 캐시 규칙이 없다. 응답 본문은 conn이 업스트림 버퍼에서 클라이언트로 바로 흘리므로(복사 없음), 캐시는 그 바이트를 따로 모아야 한다.
- 결정: (1) 포워드 경로(`route … forward`)의 GET만 다룬다. 리버스 프록시·정적·CGI는 캐시하지 않는다. (2) 저장 조건: 상태 200, Content-Length로 끝이 정해진 응답, 본문이 `cache_entry_max` 이하, 신선도가 있음(`s-maxage` → `max-age`, `Age`만큼 뺌, 상한 하루). `no-store`·`no-cache`·`private`·`Vary`·`Set-Cookie`가 있으면 저장하지 않는다. `Expires`와 휴리스틱 신선도는 쓰지 않는다. (3) 요청에 `Authorization`·`Range`·`no-cache`·`no-store`(또는 `Pragma: no-cache`)가 있으면 저장본을 쓰지 않고, `Authorization`·`Range`·`no-store`면 응답도 저장하지 않는다. (4) 같은 URL로 POST가 오면 그 항목을 지운다. (5) 적중 응답은 `Age`와 `X-Cache: HIT`를, 원본까지 간 포워드 GET은 `X-Cache: MISS`를 붙인다. (6) 저장소는 프로세스 하나에 하나, 뮤텍스로 보호하고 총 바이트 상한(`cache_max_bytes`, 기본 8 MiB, 0이면 끔)을 넘으면 가장 오래 안 쓴 항목부터 지운다. 키는 absolute-form 대상(스킴·호스트는 소문자). (7) 복사 지점은 conn의 업스트림 본문 전달 두 곳(v1 `send_upstream_body`, v2 `upstream_pull`)이고 `proxy_body_tee`·`proxy_body_done`을 부른다.
- 대안: 캐시 미스 때 handler가 원본에서 블로킹으로 통째로 받아 오기 (v2 루프가 응답 시간만큼 멈춤), 캐시를 서버 시작 때 `upstream` 그룹에 묶기 (handler가 upstream 내부 상태를 알아야 함), 해시 표 (항목이 수백 개인 규모에서는 연결 리스트로 충분하다. 코드에 `shortcut:` 주석).
- 영향: `src/handler/{cache.c,internal_cache.h,proxy.c,handler.h}`, `src/downstream/conn.c`, `src/common/types.h`(`proxy_job_t.fill`), `src/core/config.{h,c}`(`cache_max_bytes`, `cache_entry_max`). E-FW-02~E-FW-07 (두 엔진).

## D-043 요청이 끝나면 연결 버퍼의 용량을 줄인다
- 상태: 제안
- 날짜: 2026-11-24
- 맥락: `buf_consume`·`buf_reset`은 길이만 줄이고 용량은 그대로 둔다(buf.h 명세). 연결의 읽기 버퍼는 요청 본문 상한(기본 10 MiB)까지 커질 수 있고, 2배씩 늘어 본문보다 큰 용량이 잡힌다. 큰 본문을 한 번 받은 keep-alive 연결이 그 메모리를 연결이 닫힐 때까지 쥔다. 측정(2026-11-24, WSL, 연결 30개가 각각 CGI에 2 MiB POST 뒤 연결 유지): pool RSS 3,476 KB → 128,572 KB, 연결당 약 4 MB.
- 결정: `buf_shrink(b, keep)`를 추가한다. 용량이 max(len, keep)보다 크면 그 크기로 realloc하고, 아니면 아무 일도 없다 (내용·len·max 유지, 실패하면 그대로). conn은 요청 하나를 끝낸 직후(`finish_request`에서 계획·요청 뷰를 비우고 `buf_consume`한 뒤) 읽기·쓰기·pipe 버퍼에 `CONN_KEEP_BUF`(64 KiB)로 부른다. realloc으로 주소가 바뀌므로 이 시점에는 이 버퍼를 빌린 `str_t`가 남아 있으면 안 된다.
- 대안: 연결을 닫을 때만 해제 (현재 동작, keep-alive 연결이 메모리를 쥠), 2배 확장 대신 필요한 만큼만 확장 (재할당이 잦아짐), 요청마다 버퍼를 새로 만들고 해제 (할당 횟수 증가).
- 영향: `src/core/buf.{h,c}`, `src/downstream/conn.c`. 측정(같은 조건): pool 128,572 KB → 66,992 KB, loop 13,552 KB → 7,552 KB. pool에 남은 값은 glibc가 스레드마다 쥐는 메모리 영역의 보유분이다 (`MALLOC_ARENA_MAX=1`이면 8,784 KB). 성질 검사 `tests/robust` props (buf).

## D-044 v2는 읽기 반복이 끝난 뒤 읽지 않고 한 번 더 파싱한다
- 상태: 제안
- 날짜: 2026-11-24
- 맥락: v2 `conn_on_event`는 한 이벤트에서 한 연결을 최대 8번 읽는다(공정성). 읽기 한 번이 반복 한 단계를 쓰므로, 8번째 읽기에서 요청의 마지막 바이트를 받으면 완성된 요청이 읽기 버퍼에 있는데도 파싱하지 않고 `CONN_WANT_READ`를 돌려줬다. 커널 큐는 비어 있어 새 이벤트가 오지 않고, 서버는 요청이 덜 왔다고 믿은 채 본문 마감(30초) 뒤 408을 냈다. 2 MiB POST에서 20번 중 2~6번 재현됐고(WSL, 멈춘 순간 서버는 `epoll_wait` 대기, CGI 파이프·자식 없음, 클라이언트 소켓만 EPOLLIN 등록, 양쪽 소켓 큐 0), 1 MiB에서는 40번 중 0번이었다. v1(스레드 풀)은 영향이 없다.
- 결정: 반복을 `CONN_READS_PER_EVENT`(8)번 + 1번으로 하고, 마지막 한 번은 `events`에서 `EV_READ`를 빼서 읽지 않고 파싱만 한다. 요청이 완성돼 있으면 처리하고, 아니면 `CONN_WANT_READ`를 돌려준다 (커널에 데이터가 남았다면 레벨 트리거로 다음 이벤트가 온다).
- 대안: 반복 상한을 없애 EAGAIN까지 읽기 (한 연결이 큰 업로드로 루프를 오래 점유), 8번 읽은 뒤 스스로 다시 이벤트를 만들기 (engine에 새 인터페이스 필요).
- 영향: `src/downstream/conn.c`. 회귀 테스트 `tests/robust` R-25 (수정 전 loop 3/3 실패, 수정 후 pool·loop 모두 통과).

## D-045 develop의 계약 테스트가 요구하는 동작을 이 구현에도 맞춘다
- 상태: 제안
- 날짜: 2026-11-24
- 맥락: `origin/develop`은 core, common, downstream을 별도로 구현해 병합했고 단위 테스트 5개(U-COM-21, U-COM-33, U-CORE-19, U-CORE-26, U-CORE-31)를 추가했다. 이 구현을 그 테스트에 돌리자 5개가 실패했다.
- 결정: 구현을 바꾸지 않고 실패한 동작만 맞춘다. (1) `Transfer-Encoding`에서 `chunked`가 두 번 나오면 거부하고, 따옴표 안의 쉼표로 목록을 나누지 않는다. (2) chunk extension은 `;이름[=토큰|따옴표 문자열]` 문법을 지키고 빈 확장·빈 값을 거부한다. (3) 설정 오류는 `파일:줄: undefined upstream '이름'`, `파일:줄: unknown directive '이름'` 형식이다 (`route_conf_t.line` 추가). (4) 오류 로그 본문의 CR/LF는 `\xHH`로 바꾼다. (5) Linux `sendfile`이 만드는 `SIGPIPE`는 호출 동안 이 스레드에서 막고 `sigtimedwait`로 거둔다. 추가로 develop의 `D-035`에 맞춰, 머리 한도(`max_head`)를 넘긴 시작 줄에 NUL이 있으면 응답 없이 닫는다 (R-26).
- 대안: develop 구현으로 교체 (v2 엔진, 포워드 프록시, 캐시와 맞물린 부분을 다시 맞춰야 함).
- 영향: `src/common/http_framing.c`, `src/core/config.{c,h}`, `src/core/log.c`, `src/core/platform.c`, `src/downstream/conn.c`. develop의 단위 테스트 5개 통과. macOS 경로는 확인하지 않았다.
