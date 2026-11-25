#!/usr/bin/env python3
"""기능 테스트(E2E) 러너. 판정 규칙과 출력 형식: docs/TESTING.md

사용법: run_e2e.py --bin build/httpd [--pending tests/pending.txt] [--only ID접두사] [--list] [-v]
"""
import argparse
import glob
import importlib
import os
import signal
import sys
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import harness  # noqa: E402

TEST_LIMIT_S = 30


class TestTimeout(Exception):
    pass


def _on_alarm(signum, frame):
    raise TestTimeout(f"timeout ({TEST_LIMIT_S}s)")


def load_pending(path):
    ids = set()
    if path and os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if line:
                    ids.add(line.split()[0])
    return ids


def reason_of(exc):
    if isinstance(exc, AssertionError) and str(exc):
        return str(exc)
    msg = str(exc)
    return f"{type(exc).__name__}: {msg}" if msg else type(exc).__name__


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=os.path.join(harness.REPO, "build", "httpd"))
    ap.add_argument("--pending", default=None)
    ap.add_argument("--only", default=None)
    ap.add_argument("--engine", choices=("pool", "loop"))
    ap.add_argument("--list", action="store_true")
    ap.add_argument("-v", action="store_true", dest="verbose")
    # 보조 스위트(tests/robust)가 같은 실행기를 쓰도록 테스트 모듈 위치만 바꿀 수 있게 한다.
    ap.add_argument("--tests", default=os.path.join(HERE, "test_*.py"))
    args = ap.parse_args()

    sys.path.insert(0, os.path.dirname(os.path.abspath(args.tests)))
    for path in sorted(glob.glob(args.tests)):
        importlib.import_module(os.path.splitext(os.path.basename(path))[0])
    tests = sorted(harness.REGISTRY, key=lambda t: t[0])
    if args.only:
        tests = [t for t in tests if t[0].startswith(args.only)]

    if args.list:
        for tid, name, _, optional in tests:
            print(f"{tid}  {name}{'  (optional)' if optional else ''}")
        return 0

    harness.BIN = os.path.abspath(args.bin)
    harness.ENGINE = args.engine
    if not os.access(harness.BIN, os.X_OK):
        print(f"run_e2e: {harness.BIN} not found or not executable", file=sys.stderr)
        return 2
    harness.prepare_fixtures()
    pending = load_pending(args.pending)
    signal.signal(signal.SIGALRM, _on_alarm)

    counts = {"pass": 0, "pending": 0, "fail": 0, "unexpected": 0, "skip": 0}
    total = 0
    for tid, name, fn, optional in tests:
        total += 1
        reason = ""
        outcome = "pass"
        signal.setitimer(signal.ITIMER_REAL, TEST_LIMIT_S)
        try:
            fn()
        except harness.Skip as e:
            outcome, reason = "skip", str(e)
        except BaseException as e:  # noqa: BLE001  (KeyboardInterrupt는 아래에서 다시 올림)
            if isinstance(e, KeyboardInterrupt):
                raise
            outcome, reason = "fail", reason_of(e)
            if args.verbose and not isinstance(e, (AssertionError, TestTimeout,
                                                   harness.ServerStartError)):
                reason += " | " + traceback.format_exc().strip().splitlines()[-2].strip()
        finally:
            signal.setitimer(signal.ITIMER_REAL, 0)
            harness.kill_all()

        is_pending = tid in pending
        if outcome == "skip":
            label = f"SKIP ({reason})" if reason else "SKIP"
            counts["skip"] += 1
        elif outcome == "pass":
            label = "UNEXPECTED PASS" if is_pending else "PASS"
            counts["unexpected" if is_pending else "pass"] += 1
        elif is_pending:
            label = "PENDING"
            counts["pending"] += 1
        elif optional:
            label = "FAIL (optional)"
            counts["fail"] += 0
        else:
            label = "FAIL"
            counts["fail"] += 1
        line = f"{label:<17}{tid}  {name}"
        if args.verbose and outcome == "fail" and reason:
            line += f"  ({reason})"
        print(line, flush=True)

    print("---")
    print(f"{total} total: {counts['pass']} pass, {counts['pending']} pending, "
          f"{counts['fail']} fail, {counts['unexpected']} unexpected, {counts['skip']} skip")
    return 1 if counts["fail"] or counts["unexpected"] else 0


if __name__ == "__main__":
    sys.exit(main())
