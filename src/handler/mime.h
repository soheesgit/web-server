/* src/handler/mime.h
 *
 * 파일 확장자 → Content-Type.
 * 디렉토리 책임과 관련 테스트: src/handler/README.md
 */
#ifndef HTTPD_HANDLER_MIME_H
#define HTTPD_HANDLER_MIME_H

/**
 * 경로의 마지막 확장자로 Content-Type을 고른다 (D-019).
 *
 * 마지막 '/' 뒤의 파일 이름에서 마지막 '.' 뒤가 확장자다(ASCII 대소문자 무시). 파일 이름에 '.'이
 * 없거나 '.'으로 시작하는 이름(".bashrc")만 있으면 확장자 없음. 표:
 *   html, htm  text/html; charset=utf-8      css   text/css; charset=utf-8
 *   js, mjs    text/javascript; charset=utf-8 txt  text/plain; charset=utf-8
 *   json       application/json              svg   image/svg+xml
 *   png        image/png                     jpg, jpeg  image/jpeg
 *   gif        image/gif                     ico   image/x-icon
 *   webp       image/webp                    pdf   application/pdf
 *   woff2      font/woff2                    mp4   video/mp4
 *   gz         application/gzip
 * 그 밖(확장자 없음 포함)은 "application/octet-stream".
 * 예: "/a/LOGO.PNG" → image/png, "/a.b/file" → octet-stream, "x.tar.gz" → application/gzip.
 *
 * @param path  경로, NUL 종료. NULL 불가
 * @return      정적 문자열 (해제 금지). NULL을 돌려주지 않음
 *
 * 에러:    없음
 * 소유권:  정적 메모리
 * 스레드:  [thread-safe]
 * I/O:     없음
 * 블로킹:  없음
 * 테스트:  U-HD-09, E-ST-01, E-ST-02 · 추후 U-HD-07, U-HD-08
 */
const char *mime_for_path(const char *path);

#endif
