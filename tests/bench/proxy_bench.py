#!/usr/bin/env python3
"""로컬 HTTP 프록시 성능 시험. Python 표준 라이브러리만 사용한다."""

import argparse
import concurrent.futures
import contextlib
import http.client
import hashlib
import http.server
import json
import math
import os
from pathlib import Path
import platform
import random
import shutil
import socket
import statistics
import sys
import subprocess
import tempfile
import threading
import time

POLICIES = ("rr", "least_requests", "p2c")
POOLS = (1, 8, 50)
PAYLOAD = b"x" * 1024
ROOT = Path(__file__).resolve().parents[2]


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        time.sleep(self.server.delay)
        try:
            self.send_response_only(200)
            self.send_header("Content-Length", str(len(PAYLOAD)))
            self.send_header("X-Backend-Id", str(self.server.number))
            self.end_headers()
            self.wfile.write(PAYLOAD)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, _format, *_args):
        pass


class Backend(http.server.ThreadingHTTPServer):
    request_queue_size = 256
    daemon_threads = True

    def __init__(self, port, delay, number, handler=Handler):
        super().__init__(("127.0.0.1", port), handler)
        self.delay = delay
        self.number = number
        self.clients = set()
        self.lock = threading.Lock()
        self.thread = threading.Thread(target=self.serve_forever, kwargs={"poll_interval": .01})
        self.thread.start()

    def get_request(self):
        conn, addr = super().get_request()
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        with self.lock:
            self.clients.add(conn)
        return conn, addr

    def shutdown_request(self, request):
        with self.lock:
            self.clients.discard(request)
        super().shutdown_request(request)

    def close_clients(self):
        with self.lock:
            for conn in self.clients:
                with contextlib.suppress(OSError):
                    conn.shutdown(socket.SHUT_RDWR)

    def handle_error(self, request, client_address):
        if not isinstance(sys.exc_info()[1], (BrokenPipeError, ConnectionResetError)):
            super().handle_error(request, client_address)

    def stop(self):
        self.shutdown()
        self.server_close()
        self.close_clients()
        self.thread.join()


def resources(pid):
    fd = rss = None
    if platform.system() == "Linux":
        proc = Path("/proc") / str(pid)
        try:
            fd = len(list((proc / "fd").iterdir()))
            for line in (proc / "status").read_text().splitlines():
                if line.startswith("VmRSS:"):
                    rss = int(line.split()[1])
                    break
        except FileNotFoundError:
            pass
    else:
        if shutil.which("lsof"):
            res = subprocess.run(["lsof", "-nP", "-a", "-p", str(pid), "-Ff"],
                                 capture_output=True, text=True, check=False)
            if res.returncode == 0:
                fd = sum(line[1:].isdigit() for line in res.stdout.splitlines()
                         if line.startswith("f"))
        res = subprocess.run(["ps", "-o", "rss=", "-p", str(pid)],
                             capture_output=True, text=True, check=False)
        if res.returncode == 0 and res.stdout.strip():
            rss = int(res.stdout.strip())
    return fd, rss


