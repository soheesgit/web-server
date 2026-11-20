/* src/downstream/uri.c
 *
 * URI 분리·디코딩·정규화. 명세: uri.h
 */
#include "downstream/uri.h"
#include "core/stub.h"

int uri_split(str_t target, str_t *out_path, str_t *out_query)
{
    UNUSED(target);
    UNUSED(out_path);
    UNUSED(out_query);
    /* TODO(downstream): 구현 — U-DS-01 */
    STUB_RETURN(-1);
}

int uri_parse_absolute(str_t target, str_t *out_authority, str_t *out_rest)
{
    UNUSED(target);
    UNUSED(out_authority);
    UNUSED(out_rest);
    /* TODO(downstream): 구현 — (테스트 없음) */
    STUB_RETURN(-1);
}

int uri_decode_path(str_t in, char *out, size_t cap, size_t *out_len)
{
    UNUSED(in);
    UNUSED(out);
    UNUSED(cap);
    UNUSED(out_len);
    /* TODO(downstream): 구현 — U-DS-22, E-ST-10 */
    STUB_RETURN(-1);
}

int path_normalize(char *path)
{
    UNUSED(path);
    /* TODO(downstream): 구현 — U-DS-27, U-DS-28, E-ST-06 */
    STUB_RETURN(-1);
}
