# httpd

POSIX 소켓만으로 만드는 HTTP/1.1 웹서버 겸 리버스 프록시 (C11, 외부 라이브러리 없음). 정적 파일, CGI, 라운드로빈 리버스 프록시를 제공한다.

> 현재 상태: **인터페이스·테스트 확정본 (`v0.1-interfaces`)**. 모든 공개 함수는 스텁이고 테스트는 `tests/pending.txt`에 PENDING으로 올라가 있다. 구현은 디렉토리 단위로 진행한다.

## 빌드·실행

```sh
make                                              # build/httpd
./build/httpd -p 8080 -r tests/fixtures/www       # 포트와 정적 루트를 CLI로
./build/httpd -c my.conf                          # 설정 파일 (문법: src/core/config.h 주석)
./build/httpd --help
```

CLI 옵션: `-p <port>`, `-r <root>`, `-c <file>`, `-w <workers>`, `-e pool|loop`, `-h`. 설정 파일 예시는 `tests/fixtures/conf/`에 있다. 종료 코드: 0 정상, 1 설정 오류, 2 기동 실패.

## 테스트

```sh
make test               # 계약 최소셋(C) + 기능 테스트(E2E), 마지막 줄에 합계
make unit               # C 계약 최소셋만
make e2e                # E2E만 (서버와 fake 백엔드를 띄움)
make test ONLY=E-PX     # ID 접두사로 거르기
make lint               # include 방향, 금지 함수, 헤더 검사, clang-format (18.1.8 고정, D-030)
make asan               # ASan + UBSan 빌드로 make test
```

PENDING은 실패가 아니다. 테스트 층, ID, pending 규칙은 [docs/TESTING.md](docs/TESTING.md).

## 디렉토리

| 디렉토리 | 한 줄 설명 |
| --- | --- |
| [src/core/](src/core/README.md) | 기반: 문자열 뷰, 버퍼, 로그, 설정, 플랫폼 차이 |
| [src/common/](src/common/README.md) | 경계 타입과 양쪽 면 공용 HTTP 규칙 (헤더, 본문 길이, keep-alive, 상태 문구) |
| [src/downstream/](src/downstream/README.md) | 클라이언트 쪽: 대기 소켓, 연결 상태, 요청 파싱, 응답 직렬화 |
| [src/router/](src/router/README.md) | 경로·Host 기반 라우팅 |
| [src/handler/](src/handler/README.md) | 응답 계획: 정적 파일, CGI, 프록시 판단 |
| [src/upstream/](src/upstream/README.md) | 백엔드 쪽: 레지스트리·LB·연결 풀, 업스트림 요청·응답, 프록시 I/O |
| [src/engine/](src/engine/README.md) | 동시성 엔진: 스레드 풀(v1), 이벤트 루프(v2), poller |
| [src/app/](src/app/README.md) | 조립과 main |
| `tests/` | 테스트 하네스, fake, fixture, 테스트 코드 (`tests/README.md`) |

## 문서

- [AGENTS.md](AGENTS.md) — 사람과 에이전트가 처음 읽는 작업 규칙
- [docs/CONVENTIONS.md](docs/CONVENTIONS.md) — 인터페이스 규약 (이름, 반환값, 소유권, include 방향, 주석 템플릿)
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — 영역과 요청 흐름
- [docs/TESTING.md](docs/TESTING.md) — 테스트 층, ID, pending 방식, 러너
- [docs/DECISIONS.md](docs/DECISIONS.md) — 결정 사항과 미결 목록
- 기획서 「C 웹서버 + 리버스 프록시 기획서 및 테스트 명세」 — 저장소에 넣지 않는다. 팀 공유 위치: <!-- TODO: 기획서 링크 --> (테스트 ID의 원래 표는 기획서 7~14장)

## 협업 규칙

