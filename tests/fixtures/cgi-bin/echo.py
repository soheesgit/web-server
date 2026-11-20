#!/usr/bin/env python3
"""CGI fixture: 주요 환경 변수, HTTP_* 변수, stdin 본문을 key=value 줄로 출력한다."""
import os
import sys

KEYS = ["REQUEST_METHOD", "QUERY_STRING", "CONTENT_LENGTH", "CONTENT_TYPE", "SCRIPT_NAME",
        "PATH_INFO", "REMOTE_ADDR", "SERVER_PORT", "SERVER_PROTOCOL"]

length = int(os.environ.get("CONTENT_LENGTH") or 0)
body = sys.stdin.buffer.read(length) if length > 0 else b""
out = sys.stdout.buffer
out.write(b"Content-Type: text/plain\r\n\r\n")
for k in KEYS:
    out.write(f"{k}={os.environ.get(k, '')}\n".encode())
for k in sorted(os.environ):
    if k.startswith("HTTP_"):
        out.write(f"{k}={os.environ[k]}\n".encode())
out.write(b"BODY=" + body + b"\n")
out.flush()
