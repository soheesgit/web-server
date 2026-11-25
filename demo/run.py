"""학습 데모의 로컬 httpd·모의 LLM·관측 서버를 함께 실행한다."""
import argparse
from collections import deque
from contextlib import ExitStack
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import parse_qs, urlsplit

ROOT = Path(__file__).resolve().parents[1]
WEB = "demo/www"
HOST = "127.0.0.1"


def free_port():
    with socket.socket() as sock:
        sock.bind((HOST, 0))
        return sock.getsockname()[1]


def reserve(stack, port=0):
    sock = stack.enter_context(socket.socket())
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((HOST, port))
    return sock


def wait_for_port(port, child):
    cutoff = time.monotonic() + 5
    while time.monotonic() < cutoff and child.poll() is None:
        try:
            with socket.create_connection((HOST, port), timeout=0.1):
                return
        except OSError:
            time.sleep(0.03)
    raise RuntimeError(f"{HOST}:{port}에서 프로세스를 시작하지 못했습니다.")


def stop(child):
    if child is None or child.poll() is not None:
        return
    child.terminate()
    try:
        child.wait(timeout=3)
    except subprocess.TimeoutExpired:
        child.kill()
        child.wait()


class Backend:
    def __init__(self, index, port):
        self.name = f"b{index}"
        self.port = port
        self.process = None

    @property
    def endpoint(self):
        return f"{HOST}:{self.port}"

    @property
    def online(self):
        return self.process is not None and self.process.poll() is None

    def start(self):
        self.process = subprocess.Popen(
            [sys.executable, str(ROOT / "bench/fake_llm.py"), "--port", str(self.port),
             "--id", self.name, "--time-scale", "1.0"],
            stdout=subprocess.DEVNULL,
        )
        wait_for_port(self.port, self.process)


class Monitor:
    def __init__(self, backends, args):
        self.backends = {backend.endpoint: backend for backend in backends}
        self.engine = args.engine
        self.picker = args.picker
        self.lock = threading.Lock()
        self.events = deque(maxlen=400)
        self.states = {}
        self.cursor = 0
        self.state_seq = -1

    def backend_name(self, endpoint):
        backend = self.backends.get(endpoint)
        return backend.name if backend else None

    def receive(self, packet):
        first, *rows = packet.decode().splitlines()
        kind, *fields = first.split("\t")
        if kind == "state":
            seq = int(fields[0])
            states = {}
            for row in rows:
                endpoint, active, health = row.split("\t")
                states[endpoint] = {"active": int(active), "health": health}
            with self.lock:
                if seq > self.state_seq:
                    self.states = states
                    self.state_seq = seq
            return

        session, attempt, selected, result = fields
        event = {"type": kind, "session": session, "attempt": int(attempt),
                 "backend": self.backend_name(selected)}
        if kind == "attempt":
            event["error"] = int(result)
            event["choices"] = choices = []
            for row in rows:
                tag, *values = row.split("\t")
                if tag == "choice":
                    policy, limit, duration, preferred = values
                    choices.append({"policy": policy, "limit": int(limit),
                                    "duration_us": int(duration), "preferred": self.backend_name(preferred),
                                    "candidates": []})
                elif tag == "candidate":
                    endpoint, reason, active, rank = values
                    choices[-1]["candidates"].append({"backend": self.backend_name(endpoint),
                                                       "reason": reason, "active": int(active),
                                                       "rank": int(rank)})
        elif kind == "failure":
            event["reason"] = result
        else:
            return

        with self.lock:
            self.cursor += 1
            event.update(seq=self.cursor, at_ms=time.time_ns() / 1_000_000)
            self.events.append(event)

    def snapshot(self, after):
        with self.lock:
            return {
                "engine": self.engine, "picker": self.picker, "cursor": self.cursor,
                "backends": [
                    {"id": backend.name, "online": backend.online,
                     **self.states.get(endpoint, {"active": 0, "health": None})}
                    for endpoint, backend in self.backends.items()
                ],
                "events": [event for event in self.events if event["seq"] > after],
            }


def observe(sock, monitor, closing):
    sock.settimeout(0.2)
    while not closing.is_set():
        try:
            packet = sock.recv(65536)
            monitor.receive(packet)
        except (TimeoutError, ValueError, IndexError):
            continue
        except OSError:
            return


