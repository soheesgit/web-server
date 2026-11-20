#!/bin/sh
# src/ 에서 금지 함수·금지 헤더 사용을 찾는다 (tests/ 는 검사하지 않음).
# 목록의 근거: docs/CONVENTIONS.md "금지 함수". 재진입·길이 제한 버전(snprintf, strerror_r,
# gmtime_r, strtoull 등)은 이름 경계 때문에 걸리지 않는다.
set -u
cd "$(dirname "$0")/.." || exit 1

if [ ! -d src ]; then
    echo "lint_forbidden: src/ 없음, 건너뜀"
    exit 0
fi

files=$(find src -type f \( -name '*.c' -o -name '*.h' \) | sort)
if [ -z "$files" ]; then
    echo "lint_forbidden: 검사할 파일 없음"
    exit 0
fi

funcs='(^|[^A-Za-z0-9_])(gets|sprintf|strcpy|strcat|system|popen|strtok|atoi|gmtime|strerror)[[:space:]]*\('
heads='#[[:space:]]*include[[:space:]]*<(curl/|openssl/|event2/|uv\.h>)'

# shellcheck disable=SC2086
if grep -nE "$funcs|$heads" $files; then
    echo "lint_forbidden: 위 줄에서 금지 함수 또는 금지 헤더를 사용함"
    exit 1
fi
echo "lint_forbidden: ok"
