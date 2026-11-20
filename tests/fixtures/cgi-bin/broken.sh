#!/bin/sh
# CGI fixture: 빈 줄 없이 헤더만 출력하고 끝남 (헤드 미완성 → 502)
printf 'Content-Type: text/plain\r\nX-Broken: 1\r\n'
