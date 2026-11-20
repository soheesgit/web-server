/* src/core/log.h
 *
 * 접근 로그와 에러 로그 (스레드 안전, 한 줄 단위).
 * 디렉토리 책임과 관련 테스트: src/core/README.md
 */
#ifndef HTTPD_CORE_LOG_H
#define HTTPD_CORE_LOG_H

#include <stdint.h>

/**
 * 에러 로그 수준. 값이 클수록 심각하다.
 *
 * 생성:   상수
 * 소멸:   없음
 * 수명:   없음
 * 스레드: 값 타입
 */
typedef enum {
    LOG_DEBUG, /* 개발용 상세 정보 */
    LOG_INFO,  /* 기동·종료 등 정상 사건 */
    LOG_WARN,  /* 처리는 계속되는 이상 (백엔드 제외, EMFILE 등) */
    LOG_ERROR  /* 요청 또는 서버 기능 실패 */
} log_level_t;

/**
 * 로그 출력 대상을 연다.
 *
 * access_path·error_path가 NULL이거나 빈 문자열이면 해당 로그는 stderr로 간다.
 * 파일은 O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 모드 0644로 연다. 이미 열려 있으면 먼저
 * log_close와 같은 정리를 한다. 열기 전·실패 후에도 log_msg·log_access는 stderr로 쓴다.
 * min보다 낮은 수준의 log_msg는 버린다.
 *
 * @param access_path  접근 로그 파일 경로. NULL 허용
 * @param error_path   에러 로그 파일 경로. NULL 허용
 * @param min          기록할 최소 수준
 * @return             0 성공, -1 실패 (하나라도 열지 못함. 열린 것은 닫고 stderr로 되돌림)
 *
 * 에러:    open의 errno
 * 소유권:  경로를 빌려 연다. 연 fd는 log 모듈이 소유하고 log_close가 닫는다
 * 스레드:  [main-only]
 * I/O:     파일
 * 블로킹:  open 한 번
 * 테스트:  (테스트 없음)
 */
int log_open(const char *access_path, const char *error_path, log_level_t min);

/**
 * 연 로그 파일을 닫고 출력을 stderr로 되돌린다.
 *
 * 멱등. log_open 전에 불러도 안전하다.
 *
 * 에러:    없음 (errno를 바꾸지 않음)
 * 소유권:  log_open이 연 fd를 닫는다
 * 스레드:  [main-only]
 * I/O:     파일 close
 * 블로킹:  없음
 * 테스트:  (테스트 없음)
 */
void log_close(void);

/**
 * 접근 로그 한 줄을 쓴다.
 *
 * 형식 (줄 끝 LF):
 *   <ISO8601 UTC> <ip> "<method> <target>" <status> <bytes> <ms>ms
 * 예: 2023-11-14T22:13:20Z 127.0.0.1 "GET /index.html" 200 1234 3ms
 * 시각은 벽시계(platform_wall_time)를 gmtime_r로 바꾼 초 단위다. target의 제어 문자(0x00~0x1F,
 * 0x7F)와 '"'는 \xHH로 바꾼다. 한 줄은 write 한 번으로 써서 여러 스레드의 줄이 섞이지 않게 한다.
 *
 * @param client_ip   "127.0.0.1" 형식. NULL이면 "-"
 * @param method      메서드 토큰. NULL이면 "-"
 * @param target      원본 request-target. NULL이면 "-"
 * @param status      응답 상태 코드
 * @param bytes       보낸 본문 바이트 수
 * @param elapsed_ms  요청 처리 시간 (ms)
 *
 * 에러:    없음 (쓰기 실패는 무시)
 * 소유권:  인자를 빌린다
 * 스레드:  [thread-safe]
 * I/O:     파일 또는 stderr
 * 블로킹:  write 한 번
 * 테스트:  추후 U-CORE-26, U-CORE-27
 */
void log_access(const char *client_ip, const char *method, const char *target, int status,
                uint64_t bytes, int64_t elapsed_ms);

/**
 * 에러 로그 한 줄을 쓴다.
 *
 * 형식: <ISO8601 UTC> [<DEBUG|INFO|WARN|ERROR>] <메시지>LF. 메시지 끝의 LF는 하나로 합친다.
 * lvl이 log_open의 min보다 낮으면 버린다. 한 줄은 write 한 번.
 *
 * @param lvl  수준
 * @param fmt  printf 형식. NULL 불가
 *
 * 에러:    없음 (쓰기 실패는 무시)
 * 소유권:  인자를 빌린다
 * 스레드:  [thread-safe]
 * I/O:     파일 또는 stderr
 * 블로킹:  write 한 번
 * 테스트:  (테스트 없음)
 */
void log_msg(log_level_t lvl, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif
