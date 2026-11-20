/* src/downstream/uri.h
 *
 * request-target 분리, absolute-form 분해, 퍼센트 디코딩, 경로 정규화.
 * 디렉토리 책임과 관련 테스트: src/downstream/README.md
 */
#ifndef HTTPD_DOWNSTREAM_URI_H
#define HTTPD_DOWNSTREAM_URI_H

#include <stddef.h>

#include "core/str.h"

/**
 * 대상을 경로와 쿼리로 나눈다.
 *
 * 첫 '#' 이후는 버린다(fragment). 그 앞에서 첫 '?'로 나눠 앞은 경로, 뒤는 쿼리. '?'가 없으면
 * 쿼리는 길이 0. 디코딩하지 않는다.
 * 예: "/a/b?x=1#frag" → path "/a/b", query "x=1".
 *
 * @param target     입력 (origin-form 경로 부분)
 * @param out_path   [out] 경로 (target을 빌림). NULL 불가
 * @param out_query  [out] 쿼리 (target을 빌림, 없으면 len 0). NULL 불가
 * @return           0 성공, -1 target 길이 0
 *
 * 에러:    EINVAL (빈 입력)
 * 소유권:  결과는 target을 빌림
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-DS-01 (request_parse_head를 통해) · 추후 U-DS-04, U-DS-21
 */
int uri_split(str_t target, str_t *out_path, str_t *out_query);

/**
 * absolute-form 대상을 authority와 나머지로 나눈다.
 *
 * "http://"(스킴 대소문자 무시) 뒤에서 첫 '/', '?', '#' 전까지가 authority("host[:port]").
 * 나머지(out_rest)는 그 뒤 전부다. 나머지가 비어 있으면 정적 문자열 "/"를 돌려준다. 나머지가
 * '/'가 아닌 문자('?', '#')로 시작하면 -1 (보수적 거부). authority가 비었거나 userinfo('@')가
 * 있으면 -1. http 스킴이 아니면 -1.
 * 예: "http://a.com:8080/x?y" → authority "a.com:8080", rest "/x?y". "http://a.com" → rest "/".
 *
 * @param target         입력
 * @param out_authority  [out] authority (target을 빌림). NULL 불가
 * @param out_rest       [out] 경로+쿼리 (target 또는 정적 "/"를 빌림). NULL 불가
 * @return               0 성공, -1 실패
 *
 * 에러:    EINVAL
 * 소유권:  결과는 target 또는 정적 메모리를 빌림
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  추후 U-DS-16
 */
int uri_parse_absolute(str_t target, str_t *out_authority, str_t *out_rest);

/**
 * 경로를 퍼센트 디코딩한다 (한 번만).
 *
 * "%XX"(16진 2자리, 대소문자 무시)를 바이트로 바꾼다. 잘못된 %XX("%zz", "%2", 끝의 "%"),
 * 디코딩 결과가 NUL(%00), 결과 + NUL이 cap을 넘으면 -1. '+'는 그대로 둔다(경로에서 공백 아님).
 * "%2F"는 '/'가 된다 (D-002). 이중 인코딩("%252e")은 "%2e"까지만 디코딩한다.
 * 결과는 NUL 종료.
 * 예: "%20" → " ", "%EA%B0%80" → "가"(3바이트).
 *
 * @param in       입력 경로
 * @param out      [out] 결과 버퍼. NULL 불가
 * @param cap      out 크기 (NUL 포함)
 * @param out_len  [out] 성공 시 결과 길이 (NUL 제외). NULL 허용
 * @return         0 성공, -1 실패 (out 내용은 정의되지 않음)
 *
 * 에러:    EINVAL (형식, %00), ENAMETOOLONG (cap 초과)
 * 소유권:  in을 빌려 out에 쓴다
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-DS-22, E-ST-10 · 추후 U-DS-23~26, U-DS-29
 */
int uri_decode_path(str_t in, char *out, size_t cap, size_t *out_len);

/**
 * 디코딩된 절대 경로를 제자리에서 정규화한다.
 *
 * path는 '/'로 시작하는 NUL 종료 문자열이어야 한다(아니면 -1). 연속 '/'는 하나로, "." 세그먼트는
 * 제거, ".." 세그먼트는 앞 세그먼트를 지운다. 루트 위로 올라가면 -1. 끝의 '/'는 보존한다
 * (끝이 "/." 또는 "/.."이면 결과도 '/'로 끝난다). 결과는 입력보다 길어지지 않는다.
 * 예: "/a/./b//c/" → "/a/b/c/", "/a/b/../c" → "/a/c", "/" → "/", "/.." → -1, "/a/../../b" → -1.
 *
 * @param path  [in,out] 경로. NULL 불가
 * @return      0 성공, -1 실패 (path 내용은 정의되지 않음)
 *
 * 에러:    EINVAL ('/'로 시작하지 않음), EPERM (루트 위 이탈)
 * 소유권:  제자리 수정
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-DS-27, U-DS-28, E-ST-06 · 추후 U-DS-19
 */
int path_normalize(char *path);

#endif
