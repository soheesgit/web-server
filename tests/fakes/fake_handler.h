/* tests/fakes/fake_handler.h
 *
 * C 계약 테스트 도우미: 파서를 거치지 않고 요청을 채우기, 임시 root 설정, 가짜 시계.
 * 파서가 구현되지 않은 상태에서도 handler·router 테스트를 할 수 있게 한다.
 */
#ifndef HTTPD_TESTS_FAKE_HANDLER_H
#define HTTPD_TESTS_FAKE_HANDLER_H

#include <stdint.h>

#include "common/types.h"
#include "core/config.h"

/* 요청을 직접 채운다. 모든 필드를 0으로 지운 뒤: method m, method_raw "GET"/"HEAD"/"POST"/"OTHER",
 * path 복사(쿼리 없음), target·raw_path는 out->path를 빌림, minor 1, keep_alive true,
 * framing NONE, head_len 0. host가 NULL이 아니면 out->host = host이고 "Host" 헤더도 추가한다.
 * path·host 문자열은 빌린다(리터럴을 넘긴다). */
void fake_request(http_request_t *out, http_method_t m, const char *path, const char *host);

/* 헤더 하나를 끝에 추가한다 (name·value는 빌림). 가득 차면 아무것도 하지 않는다. */
void fake_request_add_header(http_request_t *r, const char *name, const char *value);

/* config_defaults로 채운 설정을 할당하고, mkdtemp로 만든 빈 디렉토리를 root로 둔다.
 * 실패하면 NULL. 해제는 free() (테스트 프로세스가 끝나면 사라진다). */
config_t *fake_config(void);

/* 가짜 단조 시계 (ms). 시작값 1000000, fake_advance_ms로만 움직인다. */
int64_t fake_now_ms(void);
void fake_advance_ms(int64_t d);

#endif
