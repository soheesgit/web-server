#!/usr/bin/env python3
"""v1 워커 설정 또는 동일 조건에서 v1·v2 동시성 엔진을 비교한다."""

import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack
import http.client
import json
from pathlib import Path
import random
import socket
import threading
import time

from proxy_bench import Backend, Handler, Proxy, ROOT, environment, percentile, resources, workload

SMALL_REQUESTS = 480
CLIENTS = 48
WARMUP = 48
IDLE_CONNECTIONS = 16
SLOW_READERS = 6
SLOW_BODY = b"x" * (8 << 20)
READ_RATE = 8 << 20
SCENARIOS = ("normal", "idle_keepalive", "slow_download")
DELAYS = (.001, .004, .014)
ORDER_SEED = 15205


class WorkerBackend(Handler):
    def do_GET(self):
        if self.path != "/slow-download":
            super().do_GET()
            return
        time.sleep(self.server.delay)
        self.send_response_only(200)
        self.send_header("Content-Length", str(len(SLOW_BODY)))
        self.end_headers()
        self.wfile.write(SLOW_BODY)


def hold_idle(port, stack, count):
    for _ in range(count):
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
        stack.callback(conn.close)
        conn.request("GET", "/api/blob")
        response = conn.getresponse()
        if response.status != 200 or len(response.read()) != 1024 or conn.sock is None:
            raise RuntimeError("could not establish idle keep-alive connection")


def slow_download(port, ready, begin):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=8)
    try:
        conn.connect()
        conn.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 64 << 10)
        conn.request("GET", "/api/slow-download", headers={"Connection": "close"})
        response = conn.getresponse()
        if response.status != 200:
            raise RuntimeError(f"large response status: {response.status}")
        ready.set()
        if not begin.wait(5):
            raise RuntimeError("small workload did not start")
        total = 0
        started = time.monotonic()
        while block := response.read(64 << 10):
            total += len(block)
            remaining = started + total / READ_RATE - time.monotonic()
            if remaining > 0:
                time.sleep(remaining)
        if total != len(SLOW_BODY):
            raise RuntimeError(f"incomplete large response: {total}")
        return time.monotonic() - started
    finally:
        ready.set()
        conn.close()


def measure(proxy, clients):
    samples = []
    done = threading.Event()

    def sample():
        while not done.is_set():
            samples.append(resources(proxy.proc.pid))
            done.wait(.04)

    sampler = threading.Thread(target=sample)
    sampler.start()
    try:
        rows, elapsed = workload(clients, proxy.port, SMALL_REQUESTS)
    finally:
        done.set()
        sampler.join()

    successes = sorted(ms for ms, status, _ in rows if status == 200)
    return {
        "requests": rows, "elapsed_s": elapsed, "resources": samples,
        "success_rps": len(successes) / elapsed,
        "small_p99_ms": percentile(successes, .99) if successes else None,
        "count_503": sum(status == 503 for _, status, _ in rows),
        "transport_errors": sum(status == 0 for _, status, _ in rows),
        "other_http_errors": sum(status not in (0, 200, 503) for _, status, _ in rows),
        "peak_rss_kib": max((rss for _, rss in samples if rss is not None), default=None),
    }


def trial(binary, scenario, workers, queue, engine="pool", idle_connections=IDLE_CONNECTIONS):
    with ExitStack() as stack:
        backends = []
        for i, delay in enumerate(DELAYS):
            backend = Backend(0, delay, i, WorkerBackend)
            stack.callback(backend.stop)
            backends.append(backend)
        proxy = Proxy(binary, backends, "least_requests", 8, extra=[
            f"engine {engine}", f"workers {workers}", f"queue_len {queue}",
            "keepalive_timeout_ms 15000", "upstream_response_timeout_ms 2000",
        ])
        stack.callback(proxy.stop)
        clients = stack.enter_context(ThreadPoolExecutor(CLIENTS))
        warmup, _ = workload(clients, proxy.port, WARMUP)
        if any(status != 200 for _, status, _ in warmup):
            raise RuntimeError(f"warmup failed ({workers} workers, queue {queue})")

        slow = []
        if scenario in ("idle_keepalive", "dense_keepalive"):
            hold_idle(proxy.port, stack, idle_connections)
        elif scenario == "slow_download":
            readers = stack.enter_context(ThreadPoolExecutor(SLOW_READERS))
            begin = threading.Event()
            ready = [threading.Event() for _ in range(SLOW_READERS)]
            slow = [readers.submit(slow_download, proxy.port, event, begin) for event in ready]
            try:
                if not all(event.wait(5) for event in ready):
                    raise RuntimeError("large response headers did not arrive")
                for future in slow:
                    if future.done():
                        future.result()
            finally:
                begin.set()
        result = measure(proxy, clients)
        result["slow_download_seconds"] = [future.result() for future in slow]
        if scenario == "slow_download" and min(result["slow_download_seconds"]) < result["elapsed_s"]:
            raise RuntimeError("small workload outlasted a slow download; reduce the request count")
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build/httpd")
    parser.add_argument("--workers", type=int, help="v1 worker count for the comparison")
    parser.add_argument("--compare-engines", action="store_true")
    parser.add_argument("--repeat", type=int, default=5)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    workers = args.workers if args.workers is not None else (40 if args.compare_engines else 20)
    if not IDLE_CONNECTIONS < workers <= 512 or args.repeat < 1:
        parser.error("--workers must be between 17 and 512 and --repeat must be positive")
    binary = args.binary.resolve(strict=True)
    dense_idle = workers - 4
    if args.compare_engines:
        if dense_idle <= IDLE_CONNECTIONS:
            parser.error("--compare-engines requires at least 21 workers")
        cases = [(s, workers, 1024, e, dense_idle if s == "dense_keepalive" else IDLE_CONNECTIONS)
                 for s in (*SCENARIOS, "dense_keepalive") for e in ("pool", "loop")]
    else:
        cases = [(s, w, q, "pool", IDLE_CONNECTIONS)
                 for s in SCENARIOS for w in (workers, workers * 2) for q in (32, 1024)]
    rng = random.Random(ORDER_SEED)
    setup = environment(binary)
    for key in ("workers", "fault_cooldown_ms"):
        setup.pop(key)
    setup.update(engine="pool_vs_loop" if args.compare_engines else "pool",
                 policy="least_requests", pool_max_idle=8,
                 small_requests=SMALL_REQUESTS, clients=CLIENTS, warmup_requests=WARMUP,
                 backend_delays_ms=[d * 1000 for d in DELAYS], idle_connections=IDLE_CONNECTIONS,
                 slow_readers=SLOW_READERS, slow_body_bytes=len(SLOW_BODY),
                 slow_reader_bytes_per_sec=READ_RATE, shuffle_seed=ORDER_SEED)
    if args.compare_engines:
        setup.update(pool_workers=workers, pool_queue_len=1024, dense_idle_connections=dense_idle)
    with args.out.open("w", encoding="utf-8") as f:
        f.write(json.dumps({"environment": setup}) + "\n")
        for repeat in range(1, args.repeat + 1):
            for scenario, worker_count, queue, engine, idle in rng.sample(cases, len(cases)):
                row = trial(binary, scenario, worker_count, queue, engine, idle)
                row.update(repeat=repeat, scenario=scenario, workers=worker_count, queue_len=queue,
                           engine=engine, idle_connections=idle if "keepalive" in scenario else 0)
                f.write(json.dumps(row, separators=(",", ":")) + "\n")
                f.flush()
                print(repeat, scenario, engine, worker_count, queue, flush=True)


if __name__ == "__main__":
    main()
