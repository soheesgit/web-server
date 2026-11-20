#!/bin/sh
# CGI fixture: 실행 권한이 없어야 한다 (모드 644 → 403)
printf 'Content-Type: text/plain\r\n\r\nshould not run\n'
