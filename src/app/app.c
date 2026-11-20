/* src/app/app.c
 *
 * 서버 조립. 명세: app.h
 */
#include "app/app.h"
#include "core/stub.h"

#include <stddef.h>

app_t *app_new(const config_t *cfg, char *err, size_t errcap)
{
    UNUSED(cfg);
    UNUSED(err);
    UNUSED(errcap);
    /* TODO(app): 구현 — E-CF-03, E-CF-04, E-* 전체 */
    STUB_RETURN(NULL);
}

int app_run(app_t *a, volatile sig_atomic_t *stop)
{
    UNUSED(a);
    UNUSED(stop);
    /* TODO(app): 구현 — E-* 전체 */
    STUB_RETURN(-1);
}

void app_free(app_t *a)
{
    UNUSED(a);
    /* TODO(app): 구현 — (테스트 없음) */
}
