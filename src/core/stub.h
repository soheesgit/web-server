/* src/core/stub.h
 *
 * 미구현 함수 몸체용 매크로 (구현이 끝나면 include를 지운다).
 * 디렉토리 책임과 관련 테스트: src/core/README.md
 */
#ifndef HTTPD_CORE_STUB_H
#define HTTPD_CORE_STUB_H
#include <errno.h>

/* 미구현 함수의 몸체. errno = ENOSYS로 두고 지정한 값을 돌려준다. */
#define STUB_RETURN(val)                                                                           \
    do {                                                                                           \
        errno = ENOSYS;                                                                            \
        return (val);                                                                              \
    } while (0)
#define STUB_VOID()                                                                                \
    do {                                                                                           \
        errno = ENOSYS;                                                                            \
        return;                                                                                    \
    } while (0)
/* 인자 미사용 경고 억제 */
#define UNUSED(x) ((void)(x))

#endif
