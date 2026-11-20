#!/bin/sh
# CGI fixture: Status 201과 사용자 헤더
printf 'Status: 201 Created\r\nX-Cgi: 1\r\nContent-Type: text/plain\r\n\r\ncreated\n'
