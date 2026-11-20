/* src/downstream/listener.h
 *
 * 대기 소켓 열기·accept·닫기.
 * 디렉토리 책임과 관련 테스트: src/downstream/README.md
 */
#ifndef HTTPD_DOWNSTREAM_LISTENER_H
#define HTTPD_DOWNSTREAM_LISTENER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

/**
 * TCP 대기 소켓을 연다.
 *
 * host:port에 socket → SO_REUSEADDR → bind → listen(backlog). host는 IPv4 주소 문자열
 * ("0.0.0.0", "127.0.0.1") 또는 이름(getaddrinfo, AI_PASSIVE, 첫 IPv4 결과). port 0이면
 * 커널이 고른다(listener_port로 확인). 결과 fd는 O_NONBLOCK, FD_CLOEXEC.
 * 실패하면 연 것을 닫고 err에 "<단계>: <strerror_r 메시지>"를 쓴다.
 * 예: 포트가 사용 중이면 "bind: Address already in use".
 *
 * @param host     바인드 주소. NULL이면 "0.0.0.0"
 * @param port     포트. 0이면 임의
 * @param backlog  listen backlog (1 미만이면 128)
 * @param err      [out] 실패 메시지. errcap이 0이면 NULL 허용
 * @param errcap   err 크기
 * @return         대기 소켓 fd, -1 실패
 *
 * 에러:    EADDRINUSE, EACCES(권한 없는 포트), 그 밖의 socket·bind·listen·getaddrinfo 실패
 * 소유권:  fd를 호출자가 소유하고 listener_close로 닫는다
 * 스레드:  [main-only]
 * I/O:     소켓
 * 블로킹:  getaddrinfo (이름일 때)
 * 테스트:  U-DS-36, E-CF-03, E-CF-04 · 추후 U-DS-37, U-DS-39
 */
int listener_open(const char *host, uint16_t port, int backlog, char *err, size_t errcap);

/**
 * 대기 소켓에서 연결 하나를 받는다.
 *
 * Linux는 accept4(SOCK_CLOEXEC), 그 밖에는 accept + platform_set_cloexec. EINTR은 내부에서
 * 재시도한다. EMFILE·ENFILE이면 log_msg(LOG_WARN)로 한 번 기록하고 -1을 돌려준다(호출자는 잠시
 * 쉬고 다시 시도). cfd의 O_NONBLOCK 상태는 정하지 않는다 — conn_new가 정한다 (D-012).
 * ECONNABORTED는 EAGAIN과 같이 취급해 -1 + EAGAIN.
 *
 * @param lfd       대기 소켓
 * @param out_cfd   [out] 성공 시 연결 소켓 (cloexec). 실패 시 바꾸지 않음. NULL 불가
 * @param out_peer  [out] 성공 시 상대 주소. NULL 허용
 * @return          0 성공, -1 실패
 *
 * 에러:    EAGAIN·EWOULDBLOCK(대기 연결 없음), EMFILE·ENFILE(fd 고갈), 그 밖의 accept errno
 * 소유권:  cfd는 호출자가 소유 (보통 conn_new로 이전)
 * 스레드:  [thread-safe]
 * I/O:     소켓
 * 블로킹:  lfd가 non-blocking이므로 없음
 * 테스트:  U-EN-02 (engine을 통해) · 추후 U-DS-38
 */
int listener_accept(int lfd, int *out_cfd, struct sockaddr_storage *out_peer);

/**
 * 대기 소켓이 실제로 bind된 포트를 돌려준다 (getsockname).
 *
 * @param lfd  대기 소켓
 * @return     포트 (1~65535), -1 실패
 *
 * 에러:    getsockname의 errno
 * 소유권:  없음
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-DS-36
 */
int listener_port(int lfd);

/**
 * 대기 소켓을 닫는다.
 *
 * lfd가 음수면 아무것도 하지 않는다.
 *
 * @param lfd  대기 소켓 또는 -1
 *
 * 에러:    없음 (errno를 바꾸지 않음)
 * 소유권:  lfd를 닫는다
 * 스레드:  [main-only]
 * I/O:     소켓 close
 * 블로킹:  없음
 * 테스트:  U-DS-36
 */
void listener_close(int lfd);

#endif