- 브랜치: `feat/<dir>-<주제>`, `fix/<dir>-<주제>`. 대상은 항상 `develop`, 머지는 squash. `main`은 릴리스용.
- `docs/CONVENTIONS.md`, `src/common/`, 공개 헤더(`src/**/*.h`)를 바꾸는 PR은 **세 명 모두** 승인해야 한다 (절차: CONVENTIONS.md 10절).
- 커밋 메시지는 Conventional Commits, scope는 디렉토리 이름. 본문에 통과시킨 테스트 ID.

## 진행 상황

통과 현황은 `tests/pending.txt`에서 지워진 항목으로 추적한다. 남은 ID 수는 `grep -cvE '^[[:space:]]*(#|$)' tests/pending.txt`.

## 부록: 과제 요구사항

### 요구사항

#### 과제 개요

- C 언어로 동작하는 웹 서버를 구현합니다.
- 외부 HTTP 라이브러리나 프레임워크 없이 POSIX 소켓 API만 사용하여 처음부터 직접 작성합니다.
- HTTP 서버와 함께 Proxy 또는 Reverse Proxy 기능을 구현합니다.

#### 필수 구현 항목

- **TCP 소켓 서버**
  - `socket()` → `bind()` → `listen()` → `accept()`의 표준 흐름을 직접 구현합니다.
  - 클라이언트 연결을 수락하고 요청과 응답을 주고받습니다.
- **HTTP 요청 파싱**
  - 요청 라인에서 메서드, URI, HTTP 버전을 파싱합니다.
  - `Host`, `Content-Length`, `Connection` 등의 헤더를 파싱합니다.
  - `POST` 요청의 본문을 처리합니다.
  - 최소 지원 메서드
    - `GET`
    - `POST`
- **HTTP 응답 생성**
  - `HTTP/1.1 200 OK`와 같은 상태 라인을 생성합니다.
  - `Content-Type`, `Content-Length`, `Connection` 등의 표준 헤더를 포함합니다.
  - 응답 본문을 송신합니다.
- **정적 파일 서빙**
  - 지정된 루트 디렉토리에서 파일을 읽어 응답합니다.
  - HTML, CSS, JS, PNG, JPG 등의 파일 형식에 MIME 타입을 매핑합니다.
  - `404`, `403`, `500` 등의 기본 오류 응답을 처리합니다.
- **동적 요청 처리**
  - 요청에 따라 애플리케이션 로직을 실행하고 응답을 생성합니다.
  - 예를 들어 CGI 프로그램을 실행하거나 백엔드 서버로 요청을 전달합니다.
- **동시 요청 처리**
  - 다음 방식 중 하나 이상을 구현합니다.
    - 멀티프로세스 (`fork()`)
    - 멀티스레드 (`pthread`, 요청 처리 스레드 풀 권장)
    - I/O 멀티플렉싱 (`select()`, `poll()`, `epoll()`, `kqueue()`)
  - I/O 멀티플렉싱 방식을 선택하면 이벤트 루프를 통해 여러 연결의 I/O를 관리합니다.

#### Proxy 또는 Reverse Proxy 구현 (필수)

두 방식 중 최소 한 가지를 구현합니다. 두 가지 모두 구현해도 좋습니다.

- **Forward Proxy**
  - 클라이언트의 HTTP 요청을 외부 서버로 대신 전달하고 응답을 그대로 돌려줍니다.
  - 요청 흐름은 클라이언트 → Proxy → 원 서버입니다.
  - 요청 URL에서 호스트를 파악하여 새 TCP 연결을 만들고 데이터를 중계합니다.
  - 캐싱 기능은 선택 사항입니다.
- **Reverse Proxy**
  - 외부 요청을 받아 여러 내부 백엔드 서버로 라우팅합니다.
  - 요청 흐름은 클라이언트 → Reverse Proxy → 백엔드(1~N)입니다.
  - 가능하면 Round-Robin과 같은 단순 로드 밸런싱을 포함합니다.
  - 라우팅 규칙은 경로(예를 들어 `/api/*` → `8001`, `/static/*` → `8002`)나 `Host` 헤더를 기준으로 설계합니다.
  - 정적 요청을 프록시가 직접 서빙할지, 백엔드로 전달할지는 구현자가 선택합니다.

#### 기술 스택 및 제한

- **언어**는 C를 사용합니다.
- 표준 C 라이브러리와 POSIX API(`<sys/socket.h>`, `<netinet/in.h>`, `<unistd.h>`, `<pthread.h>` 등)를 사용할 수 있습니다. libevent, libuv, nginx, Apache, mongoose 등 완성된 HTTP 서버나 이벤트 루프 라이브러리는 사용할 수 없습니다.
- **빌드**를 위해 `Makefile` 또는 `CMakeLists.txt`를 제공합니다.
- **실행** 시 포트 번호와 정적 파일 루트를 CLI 인자 또는 설정 파일로 받습니다. Reverse Proxy를 구현한 경우 라우팅 규칙도 같은 방식으로 받습니다.

#### 개발 환경

- POSIX 소켓 API를 사용하므로 macOS와 Linux에서 개발할 수 있습니다.
- Windows를 사용하는 학생은 Ubuntu를 설치한 WSL2(Windows Subsystem for Linux), Docker 또는 클라우드 환경 중 하나에서 개발해야 합니다.
- 같은 소스 코드를 macOS, WSL, Linux에서 빌드하고 실행할 경우 OS별 라이브러리와 함수의 차이를 고려합니다. POSIX 표준 범위 안에서 구현하거나 `#ifdef`로 플랫폼별 분기를 처리합니다.

### 중점 포인트

- **소켓 프로그래밍의 기본 흐름**
  - TCP 3-way handshake, `SO_REUSEADDR`, 블로킹과 논블로킹의 차이
  - `bind()` 실패와 `accept()` 블로킹 등의 동작 원리
- **HTTP 프로토콜 명세(RFC 7230~7235)의 최소 구현 범위**
  - 요청 라인, 헤더, 본문의 경계 식별
  - `Content-Length`를 기준으로 본문 길이 처리
  - `Connection: keep-alive`의 의미와 처리
- **동시성 모델 선택의 트레이드오프**
  - 요청당 프로세스, 스레드 풀, 이벤트 루프 비교
  - C10K 문제와 관련된 설계 고려 사항
- **Proxy와 Reverse Proxy의 차이**
  - 누구의 입장에서 누구를 대신하는가
  - 보안, 로드 밸런싱, 캐싱 측면의 활용
- **메모리 관리**
  - `malloc()`과 `free()`를 짝지어 호출하고 버퍼 오버플로우 방지
  - 클라이언트 연결 끊김이나 비정상 종료 시 자원 회수
- **시그널 및 종료 처리**
  - `SIGINT`와 `SIGTERM` 수신 시 안전하게 종료
  - 쓰기 도중 클라이언트 연결이 끊기는 경우에 대비해 `SIGPIPE` 무시

### 품질

- **단위 테스트**
  - HTTP 파서, MIME 매핑, 라우팅 및 프록시 전달 로직 등 각 모듈을 검증합니다.
- **기능 테스트**
  - `curl`, 브라우저 등 실제 HTTP 클라이언트로 정적 파일 응답과 프록시 동작을 확인합니다.
  - 여러 클라이언트가 동시에 접속해도 정상적으로 동작하는지 확인합니다.
- **부하 테스트**
  - `ab`(Apache Bench), `wrk`, `hey` 등으로 동시 요청 수에 따른 처리량을 측정합니다.
- **메모리 누수 검사**
  - `valgrind` 또는 컴파일러 sanitizer(`-fsanitize=address,leak` 등)로 메모리 누수와 잘못된 메모리 접근을 검사합니다.
- **엣지 케이스**
  - 불완전한 헤더, 비정상 메서드, 잘못된 버전 등 잘못된 HTTP 요청
  - 매우 큰 요청 본문이나 매우 긴 URI
  - 백엔드 서버의 응답이 없거나 타임아웃이 발생하는 경우(Reverse Proxy)
  - 클라이언트의 갑작스러운 연결 종료
- 이력서와 포트폴리오에 포함할 수 있을 만큼 완성도 높게 구현합니다.
