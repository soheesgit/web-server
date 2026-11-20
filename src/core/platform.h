/* src/core/platform.h
 *
 * OS 차이를 감춘 저수준 호출: fd 플래그, SIGPIPE 없는 송신, sendfile, 시계, rlimit.
 * 디렉토리 책임과 관련 테스트: src/core/README.md
 */
#ifndef HTTPD_CORE_PLATFORM_H
#define HTTPD_CORE_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

/**
 * fd에 O_NONBLOCK을 켠다.
 *
 * 다른 상태 플래그는 유지한다. 이미 켜져 있으면 아무것도 하지 않고 0.
 *
 * @param fd  대상 fd
 * @return    0 성공, -1 실패
 *
 * 에러:    fcntl의 errno (EBADF 등)
 * 소유권:  없음
 * 스레드:  [thread-safe]
 * I/O:     없음 (fcntl)
 * 블로킹:  없음
 * 테스트:  U-CORE-28
 */
int platform_set_nonblock(int fd);

/**
 * fd에 FD_CLOEXEC를 켠다.
 *
 * 다른 fd 플래그는 유지한다.
 *
 * @param fd  대상 fd
 * @return    0 성공, -1 실패
 *
 * 에러:    fcntl의 errno
 * 소유권:  없음
 * 스레드:  [thread-safe]
 * I/O:     없음 (fcntl)
 * 블로킹:  없음
 * 테스트:  U-CORE-29
 */
int platform_set_cloexec(int fd);

/**
 * 프로세스 전체에서 SIGPIPE를 무시한다 (SIG_IGN).
 *
 * platform_send가 시그널 없이 동작하더라도, pipe(CGI stdin) 쓰기 등에서의 SIGPIPE를 막기 위해
 * main이 시작할 때 한 번 부른다. 실패해도 계속 진행한다.
 *
 * 에러:    없음
 * 소유권:  없음
 * 스레드:  [main-only]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  추후 N-SIG-03
 */
void platform_ignore_sigpipe(void);

/**
 * 소켓에 SIGPIPE 없이 보낸다.
 *
 * Linux는 send(MSG_NOSIGNAL), macOS는 SO_NOSIGPIPE 설정 후 send. SIGPIPE를 무시하지 않은
 * 프로세스에서도 시그널이 나지 않아야 한다. EINTR은 내부에서 재시도한다. 부분 전송 가능.
 *
 * @param fd  연결된 소켓
 * @param p   보낼 바이트. n이 0이면 NULL 허용
 * @param n   바이트 수
 * @return    보낸 바이트 수, -1 에러
 *
 * 에러:    EPIPE(상대가 닫음), EAGAIN·EWOULDBLOCK, ECONNRESET, 그 밖의 send errno
 * 소유권:  p를 빌림
 * 스레드:  [thread-safe]
 * I/O:     소켓
 * 블로킹:  fd가 blocking이면 send 한 번 (SO_SNDTIMEO까지)
 * 테스트:  U-CORE-30
 */
ssize_t platform_send(int fd, const void *p, size_t n);

/**
 * 파일 구간을 소켓으로 보낸다.
 *
 * in_fd의 *inout_off부터 최대 count바이트를 out_fd로 보내고, 보낸 만큼 *inout_off를 전진한다.
 * Linux는 sendfile(2), macOS는 sendfile(2)(BSD 형태), 그 밖의 OS는 pread + platform_send 루프.
 * 부분 전송 가능 (non-blocking 소켓이면 보낸 만큼 돌려줌). 파일 끝이면 0.
 * SIGPIPE를 일으키지 않는다. EINTR은 내부에서 재시도한다.
 *
 * @param out_fd     연결된 소켓
 * @param in_fd      일반 파일 fd
 * @param inout_off  [in,out] 파일 오프셋. NULL 불가. 실패 시에도 실제 보낸 만큼은 전진
 * @param count      최대 바이트 수
 * @return           보낸 바이트 수(≥0), -1 에러
 *
 * 에러:    EAGAIN(보낸 것 없이 소켓 가득), EPIPE, 그 밖의 errno
 * 소유권:  없음
 * 스레드:  [thread-safe]
 * I/O:     파일 읽기, 소켓 쓰기
 * 블로킹:  out_fd가 blocking이면 SO_SNDTIMEO까지
 * 테스트:  추후 U-CORE-31
 */
ssize_t platform_sendfile(int out_fd, int in_fd, off_t *inout_off, size_t count);

/**
 * 단조 시계를 ms 단위로 돌려준다 (CLOCK_MONOTONIC).
 *
 * 기준점은 정해지지 않았고 차이만 의미가 있다. 연속 호출 값은 감소하지 않는다.
 *
 * @return  ms. 시계 오류면 -1
 *
 * 에러:    없음
 * 소유권:  없음
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  추후 U-CORE-32
 */
int64_t platform_now_ms(void);

/**
 * 벽시계 시각을 초 단위로 돌려준다 (Date·Last-Modified 헤더용).
 *
 * @return  1970-01-01 UTC 이후 초. 실패면 (time_t)-1
 *
 * 에러:    없음
 * 소유권:  없음
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  (테스트 없음)
 */
time_t platform_wall_time(void);

/**
 * RLIMIT_NOFILE 소프트 한도를 want까지 올린다.
 *
 * 하드 한도를 넘지 않게 min(want, hard)로 올린다(macOS는 OPEN_MAX도 상한). 이미 want 이상이면
 * 바꾸지 않는다. 결과 소프트 한도를 *out_got에 쓴다.
 *
 * @param want     원하는 fd 수
 * @param out_got  [out] 적용된 소프트 한도. NULL 허용
 * @return         0 성공(want보다 작게 올라갔어도), -1 getrlimit·setrlimit 실패
 *
 * 에러:    getrlimit·setrlimit의 errno
 * 소유권:  없음
 * 스레드:  [main-only]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  (테스트 없음)
 */
int platform_raise_nofile(uint64_t want, uint64_t *out_got);

#endif
