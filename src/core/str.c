/* src/core/str.c
 *
 * 빌린 문자열 뷰. 명세: str.h
 */
#include "core/str.h"
#include "core/stub.h"

#include <stddef.h>

str_t str_from_cstr(const char *s)
{
    UNUSED(s);
    /* TODO(core): 구현 — (테스트 없음) */
    STUB_RETURN(((str_t){NULL, 0}));
}

bool str_eq(str_t s, const char *lit)
{
    UNUSED(s);
    UNUSED(lit);
    /* TODO(core): 구현 — (테스트 없음) */
    STUB_RETURN(false);
}

bool str_eq_ci(str_t s, const char *lit)
{
    UNUSED(s);
    UNUSED(lit);
    /* TODO(core): 구현 — (테스트 없음) */
    STUB_RETURN(false);
}

bool str_eq_ci_str(str_t a, str_t b)
{
    UNUSED(a);
    UNUSED(b);
    /* TODO(core): 구현 — (테스트 없음) */
    STUB_RETURN(false);
}

str_t str_trim_ows(str_t s)
{
    UNUSED(s);
    /* TODO(core): 구현 — (테스트 없음) */
    STUB_RETURN(((str_t){NULL, 0}));
}

int str_to_u64(str_t s, uint64_t max, uint64_t *out_v)
{
    UNUSED(s);
    UNUSED(max);
    UNUSED(out_v);
    /* TODO(core): 구현 — U-CORE-04, U-CORE-05 */
    STUB_RETURN(-1);
}

int str_to_cstr(str_t s, char *out, size_t cap)
{
    UNUSED(s);
    UNUSED(out);
    UNUSED(cap);
    /* TODO(core): 구현 — (테스트 없음) */
    STUB_RETURN(-1);
}
