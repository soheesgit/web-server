#!/usr/bin/env python3
"""풀 크기별 실제 60초 유휴 만료와 30초 주기 정리를 각각 5회 확인한다."""

import argparse
import concurrent.futures
from contextlib import ExitStack
import json
from pathlib import Path
import random
import threading
import time

from proxy_bench import POOLS, Proxy, ROOT, resources
from proxy_verify import backend, request, wait_for_held


def clients_open(server):
    with server.lock:
        return len(server.clients)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build/httpd")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    cases = [(repeat, pool) for repeat in range(1, 6) for pool in POOLS]
    random.Random(24601).shuffle(cases)
    trials = []
    with ExitStack() as cleanup:
        for repeat, pool in cases:
            origin = cleanup.enter_context(backend())
            proxy = Proxy(binary, [origin], "rr", pool, extra=[
                "workers 16", "upstream_response_timeout_ms 115000",
            ])
            cleanup.callback(lambda p=proxy: p.stop() if p.proc.poll() is None else None)
            origin.gate = threading.Event()
            with concurrent.futures.ThreadPoolExecutor(12) as callers:
                submitted = [callers.submit(request, proxy, "/api/hold") for _ in range(12)]
                try:
                    wait_for_held(origin, 12)
                finally:
                    origin.gate.set()
                assert all(reply.result()[0] == 200 for reply in submitted)
            returned = time.monotonic()
            expected = min(pool, 12)
            deadline = returned + 2
            while clients_open(origin) != expected and time.monotonic() < deadline:
                time.sleep(.01)
            actual = clients_open(origin)
            assert actual == expected, (repeat, pool, actual)
            trials.append((proxy, origin, returned, {
                "repeat": repeat, "pool": pool, "seed_concurrency": 12, "idle_at_start": actual,
                "fd_at_start": resources(proxy.proc.pid)[0],
            }))

        # 각 서버의 idle 구간을 겹쳐도 측정 중 추가 트래픽은 없다.
        start_last = max(trial[2] for trial in trials)
        time.sleep(max(0, start_last + 62 - time.monotonic()))
        for proxy, origin, returned, row in trials:
            row["retained_at_midcheck"] = clients_open(origin)
            row["midcheck_age_s"] = round(time.monotonic() - returned, 3)
        time.sleep(max(0, start_last + 92 - time.monotonic()))
        with args.out.open("w", encoding="utf-8") as out:
            for proxy, origin, returned, row in trials:
                row["idle_after_sweep"] = clients_open(origin)
                row["sweep_check_age_s"] = round(time.monotonic() - returned, 3)
                row["fd_after_sweep"] = resources(proxy.proc.pid)[0]
                assert row["idle_after_sweep"] == 0, row
                assert request(proxy)[0] == 200
                stat, = proxy.stop()
                row["expired"] = stat["expired"]
                row["tcp_after_reburst"] = stat["sockets"]
                assert stat["expired"] == row["idle_at_start"] and stat["sockets"] == 13, row
                out.write(json.dumps(row, separators=(",", ":")) + "\n")
                out.flush()
                print(f"PASS pool={row['pool']} run={row['repeat']}", flush=True)


if __name__ == "__main__":
    main()
