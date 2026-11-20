#!/bin/sh
# CGI fixture: 헤더 뒤 10MB(10485760바이트, 0x00) 출력 (D-020)
printf 'Content-Type: application/octet-stream\r\n\r\n'
exec head -c 10485760 /dev/zero
