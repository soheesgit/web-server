# core/ AGENTS

- 대상 테스트: 계약 U-CORE-04, 05, 08, 10, 11, 13, 17, 21, 28, 29, 30 · 기능 E-CF-01, 02, 04.
- include 가능한 다른 디렉토리 헤더: 없음.
- 함정:
  - `str_t`는 NUL 종료가 아니다. `strlen`·`strcmp`·`strtoull`을 `ptr`에 바로 쓰지 말고 길이로 다룬다.
  - `buf_append`는 실패해도 내용이 그대로여야 한다(realloc 실패, max 초과 모두). 재할당 후에는 이 버퍼를 빌린 `str_t`가 무효다.
  - macOS에는 `MSG_NOSIGNAL`이 없다(`SO_NOSIGPIPE`). sendfile 시그니처도 Linux와 다르다. 분기는 `platform.c`에만.
  - `config_load`의 오류 메시지는 E-CF-02가 `파일:줄` 형식을 확인한다.
- 작업을 나눌 때는 파일 단위로.
