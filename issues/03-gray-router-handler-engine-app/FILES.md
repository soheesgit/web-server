# 담당 파일

이 이슈의 범위와 현재 구현 파일을 연결한 목록이다. 프로젝트의 include 경로와 모듈 규칙을 유지하기 위해 소스 파일은 `src/`의 기존 구조에 둔다.

## GRAY: router + handler + engine + app

- 라우팅: `src/router/router.c`, `src/router/router.h`
- 응답 계획·MIME·STATIC·CGI·PROXY 판단: `src/handler/plan.c`, `src/handler/mime.c`, `src/handler/static.c`, `src/handler/cgi.c`, `src/handler/proxy.c`
- 동시성 엔진 v1: `src/engine/engine_pool.c`
- poller backend: `src/engine/poller_epoll.c`, `src/engine/poller_kqueue.c`, `src/engine/poller_poll.c`
- app 조립·실행: `src/app/app.c`, `src/app/main.c`
- 모듈 진행 상태: 각 디렉토리의 `README.md`

선택 단계인 v2 event loop는 구현되지 않았다. `ENGINE_EVENT_LOOP`, `engine_watch`, `engine_unwatch`는 현재 `ENOTSUP`을 반환한다.
Docker Linux 환경에서 v1 pool 기준 빌드·린트·단위·필수 E2E를 실행했다. v2 전체 동작,
ASan 최종 결과, macOS 빌드는 미확인이다.
