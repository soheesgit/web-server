/* src/core/log.c
 *
 * 접근·에러 로그. 명세: log.h
 */
#include "core/log.h"
#include "core/stub.h"

int log_open(const char *access_path, const char *error_path, log_level_t min)
{
    UNUSED(access_path);
    UNUSED(error_path);
    UNUSED(min);
    /* TODO(core): 구현 — (테스트 없음) */
    STUB_RETURN(-1);
}

void log_close(void)
{
    /* TODO(core): 구현 — (테스트 없음) */
}

void log_access(const char *client_ip, const char *method, const char *target, int status,
                uint64_t bytes, int64_t elapsed_ms)
{
    UNUSED(client_ip);
    UNUSED(method);
    UNUSED(target);
    UNUSED(status);
    UNUSED(bytes);
    UNUSED(elapsed_ms);
    /* TODO(core): 구현 — (테스트 없음) */
    STUB_VOID();
}

void log_msg(log_level_t lvl, const char *fmt, ...)
{
    UNUSED(lvl);
    UNUSED(fmt);
    /* TODO(core): 구현 — (테스트 없음) */
    STUB_VOID();
}
