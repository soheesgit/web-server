/* src/handler/plan.h
 *
 * 응답 계획(response_plan_t) 초기화·메모리 본문·에러 계획·헤더 추가·정리.
 * 디렉토리 책임과 관련 테스트: src/handler/README.md
 */
#ifndef HTTPD_HANDLER_PLAN_H
#define HTTPD_HANDLER_PLAN_H

#include <stddef.h>

#include "common/types.h"

/**
 * 계획을 빈 상태로 초기화한다.
 *
 * 모든 바이트를 0으로 지운 뒤: kind BODY_NONE, head.status 0, head.framing NONE, close false,
 * head_pending false, 모든 fd -1(file.fd, pipe.out_fd, pipe.in_fd), pipe.pid 0,
 * pipe.deadline_ms -1, upstream.conn NULL. head.fields는 0으로 채운 buf_t(아직 할당 없음)이며
 * plan_add_header 등이 처음 쓸 때 buf_init한다. 이전 내용은 해제하지 않는다.
 *
 * @param p  계획. NULL 불가
 *
 * 에러:    없음
 * 소유권:  할당 없음
 * 스레드:  [per-object]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-HD-02, U-HD-04 · 추후 U-HD-01
 */
void plan_init(response_plan_t *p);

/**
 * 메모리 본문 계획으로 만든다.
 *
 * 기존 자원을 plan_cleanup으로 정리한 뒤 kind BODY_MEM, head.status = status,
 * framing LENGTH n, body를 복사한다. content_type이 NULL이 아니면 "Content-Type: …" 헤더를 추가.
 * 실패하면 p는 plan_init 상태.
 *
 * @param p             계획. NULL 불가
 * @param status        상태 코드 (100~599)
 * @param content_type  Content-Type 값. NULL 허용 (헤더 없음)
 * @param body          본문. n이 0이면 NULL 허용
 * @param n             본문 바이트 수
 * @return              0 성공, -1 실패
 *
 * 에러:    ENOMEM, EINVAL (status 범위)
 * 소유권:  body를 복사한다. 계획이 사본을 소유
 * 스레드:  [per-object]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  (테스트 없음)
 */
int plan_set_mem(response_plan_t *p, int status, const char *content_type, const void *body,
                 size_t n);

/**
 * 에러 계획으로 바꾼다.
 *
 * 기존 자원(파일 fd, pipe·자식, 업스트림 연결, 헤더)을 plan_cleanup으로 정리한 뒤
 * http_error_body로 본문을 만들어 BODY_MEM, Content-Type "text/html; charset=utf-8",
 * framing LENGTH = 본문 길이로 둔다. close는 정리 전 값을 유지한다.
 * 405를 만들 때 Allow 헤더는 호출자가 plan_add_header로 덧붙인다.
 *
 * @param p       계획. NULL 불가. plan_init된 상태여야 함
 * @param status  400~599
 * @return        0 성공, -1 실패 (p는 plan_init 상태, status만 설정됨)
 *
 * 에러:    ENOMEM, EINVAL (status 범위)
 * 소유권:  기존 자원을 해제하고 새 본문을 소유
 * 스레드:  [per-object]
 * I/O:     close, kill, waitpid (정리할 자원이 있을 때)
 * 블로킹:  waitpid (SIGKILL 직후)
 * 테스트:  U-HD-02, E-ST-04 · 추후 U-HD-03
 */
int plan_error(response_plan_t *p, int status);

/**
 * 응답 헤더 한 줄을 head.fields 끝에 추가한다: "<name>: <value>\r\n".
 *
 * fields가 아직 할당되지 않았으면 buf_init(…, 256, 64 KiB)한다. name·value에 CR·LF가 있으면
 * 거부한다(헤더 주입 방지). Date·Server·Content-Length·Connection·Transfer-Encoding은
 * writer가 붙이므로 넣지 않는다(넣으려 하면 -1 + EINVAL).
 *
 * @param p      계획. NULL 불가
 * @param name   헤더 이름, NUL 종료. NULL 불가
 * @param value  헤더 값, NUL 종료. NULL 불가
 * @return       0 성공, -1 실패 (fields 불변)
 *
 * 에러:    EINVAL (CR·LF, writer 전용 헤더), EMSGSIZE·ENOMEM
 * 소유권:  name·value를 복사
 * 스레드:  [per-object]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-HD-11 (static_handle을 통해)
 */
int plan_add_header(response_plan_t *p, const char *name, const char *value);

/**
 * 계획이 가진 자원을 모두 회수하고 plan_init 상태로 되돌린다.
 *
 * 멱등이며 NULL 허용. BODY_FILE: fd close. BODY_PIPE: in_fd·out_fd close, pid > 0이면
 * kill(SIGKILL) + waitpid(EINTR 재시도)로 회수. BODY_UPSTREAM: upstream.conn이 남아 있으면
 * upstream_release(conn, false). BODY_MEM: data 해제. head.fields·pipe.in_data 해제.
 * 정상 중계를 마친 conn은 먼저 upstream_release(conn, 재사용 여부)를 부르고 upstream.conn을
 * NULL로 만든 뒤 plan_cleanup을 부른다.
 *
 * @param p  계획. NULL 허용
 *
 * 에러:    없음 (errno를 바꾸지 않음)
 * 소유권:  계획이 소유한 모든 것을 해제
 * 스레드:  [per-object]
 * I/O:     close, kill, waitpid
 * 블로킹:  waitpid (SIGKILL 직후)
 * 테스트:  U-HD-04, E-CG-07, E-CG-09 · 추후 U-HD-05, U-HD-06
 */
void plan_cleanup(response_plan_t *p);

#endif
