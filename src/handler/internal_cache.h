/* src/handler/internal_cache.h
 *
 * 포워드 프록시 응답 캐시 (D-047). handler/ 안에서만 쓴다 (proxy.c ↔ cache.c).
 * 정책: GET만, 상태 200, Content-Length로 끝이 정해진 응답만 저장한다. 신선도는 s-maxage → max-age
 * (Age만큼 뺌). no-store·private·no-cache·Vary·Set-Cookie가 있거나 신선도가 없으면 저장하지 않는다.
 * 요청에 Authorization·Range·no-cache·no-store가 있으면 캐시를 쓰지 않는다. POST는 같은 URL의
 * 항목을 지운다. 저장소는 프로세스 하나에 하나(뮤텍스 보호), 총 바이트 상한을 넘으면 오래 안 쓴
 * 것부터 지운다.
 */
#ifndef HTTPD_HANDLER_INTERNAL_CACHE_H
#define HTTPD_HANDLER_INTERNAL_CACHE_H

#include <stdbool.h>
#include <stddef.h>

#include "common/types.h"
#include "core/str.h"

/* 응답 하나를 받는 동안 모으는 복사본. 소유: proxy_job_t.fill (proxy_job_cleanup이 해제) */
typedef struct cache_fill cache_fill_t;

/**
 * absolute-form 대상("http://Host:80/p?q")으로 캐시 키를 만든다. 스킴과 호스트 부분만 소문자로
 * 바꾼다.
 *
 * @param target  요청 대상
 * @param out     [out] NUL 종료 키
 * @param cap     out 크기
 * @return        키 길이. "http://"로 시작하지 않거나 cap에 안 들어가면 0
 */
size_t cache_make_key(str_t target, char *out, size_t cap);

/**
 * 요청 헤더로 이 요청이 캐시를 쓸 수 있는지 정한다.
 *
 * @param req        요청. NULL 불가
 * @param out_bypass [out] true면 저장된 항목을 쓰지 않는다 (Authorization·Range·no-cache·Pragma)
 * @param out_store  [out] false면 응답도 저장하지 않는다 (no-store, Authorization, Range)
 */
void cache_request_policy(const http_request_t *req, bool *out_bypass, bool *out_store);

/**
 * 신선한 항목이 있으면 응답 계획을 만든다 (본문은 BODY_MEM, 머리에 Age·X-Cache: HIT).
 *
 * @param key         cache_make_key의 결과
 * @param key_len     키 길이
 * @param keep_alive  클라이언트 연결 유지 여부
 * @param out_plan    [out] 적중이면 채움. 실패하면 plan_cleanup 상태로 돌려둠
 * @return            0 적중, -1 없음·만료·실패
 */
int cache_serve(const char *key, size_t key_len, bool keep_alive, response_plan_t *out_plan);

/**
 * 키의 항목을 지운다. 없으면 아무 일도 없다 (POST 같은 변경 요청 뒤에 부른다).
 */
void cache_invalidate(const char *key, size_t key_len);

/**
 * 응답을 모을 준비를 한다. 이 시점에는 아무것도 저장하지 않는다.
 *
 * @param key         키 (복사해 둔다)
 * @param key_len     키 길이
 * @param entry_max   항목 하나의 최대 바이트
 * @param total_max   저장소 전체 최대 바이트
 * @return            새 객체, NULL 실패 (캐시 없이 진행해도 된다)
 */
cache_fill_t *cache_fill_new(const char *key, size_t key_len, size_t entry_max, size_t total_max);

/**
 * 응답 머리가 저장할 만한지 판단한다. 아니면 이후 cache_fill_data·cache_fill_commit은 아무 일도 안
 * 한다.
 *
 * @param f       NULL이면 -1
 * @param resp    업스트림 응답 머리
 * @param fields  클라이언트에 보낼 머리 줄들 ("Name: value\r\n" 누적, Date·Server·Content-Length
 * 제외)
 * @return        0 저장 대상, -1 아님
 */
int cache_fill_arm(cache_fill_t *f, const http_response_t *resp, const buf_t *fields);

/**
 * 본문 바이트를 모은다. 선언한 길이를 넘으면 저장을 포기한다.
 */
void cache_fill_data(cache_fill_t *f, const char *p, size_t n);

/**
 * 본문을 다 받았으면(길이가 맞으면) 저장소에 넣는다. 총 상한을 넘으면 오래 안 쓴 항목을 지운다.
 */
void cache_fill_commit(cache_fill_t *f);

/**
 * 객체를 해제한다 (NULL 허용, 저장된 항목에는 영향 없음).
 */
void cache_fill_free(cache_fill_t *f);

/**
 * 저장소를 비운다 (테스트·종료용).
 */
void cache_reset(void);

#endif