def create_monitor_server(monitor):
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            url = urlsplit(self.path)
            if url.path != "/__demo/state":
                self.send_error(404)
                return
            try:
                after = max(0, int(parse_qs(url.query).get("after", ["0"])[0]))
            except ValueError:
                self.send_error(400)
                return
            body = json.dumps(monitor.snapshot(after)).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *_args):
            pass

    return ThreadingHTTPServer((HOST, 0), Handler)


def operator_input(backends, closing):
    for command in sys.stdin:
        if closing.is_set():
            return
        number = command.strip()
        if number not in ("1", "2", "3"):
            print("1, 2, 3을 입력해 해당 백엔드를 끄거나 다시 켭니다.", flush=True)
            continue
        backend = backends[int(number) - 1]
        if backend.online:
            stop(backend.process)
            print(f"{backend.name} 중단", flush=True)
        else:
            try:
                backend.start()
                if closing.is_set():
                    stop(backend.process)
                print(f"{backend.name} 재시작", flush=True)
            except RuntimeError as error:
                stop(backend.process)
                print(error, file=sys.stderr)


def launch(args):
    if args.binary is None:
        subprocess.run(["make", "-s"], cwd=ROOT, check=True)
        binary = ROOT / "build/httpd"
    else:
        binary = args.binary.resolve(strict=True)

    closing = threading.Event()
    httpd = None
    with ExitStack() as resources:
        gateway_port = reserve(resources, args.port)
        reserved_backends = [reserve(resources) for _ in range(3)]
        backends = [Backend(index, sock.getsockname()[1])
                    for index, sock in enumerate(reserved_backends, start=1)]
        monitor = Monitor(backends, args)
        trace_socket = resources.enter_context(socket.socket(socket.AF_INET, socket.SOCK_DGRAM))
        service = resources.enter_context(create_monitor_server(monitor))
        work = resources.enter_context(tempfile.TemporaryDirectory(prefix="week13-study-demo-"))
        trace_socket.bind((HOST, 0))
        config = Path(work) / "httpd.conf"
        worker = threading.Thread(target=service.serve_forever, daemon=True)
        worker.start()
        threading.Thread(target=observe, args=(trace_socket, monitor, closing), daemon=True).start()

        try:
            for backend, reservation in zip(backends, reserved_backends):
                reservation.close()
                backend.start()
            endpoints = " ".join(backend.endpoint for backend in backends)
            config.write_text(
                f"listen {HOST}:{args.port}\nroot {WEB}\nengine {args.engine}\n"
                f"upstream llm {endpoints}\npicker llm {args.picker}\n"
                f"upstream demo_monitor {HOST}:{service.server_port}\n"
                f"route / static {WEB}\nroute /v1/ proxy llm\nroute /__demo/ proxy demo_monitor\n"
                "backend_fail_threshold 1\nbackend_cooldown_ms 2500\n"
                "upstream_connect_timeout_ms 700\nupstream_response_timeout_ms 5000\n"
                f"access_log {Path(work) / 'access.log'}\n"
            )
            env = {**os.environ, "HTTPD_DEMO_TRACE_PORT": str(trace_socket.getsockname()[1]),
                   "HTTPD_PROXY_LB": "rr", "HTTPD_PROXY_RECOVERY": "single"}
            gateway_port.close()
            httpd = subprocess.Popen([str(binary), "-c", str(config)], cwd=ROOT, env=env)
            wait_for_port(args.port, httpd)
            print(f"http://{HOST}:{args.port}/", flush=True)
            print("장애 시연: 이 터미널에서 1, 2, 3 + Enter · 같은 번호로 복구", flush=True)
            threading.Thread(target=operator_input, args=(backends, closing), daemon=True).start()
            return httpd.wait()
        finally:
            closing.set()
            stop(httpd)
            for backend in backends:
                stop(backend.process)
            service.shutdown()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--engine", choices=("loop", "pool"), default="loop")
    parser.add_argument("--picker", choices=("rr", "hash", "bounded"), default="bounded")
    parser.add_argument("--binary", type=Path)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("--port는 1~65535 범위여야 합니다.")

    def terminate(_signum, _frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, terminate)
    try:
        return launch(args)
    except KeyboardInterrupt:
        return 0
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"데모 실행 실패: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