def request(port):
    start = time.perf_counter()
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
    status, source = 0, None
    try:
        conn.request("GET", "/api/blob", headers={"Connection": "close"})
        resp = conn.getresponse()
        data = resp.read()
        status = resp.status
        source = resp.getheader("X-Backend-Id")
        if status == 200 and data != PAYLOAD:
            status = 0
    except (OSError, http.client.HTTPException):
        pass
    finally:
        conn.close()
    return [(time.perf_counter() - start) * 1000, status, source]


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class Proxy:
    def __init__(self, binary, servers, policy, pool, extra=(), env_extra=None):
        self.tmp = tempfile.TemporaryDirectory(prefix="proxy-bench-")
        root = Path(self.tmp.name)
        self.log = root / "proxy.log"
        self.port = free_port()
        self.proc = None
        cfg = root / "bench.conf"
        endpoints = " ".join(f"127.0.0.1:{s.server_port}" for s in servers)
        cfg.write_text(f"listen 127.0.0.1:{self.port}\nroot {root}\nworkers 72\nqueue_len 256\n"
                       f"upstream bench {endpoints}\nroute /api/ proxy bench strip\n"
                       f"pool_max_idle {pool}\nbackend_cooldown_ms 700\n"
                       "upstream_connect_timeout_ms 150\nupstream_response_timeout_ms 120\n"
                       "shutdown_grace_ms 1000\n" + "".join(f"{line}\n" for line in extra))
        env = {**os.environ, "HTTPD_PROXY_LB": policy, "HTTPD_PROXY_RECOVERY": "ramp",
               "HTTPD_PROXY_METRICS": "1"}
        env.update(env_extra or {})
        try:
            with self.log.open("w") as log:
                self.proc = subprocess.Popen([str(binary), "-c", str(cfg)], env=env,
                                             stdout=subprocess.DEVNULL, stderr=log)
            deadline = time.monotonic() + 4
            while time.monotonic() < deadline and self.proc.poll() is None:
                try:
                    with socket.create_connection(("127.0.0.1", self.port), timeout=.1):
                        return
                except OSError:
                    time.sleep(.02)
            raise RuntimeError(self.log.read_text())
        except BaseException:
            self.stop()
            raise

    def stop(self):
        try:
            if self.proc is None:
                return []
            if self.proc.poll() is None:
                self.proc.terminate()
                try:
                    self.proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.proc.kill()
                    self.proc.wait()
            text = self.log.read_text()
            if self.proc.returncode != 0:
                raise RuntimeError(text)
            rows = []
            for line in text.splitlines():
                if "proxy_metrics " in line:
                    fields = dict(part.split("=") for part in line.split("proxy_metrics ", 1)[1]
                                  .split())
                    rows.append({key: int(val) if val.isdigit() else val
                                 for key, val in fields.items()})
            return rows
        finally:
            self.tmp.cleanup()


def workload(pool, port, count, spacing=0):
    start = time.perf_counter()
    tasks = []
    for i in range(count):
        if spacing:
            remain = start + i * spacing - time.perf_counter()
            if remain > 0:
                time.sleep(remain)
        tasks.append(pool.submit(request, port))
    rows = [f.result() for f in tasks]
    return rows, time.perf_counter() - start


def trial(binary, condition, policy, pool_size, repeat):
    concurrency = 48 if condition == "pool_high" else 4 if condition == "pool_low" else 24
    delays = (.002, .002, .002) if condition != "heterogeneous" else (.001, .004, .014)
    servers = []
    proxy = None
    worker = None
    sample_done = threading.Event()
    samples = []
    results = []
    fault_done = threading.Event()
    try:
        servers.extend(Backend(0, delay, i) for i, delay in enumerate(delays))
        proxy = Proxy(binary, servers, policy, pool_size)

        def sample():
            while not sample_done.is_set():
                samples.append(resources(proxy.proc.pid))
                sample_done.wait(.05)

        def fault():
            time.sleep(.10)
            if condition == "blackout":
                ports = [server.server_port for server in servers]
                for server in servers:
                    server.stop()
                time.sleep(.30)
                for i, port in enumerate(ports):
                    servers[i] = Backend(port, delays[i], i)
            else:
                # 셋 중 두 대가 각기 다른 오류를 내고 한 대는 계속 서비스한다.
                servers[1].delay = .180
                failed = servers[2]
                port = failed.server_port
                failed.stop()
                time.sleep(.30)
                servers[1].delay = delays[1]
                servers[2] = Backend(port, delays[2], 2)
            fault_done.set()

        with concurrent.futures.ThreadPoolExecutor(concurrency) as clients:
            warmup, _ = workload(clients, proxy.port, 48)
            if any(status != 200 for _, status, _ in warmup):
                raise RuntimeError(f"warmup failed for {condition}: {warmup}")
            sampler = threading.Thread(target=sample)
            sampler.start()
            try:
                if condition in ("fault", "blackout"):
                    worker = threading.Thread(target=fault)
                    worker.start()
                    rows, duration = workload(clients, proxy.port, 360, .004)
                    results = [(condition, rows, duration)]
                elif condition.startswith("pool_"):
                    for phase in ("burst", "reuse", "peer_closed"):
                        if phase == "reuse":
                            time.sleep(.22)
                        if phase == "peer_closed":
                            for s in servers:
                                s.close_clients()
                            time.sleep(.05)
                        rows, duration = workload(clients, proxy.port, 240)
                        results.append((phase, rows, duration))
                else:
                    rows, duration = workload(clients, proxy.port, 480)
                    results = [("steady", rows, duration)]
            finally:
                sample_done.set()
                sampler.join()
        if worker is not None:
            worker.join()
            worker = None
            if not fault_done.is_set():
                raise RuntimeError("fault reintegration failed")
        metrics = proxy.stop()
        proxy = None
        if len(metrics) != 3 or any(row["active"] != 0 for row in metrics):
            raise RuntimeError(f"unexpected final backend metrics: {metrics}")
        return {"condition": condition, "policy": policy, "pool": pool_size, "repeat": repeat,
                "concurrency": concurrency, "warmup": len(warmup), "queue_peak": 0,
                "resources": samples, "metrics": metrics, "phases": results}
    finally:
        if worker is not None:
            worker.join()
        if proxy is not None:
            proxy.stop()
        for backend in servers:
            backend.stop()


