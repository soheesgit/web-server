/* src/core/buf.h
 *
 * 상한이 있는 가변 바이트 버퍼와 fd 읽기·쓰기.
 * 디렉토리 책임과 관련 테스트: src/core/README.md
 */
#ifndef HTTPD_CORE_BUF_H
#define HTTPD_CORE_BUF_H

#include <stddef.h>
#include <sys/types.h>

/**
 * 힙에 둔 가변 바이트 버퍼. len은 max를 넘지 않는다.
 *
 * 생성:   buf_init. 0으로 채운 buf_t는 "빈 버퍼"로 buf_cleanup에만 안전하다
 * 소멸:   buf_cleanup
 * 수명:   buf_cleanup까지. data는 buf_append·buf_appendf·buf_read_fd가 재할당할 수 있으므로
 *         data를 빌린 str_t·포인터는 그 호출 뒤 무효다
 * 스레드: [per-object]
 */
typedef struct {
    char *data; /* 힙 메모리. 빈 버퍼면 NULL 가능. NUL 종료를 보장하지 않음 */
    size_t len; /* 유효 바이트 수. 0 ≤ len ≤ cap, len ≤ max */
    size_t cap; /* data의 할당 크기 (바이트) */
    size_t max; /* len 상한 (바이트). 넘으려 하면 실패 */
} buf_t;

/**
 * 버퍼를 초기화한다.
 *
 * initial 바이트를 미리 할당한다(initial이 0이면 할당하지 않고 data = NULL). len = 0.
 * initial > max이면 initial을 max로 줄인다. max가 0이면 실패.
 *
 * @param b        초기화할 버퍼. NULL 불가. 이전 내용은 해제하지 않고 덮어쓴다
 * @param initial  처음 할당할 바이트 수
 * @param max      len 상한 (바이트). 0 불가
 * @return         0 성공, -1 실패 (실패하면 *b는 0으로 채워진 상태)
 *
 * 에러:    ENOMEM 할당 실패, EINVAL max가 0
 * 소유권:  b->data를 할당하고 b가 소유한다. buf_cleanup으로 해제
 * 스레드:  [per-object]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-CORE-08
 */
int buf_init(buf_t *b, size_t initial, size_t max);

/**
 * 버퍼 메모리를 해제하고 0으로 채운다.
 *
 * 멱등이다. 두 번 불러도, 0으로 채운(초기화하지 않은) 버퍼에 불러도, b가 NULL이어도 안전하다.
 * 호출 뒤 *b는 모든 필드가 0이다.
 *
 * @param b  정리할 버퍼. NULL 허용
 *
 * 에러:    없음 (errno를 바꾸지 않음)
 * 소유권:  b->data를 해제한다
 * 스레드:  [per-object]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  추후 U-CORE-12
 */
void buf_cleanup(buf_t *b);

/**
 * 버퍼 끝에 n바이트를 덧붙인다.
 *
 * len + n > max이면 아무것도 바꾸지 않고 -1 + EMSGSIZE. 필요하면 cap을 늘린다(늘리는 방식은
 * 구현 재량이지만 cap ≤ max를 넘겨 할당할 필요는 없다). n이 0이면 아무것도 하지 않고 0.
 * 할당이 실패해도 내용은 바뀌지 않는다.
 *
 * @param b  대상 버퍼. buf_init된 버퍼. NULL 불가
 * @param p  덧붙일 바이트. n이 0이면 NULL 허용. b->data 내부를 가리켜도 된다
 * @param n  바이트 수
 * @return   0 성공, -1 실패
 *
 * 에러:    EMSGSIZE max 초과, ENOMEM 할당 실패
 * 소유권:  p를 빌려 복사한다
 * 스레드:  [per-object]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-CORE-08, U-CORE-10 · 추후 U-CORE-09
 */
int buf_append(buf_t *b, const void *p, size_t n);

/**
 * printf 형식으로 만든 문자열을 버퍼 끝에 덧붙인다 (NUL은 붙이지 않음).
 *
 * 결과가 max를 넘거나 형식화·할당이 실패하면 내용을 바꾸지 않고 -1.
 * 예: buf_appendf(b, "%d-%s", 42, "x") → "42-x" 추가.
 *
 * @param b    대상 버퍼. NULL 불가
 * @param fmt  printf 형식 문자열. NULL 불가
 * @return     0 성공, -1 실패
 *
 * 에러:    EMSGSIZE max 초과, ENOMEM 할당 실패, EINVAL 형식화 실패
 * 소유권:  인자를 빌린다
 * 스레드:  [per-object]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  추후 U-CORE-16
 */
int buf_appendf(buf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/**
 * 앞에서 n바이트를 제거하고 남은 바이트를 앞으로 당긴다.
 *
 * n ≥ len이면 len = 0. 메모리는 줄이지 않는다.
 * 예: 10바이트 중 4 → 남은 6바이트가 앞으로, 10 → len 0, 20 → len 0.
 *
 * @param b  대상 버퍼. NULL 불가
 * @param n  제거할 바이트 수
 *
 * 에러:    없음
 * 소유권:  없음. 이 버퍼를 빌린 str_t는 무효가 된다
 * 스레드:  [per-object]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-CORE-11
 */
void buf_consume(buf_t *b, size_t n);

/**
 * len을 0으로 만든다. 메모리는 유지한다.
 *
 * @param b  대상 버퍼. NULL 불가
 *
 * 에러:    없음
 * 소유권:  없음
 * 스레드:  [per-object]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  (테스트 없음)
 */
void buf_reset(buf_t *b);

/**
 * fd에서 read를 한 번 해서 버퍼 끝에 붙인다.
 *
 * 읽을 크기는 남은 자리(max - len)와 구현이 정한 한 번의 읽기 크기 중 작은 값이다.
 * len이 이미 max면 read를 부르지 않고 -1 + EMSGSIZE. EINTR은 내부에서 재시도한다.
 *
 * @param b   대상 버퍼. NULL 불가
 * @param fd  읽을 fd
 * @return    >0 읽은 바이트 수, 0 EOF, -1 에러
 *
 * 에러:    EAGAIN·EWOULDBLOCK(non-blocking이고 데이터 없음), EMSGSIZE(버퍼 가득),
 *          ENOMEM, 그 밖의 read errno (ECONNRESET 등)
 * 소유권:  없음
 * 스레드:  [per-object]
 * I/O:     fd 읽기
 * 블로킹:  fd가 blocking이면 read 한 번 (fd의 SO_RCVTIMEO까지)
 * 테스트:  U-CORE-13 · 추후 U-CORE-14
 */
ssize_t buf_read_fd(buf_t *b, int fd);

/**
 * 버퍼 앞부분을 fd에 write 한 번으로 보내고, 보낸 만큼 consume한다.
 *
 * 소켓이면 SIGPIPE를 일으키지 않는다(platform_send 사용). len이 0이면 0.
 * EINTR은 내부에서 재시도한다. 부분 전송이면 보낸 만큼만 지우고, 다시 불러 이어 쓴다.
 *
 * @param b   대상 버퍼. NULL 불가
 * @param fd  쓸 fd
 * @return    보낸 바이트 수(≥0), -1 에러
 *
 * 에러:    EAGAIN·EWOULDBLOCK, EPIPE(상대가 닫음), 그 밖의 write errno
 * 소유권:  없음
 * 스레드:  [per-object]
 * I/O:     fd 쓰기
 * 블로킹:  fd가 blocking이면 write 한 번 (SO_SNDTIMEO까지)
 * 테스트:  추후 U-CORE-15
 */
ssize_t buf_write_fd(buf_t *b, int fd);

#endif
