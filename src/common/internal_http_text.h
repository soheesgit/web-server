/* HTTP 헤더에서 공통으로 사용하는 바이트 및 목록 처리. */
#ifndef HTTPD_COMMON_INTERNAL_HTTP_TEXT_H
#define HTTPD_COMMON_INTERNAL_HTTP_TEXT_H

#include <string.h>

#include "core/str.h"

/** HTTP의 선택적 공백 바이트인지 확인한다. */
static inline bool http_is_ows(char c)
{
    return c == ' ' || c == '\t';
}

/** HTTP 헤더 값에 들어갈 수 있는 바이트인지 확인한다. */
static inline bool http_field_byte_valid(unsigned char c)
{
    return http_is_ows((char)c) || (c >= 0x21 && c != 0x7f);
}

/** HTTP 헤더 이름과 토큰에 허용되는 바이트인지 확인한다. */
static inline bool http_token_byte_valid(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           (c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

/** 쉼표 목록의 첫 항목을 꺼낸다. NULL은 목록 끝이며 길이 0인 항목과 구분된다. */
static inline str_t http_pop_list_item(str_t *list)
{
    const char *comma = memchr(list->ptr, ',', list->len);
    if (comma == NULL) {
        str_t item = *list;
        *list = (str_t){0};
        return str_trim_ows(item);
    }

    size_t n = (size_t)(comma - list->ptr);
    str_t item = {list->ptr, n};
    *list = (str_t){comma + 1, list->len - n - 1};
    return str_trim_ows(item);
}

#endif
