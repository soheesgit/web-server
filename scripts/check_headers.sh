#!/bin/sh
# 공개 헤더 검사.
#   (1) 각 src/*/*.h 를 단독으로 include한 .c가 컴파일되는가 (자기충족)
#   (2) 헤더의 모든 함수 선언 바로 위에 /** 로 시작하는 주석이 있는가
#   (3) 각 src/<dir>/ 에 README.md 와 AGENTS.md 가 있는가
#   (4) include guard 이름이 HTTPD_<DIR>_<FILE>_H 인가
# 환경 변수: CC (기본 cc), HTTPD_CFLAGS (Makefile이 컴파일 플래그를 넘김)
set -u
cd "$(dirname "$0")/.." || exit 1

CC=${CC:-cc}
HTTPD_CFLAGS=${HTTPD_CFLAGS:--std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Werror -Isrc}
fail=0

if [ ! -d src ]; then
    echo "check_headers: src/ 없음, 건너뜀"
    exit 0
fi

# (3) 디렉토리 문서
for d in src/*/; do
    [ -d "$d" ] || continue
    for f in README.md AGENTS.md; do
        if [ ! -f "$d$f" ]; then
            echo "${d}${f}: 없음"
            fail=1
        fi
    done
done

headers=$(find src -mindepth 2 -maxdepth 2 -name '*.h' | sort)

# (1) 자기충족
tmp=$(mktemp -d "${TMPDIR:-/tmp}/check_headers.XXXXXX") || exit 1
trap 'rm -rf "$tmp"' EXIT INT TERM
for h in $headers; do
    rel=${h#src/}
    printf '#include "%s"\n' "$rel" > "$tmp/one.c"
    # shellcheck disable=SC2086
    if ! $CC $HTTPD_CFLAGS -fsyntax-only -x c "$tmp/one.c" 2> "$tmp/err.txt"; then
        echo "$h: 단독 include로 컴파일되지 않음 (자기충족 아님)"
        sed 's/^/    /' "$tmp/err.txt"
        fail=1
    fi
done

# (2) 함수 선언 주석, (4) include guard
if [ -n "$headers" ]; then
    # shellcheck disable=SC2086
    python3 - $headers <<'PY' || fail=1
import re
import sys

bad = 0
for path in sys.argv[1:]:
    parts = path.split("/")
    want = "HTTPD_%s_%s_H" % (parts[1].upper(), parts[2][:-2].upper())
    lines = open(path, encoding="utf-8").read().splitlines()

    guard = None
    for l in lines:
        m = re.match(r"\s*#\s*ifndef\s+(\w+)", l)
        if m:
            guard = m.group(1)
            break
    if guard != want:
        print("%s: include guard가 %s 이어야 함 (현재 %s)" % (path, want, guard))
        bad += 1
    elif not any(re.match(r"\s*#\s*define\s+%s\b" % want, l) for l in lines):
        print("%s: #define %s 없음" % (path, want))
        bad += 1

    depth = 0
    in_comment = False
    in_macro = False
    for i, raw in enumerate(lines):
        line = raw
        # 주석 상태 추적 (선언 판정 전에 이 줄이 주석 안에서 시작하는지 기록)
        starts_in_comment = in_comment
        code = ""
        j = 0
        while j < len(line):
            if in_comment:
                k = line.find("*/", j)
                if k < 0:
                    j = len(line)
                else:
                    in_comment = False
                    j = k + 2
            else:
                k = line.find("/*", j)
                if k < 0:
                    code += line[j:]
                    j = len(line)
                else:
                    code += line[j:k]
                    in_comment = True
                    j = k + 2
        was_macro = in_macro
        in_macro = raw.rstrip().endswith("\\") and (was_macro or raw.lstrip().startswith("#"))
        if was_macro or raw.lstrip().startswith("#"):
            continue
        is_decl = (
            depth == 0
            and not starts_in_comment
            and raw[:1] not in ("", " ", "\t", "}", "/", "*", "#")
            and not raw.startswith("typedef")
            and "(" in code
        )
        depth += code.count("{") - code.count("}")
        if not is_decl:
            continue
        k = i - 1
        while k >= 0 and lines[k].strip() == "":
            k -= 1
        ok = False
        if k >= 0 and lines[k].rstrip().endswith("*/"):
            s = k
            while s >= 0 and "/*" not in lines[s]:
                s -= 1
            ok = s >= 0 and lines[s].lstrip().startswith("/**")
        if not ok:
            print("%s:%d: 함수 선언 바로 위에 /** 명세 주석이 없음: %s" % (path, i + 1, raw.strip()))
            bad += 1
sys.exit(1 if bad else 0)
PY
fi

if [ "$fail" -ne 0 ]; then
    exit 1
fi
echo "check_headers: ok"
