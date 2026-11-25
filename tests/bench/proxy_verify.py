#!/usr/bin/env python3
"""기획서 ID가 없는 Stage 4 시나리오. 모든 백엔드와 클라이언트는 로컬에서 실행한다."""

import argparse
import concurrent.futures
from contextlib import contextmanager
import http.client
import json
from pathlib import Path
import socket
import threading
import time
from types import SimpleNamespace

from proxy_bench import Backend, Handler, Proxy, ROOT

ENGINE = "pool"


class Controlled(Handler):
    def respond(self, data, content_type="text/plain"):
        self.send_response_only(200)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Content-Type", content_type)
        self.send_header("X-Backend-Id", str(self.server.number))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path == "/hold" and self.server.gate is not None:
            with self.server.signal:
                self.server.waiting += 1
                self.server.signal.notify_all()
            self.server.gate.wait(115)
        if self.path in ("/incomplete", "/invalid-chunk"):
            self.send_response_only(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            self.wfile.write(b"3\r\nabc\r\n")
            self.wfile.write(b"3\r\na" if self.path == "/incomplete" else b"invalid\r\n")
            self.close_connection = True
            return
        if self.path == "/trailers":
            self.send_response_only(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            self.wfile.write(b"3;note=val\r\nabc\r\n0\r\nX-Trace: done\r\n\r\n")
            return
        self.echo()

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        with self.server.signal:
            self.server.posts += 1
        if self.path == "/drop":
            self.close_connection = True
            return
        self.echo(body)

    def echo(self, body=b""):
        reply = {"path": self.path, "body": body.decode(), "headers": dict(self.headers.items())}
        self.respond(json.dumps(reply).encode(), "application/json")


@contextmanager
def backend(port=0, number=0):
    server = Backend(port, 0, number, Controlled)
    server.gate = None
    server.waiting = server.posts = 0
    server.signal = threading.Condition()
    try:
        yield server
    finally:
        if server.gate is not None:
            server.gate.set()
        server.stop()


@contextmanager
def proxy(binary, backends, policy="rr", extra=(), env=None):
    p = Proxy(binary, backends, policy, 8, [*extra, f"engine {ENGINE}"], env)
    try:
        yield p
    finally:
        p.metrics = p.stop()
        assert p.metrics and all(row["active"] == 0 for row in p.metrics), p.metrics


def request(p, path="/api/echo", method="GET", body=None, headers=None, timeout=5):
    conn = http.client.HTTPConnection("127.0.0.1", p.port, timeout=timeout)
    try:
        conn.request(method, path, body=body, headers={"Connection": "close", **(headers or {})})
        resp = conn.getresponse()
        data = resp.read()
        return resp.status, resp.getheader("X-Backend-Id"), data
    finally:
        conn.close()


def unused_endpoint(sock):
    sock.bind(("127.0.0.1", 0))
    return SimpleNamespace(server_port=sock.getsockname()[1])


def forward_proxy(binary):
    with backend() as target:
        port = target.server_port
        env = {"HTTPD_FORWARD_PROXY": str(port)}
        extra = ["host 127.0.0.1 route / proxy bench"]
        with proxy(binary, [target], extra=extra) as p:
            assert request(p, f"http://127.0.0.1:{port}/echo")[0] == 403
        with proxy(binary, [target], extra=extra, env=env) as p:
            absolute = f"http://127.0.0.1:{port}"
            status, _, body = request(
                p, absolute + "?v=2", "POST", b"forward-body",
                {"Host": "client.example", "Proxy-Authorization": "Basic local-test",
                 "Connection": "close, X-Hop", "X-Hop": "local", "Content-Type": "text/plain"},
            )
            assert status == 200, status
            echo = json.loads(body)
            assert echo["path"] == "/?v=2" and echo["body"] == "forward-body", echo
            fields = {key.lower(): value for key, value in echo["headers"].items()}
            assert fields["host"] == f"127.0.0.1:{port}" and fields["content-type"] == "text/plain"
            assert "proxy-authorization" not in fields and "x-hop" not in fields
            assert request(p, f"http://127.0.0.1:{p.port}/echo")[0] == 403
            wrong = port + 1 if port < 65535 else port - 1
            assert request(p, f"http://127.0.0.1:{wrong}/echo")[0] == 403
            assert request(p, f"http://localhost:{port}/echo")[0] == 403
            assert request(p, absolute, "HEAD")[0] == 405
            assert request(p, f"127.0.0.1:{port}", "CONNECT")[0] == 501
            assert request(p, "/api/echo")[0] == 200
        assert target.posts == 1


def retry_and_framing(binary):
    with socket.socket() as reserved, backend() as origin:
        dead = unused_endpoint(reserved)
        with proxy(binary, [dead, origin],
                   extra=["host keep.example route /api/ proxy bench strip keep_host"]) as p:
            status, _, data = request(p, headers={"Host": "original.example"})
            assert status == 200
            echo = json.loads(data)
            assert echo["headers"]["Host"] == f"127.0.0.1:{origin.server_port}", echo
            assert echo["headers"]["X-Forwarded-Host"] == "original.example", echo
            assert json.loads(request(p, headers={"Host": "keep.example"})[2])["headers"]["Host"] == (
                "keep.example"
            )
            assert request(p, "/api/drop", "POST", b"once")[0] == 502
            assert origin.posts == 1
            assert request(p, "/api/trailers")[2] == b"abc"
            for path, expected in (("/api/incomplete", b"abca"), ("/api/invalid-chunk", b"abc")):
                try:
                    request(p, path)
                except http.client.IncompleteRead as error:
                    assert error.partial == expected, error.partial
                else:
                    raise AssertionError(f"{path} was accepted")
            assert request(p)[0] == 200
        assert p.metrics[1]["reused"] > 0


def wait_for_held(server, n):
    with server.signal:
        assert server.signal.wait_for(lambda: server.waiting >= n, timeout=3), server.waiting


def load_balancing(binary):
    for policy in ("least_requests", "p2c"):
        with backend(number=0) as a, backend(number=1) as b, proxy(
                binary, [a, b], policy, extra=["upstream_response_timeout_ms 4000"]) as p:
            a.gate = threading.Event()
            with concurrent.futures.ThreadPoolExecutor(2) as clients:
                held = clients.submit(request, p, "/api/hold")
                try:
                    wait_for_held(a, 1)
                    for _ in range(8):
                        assert request(p)[:2] == (200, "1")
                finally:
                    a.gate.set()
                assert held.result()[:2] == (200, "0")
        assert [b["assigned"] for b in p.metrics] == [1, 8], p.metrics


def half_open(binary):
    with socket.socket() as reserved:
        port = unused_endpoint(reserved).server_port
    with proxy(binary, [SimpleNamespace(server_port=port)],
               extra=["backend_fail_threshold 1", "backend_cooldown_ms 400",
                      "upstream_response_timeout_ms 4000"]) as p, backend(port=port) as target:
        # 서버가 열린 뒤라도 첫 수송 오류를 만들어 제외 상태로 전환한다.
        assert request(p, "/api/drop", "POST", b"once")[0] == 502
        t = time.monotonic()
        assert request(p)[0] == 503 and time.monotonic() - t < .3
        time.sleep(.45)
        with concurrent.futures.ThreadPoolExecutor(10) as clients:
            started = 0
            for limit in (1, 2, 4, 8):
                target.gate = threading.Event()
                jobs = [clients.submit(request, p, "/api/hold") for _ in range(limit)]
                started += limit
                try:
                    wait_for_held(target, started)
                    assert request(p)[0] == 503, limit
                finally:
                    target.gate.set()
                assert all(job.result()[0] == 200 for job in jobs)
        target.gate = threading.Event()
        with socket.create_connection(("127.0.0.1", p.port)) as client:
            client.sendall(b"GET /api/hold HTTP/1.1\r\nHost: localhost\r\n\r\n")
            wait_for_held(target, started + 1)
        # 앞 요청이 취소돼도 새 요청이 서비스되고 종료 시 모든 예약이 돌아온다.
        assert request(p)[0] == 200
        target.gate.set()
    assert p.metrics[0]["peak_active"] == 8, p.metrics
    assert p.metrics[0]["recoveries"] == 1 and p.metrics[0]["cooldowns"] == 1, p.metrics


def idle_expiry(binary):
    # 실제 운영 상수 60/30초를 사용한다. reaper 검사 동안 같은 백엔드의 활성 요청도 유지한다.
    with backend(number=0) as a, backend(number=1) as b, proxy(
            binary, [a], extra=["upstream_response_timeout_ms 110000"]) as idle, proxy(
            binary, [b], extra=["upstream_response_timeout_ms 110000"]) as leased:
        assert request(idle)[0] == 200
        assert request(leased)[0] == 200
        with b.lock:
            initial_sock = next(iter(b.clients))
        a.gate = threading.Event()
        with concurrent.futures.ThreadPoolExecutor(1) as clients:
            # warm connection을 빌린 활성 요청은 정리 대상 바깥에 있어야 한다.
            hold = clients.submit(request, idle, "/api/hold", timeout=105)
            try:
                wait_for_held(a, 1)
                assert request(idle)[0] == 200
                time.sleep(62)
                assert request(leased)[0] == 200
                with b.lock:
                    assert initial_sock not in b.clients
                time.sleep(30)
                with a.lock:
                    assert len(a.clients) == 1, len(a.clients)
                assert not hold.done()
            finally:
                a.gate.set()
            assert hold.result()[0] == 200
    assert idle.metrics[0]["expired"] == 1, idle.metrics
    assert leased.metrics[0]["expired"] == 1 and leased.metrics[0]["sockets"] == 2, leased.metrics


def main():
    global ENGINE
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", type=Path, default=ROOT / "build/httpd")
    ap.add_argument("--engine", choices=("pool", "loop"), default="pool")
    ap.add_argument("--idle-expiry", action="store_true")
    args = ap.parse_args()
    ENGINE = args.engine
    binary = args.binary.resolve(strict=True)
    for test in (forward_proxy, retry_and_framing, load_balancing, half_open):
        test(binary)
        print(f"PASS {test.__name__}", flush=True)
    if args.idle_expiry:
        idle_expiry(binary)
        print("PASS idle_expiry", flush=True)


if __name__ == "__main__":
    main()
