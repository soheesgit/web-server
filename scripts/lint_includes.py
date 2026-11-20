#!/usr/bin/env python3
"""src/ 안의 #include "..." 가 허용된 방향만 따르는지 검사한다.

허용 방향 표의 근거: docs/CONVENTIONS.md "include 방향".
같은 디렉토리 안의 include는 항상 허용한다. 다른 디렉토리의 내부 헤더
(<dir>/internal_*.h)는 허용 방향이어도 금지다.
위반하면 "파일:줄: A -> B not allowed"를 출력하고 종료 코드 1.
"""
import pathlib
import re
import sys

ALLOWED = {
    "core": set(),
    "common": {"core"},
    "router": {"core", "common"},
    "upstream": {"core", "common"},
    "handler": {"core", "common", "router", "upstream"},
    "engine": {"core"},
    "downstream": {"core", "common", "router", "handler", "upstream", "engine"},
    "app": {"core", "common", "router", "handler", "upstream", "engine", "downstream"},
}

INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"')


def main() -> int:
    repo = pathlib.Path(__file__).resolve().parent.parent
    src = repo / "src"
    if not src.is_dir():
        print("lint_includes: src/ 없음, 건너뜀")
        return 0
    bad = 0
    for path in sorted(src.glob("*/*.[ch]")):
        here = path.parent.name
        rel = path.relative_to(repo)
        if here not in ALLOWED:
            print(f"{rel}:1: 알 수 없는 디렉토리 '{here}' (docs/CONVENTIONS.md에 없음)")
            bad += 1
            continue
        text = path.read_text(encoding="utf-8")
        for no, line in enumerate(text.splitlines(), 1):
            m = INCLUDE_RE.match(line)
            if not m:
                continue
            target = m.group(1)
            dst = target.split("/", 1)[0] if "/" in target else here
            if dst == here:
                continue
            name = target.rsplit("/", 1)[-1]
            if name.startswith("internal_"):
                print(f"{rel}:{no}: {here} -> {dst} not allowed (내부 헤더 {target})")
                bad += 1
            elif dst not in ALLOWED or dst not in ALLOWED[here]:
                print(f"{rel}:{no}: {here} -> {dst} not allowed")
                bad += 1
    if bad:
        return 1
    print("lint_includes: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