def environment(binary):
    def command(*argv):
        return subprocess.check_output(argv, text=True).splitlines()[0]

    info = {"platform": platform.platform(), "cpu": platform.machine(), "cpus": os.cpu_count(),
            "python": platform.python_version(), "compiler": command("cc", "--version"),
            "binary": str(binary.relative_to(ROOT)) if binary.is_relative_to(ROOT) else str(binary),
            "commit_base": command("git", "-C", str(ROOT), "rev-parse", "HEAD"),
            "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
            "upstream_body_bytes": len(PAYLOAD), "shuffle_seed": 24601,
            "workers": 72, "fault_cooldown_ms": 700}
    if platform.system() == "Darwin":
        info["model"] = command("sysctl", "-n", "hw.model")
        info["memory_bytes"] = int(command("sysctl", "-n", "hw.memsize"))
    return info


def percentile(values, fraction):
    return values[math.ceil(len(values) * fraction) - 1]


def summarize(trial_row):
    phases = trial_row["phases"]
    requests = [item for _, items, _ in phases for item in items]
    durations = sum(seconds for _, _, seconds in phases)
    sorted_ms = sorted(row[0] for row in requests)
    backends = trial_row["metrics"]
    n = len(requests)
    new_tcp = sum(b["sockets"] for b in backends)
    reused = sum(b["reused"] for b in backends)
    return {
        "rps": n / durations, "mean_ms": statistics.mean(sorted_ms),
        **{f"p{p}": percentile(sorted_ms, p / 100) for p in (50, 95, 99)},
        "fail_pct": 100 * sum(row[1] != 200 for row in requests) / n,
        "503": sum(row[1] == 503 for row in requests), "new_tcp": new_tcp, "reused": reused,
        "reuse_pct": 100 * reused / (new_tcp + reused),
        "fd": max((x for x, _ in trial_row["resources"] if x is not None), default=None),
        "rss_kib": max((x for _, x in trial_row["resources"] if x is not None), default=None),
        "recovery_ms": max((b["detect_to_healthy_ms"] for b in backends), default=0),
        "cooldowns": sum(b["cooldowns"] for b in backends),
        "assignment_share": [b["assigned"] / sum(x["assigned"] for x in backends)
                             for b in backends],
        "peak_active": [b["peak_active"] for b in backends],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build/httpd")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--repeat", type=int, default=5)
    options = parser.parse_args()
    binary = options.binary.resolve(strict=True)
    conditions = [(env, policy, 8) for env in ("homogeneous", "heterogeneous", "fault", "blackout")
                  for policy in POLICIES]
    conditions += [(env, "rr", size) for env in ("pool_low", "pool_high") for size in POOLS]
    rng = random.Random(24601)
    with options.out.open("w", encoding="utf-8") as out:
        out.write(json.dumps({"environment": environment(binary)}) + "\n")
        for repeat in range(1, options.repeat + 1):
            ordered = rng.sample(conditions, len(conditions))
            for condition, policy, size in ordered:
                row = trial(binary, condition, policy, size, repeat)
                row["summary"] = summarize(row)
                out.write(json.dumps(row, separators=(",", ":")) + "\n")
                out.flush()
                print(f"{repeat}/{options.repeat} {condition} {policy} {size}", flush=True)


if __name__ == "__main__":
    main()
