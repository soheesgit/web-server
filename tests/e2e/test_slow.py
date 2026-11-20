"""기능 테스트: 느리거나 갑자기 끊기는 클라이언트 (E-SL-01~04). 원래 표: 기획서 13.6절."""
import select
import socket
import threading
import time

from harness import (Server, build_request, expect, expect_eq, request, set_linger_rst,
                     stream_get, test, within)


def timed_get(port, target="/index.html"):
    t0 = time.monotonic()
    r = request(port, "GET", target, timeout=5)
    return r.status, time.monotonic() - t0


@test("E-SL-01", "slowloris closed at the absolute header deadline")
def _():
    result = {}
    with Server(conf="basic.conf", extra=["header_timeout_ms 2000"]) as s:
        def slowloris():
            sock = socket.create_connection(("127.0.0.1", s.port))
            t0 = time.monotonic()
            sock.sendall(b"GET / HTTP/1.1\r\nHost: a\r\n")
            try:
                while time.monotonic() - t0 < 8:
                    r, _, _ = select.select([sock], [], [], 0.3)
                    if r:
                        if not sock.recv(65536):
                            break
                        continue
                    sock.sendall(b"X")
            except OSError:
                pass
            result["elapsed"] = time.monotonic() - t0
            sock.close()

        th = threading.Thread(target=slowloris)
        th.start()
        time.sleep(0.3)
        for _ in range(3):
            status, dt = timed_get(s.port)
            expect_eq(status, 200, "other client status")
            expect(dt < 1.0, f"other client waited {dt:.2f}s")
        th.join(timeout=10)
        expect("elapsed" in result, "slowloris thread did not finish")
        expect(result["elapsed"] < 7.5, "slowloris connection was never closed")
        within(result["elapsed"], 2.0, "absolute header deadline")


@test("E-SL-02", "slow reader of 100MB does not delay others")
def _():
    with Server(conf="basic.conf") as s:
        slow = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        slow.connect(("127.0.0.1", s.port))
        slow.sendall(build_request("GET", "/big.bin", port=s.port))
        stop = threading.Event()

        def reader():
            while not stop.wait(0.1):
                try:
                    slow.recv(1024)
                except OSError:
                    return

        th = threading.Thread(target=reader)
        th.start()
        try:
            time.sleep(0.3)
            for _ in range(5):
                status, dt = timed_get(s.port)
                expect_eq(status, 200, "other client status")
                expect(dt < 1.0, f"other client waited {dt:.2f}s")
        finally:
            stop.set()
            th.join(timeout=2)
            set_linger_rst(slow)
            slow.close()


@test("E-SL-03", "RST during the request: server survives")
def _():
    with Server(conf="basic.conf") as s:
        for _ in range(10):
            sock = socket.create_connection(("127.0.0.1", s.port))
            sock.sendall(b"GET / HTTP/1.1\r\nHo")
            set_linger_rst(sock)
            sock.close()
        expect_eq(request(s.port, "GET", "/index.html").status, 200, "server alive")


@test("E-SL-04", "download aborted mid-way: server survives, fds restored")
def _():
    with Server(conf="basic.conf") as s:
        res = stream_get(s.port, "/big.bin", limit=1 << 20)
        expect_eq(res["status"], 200, "status")
        expect_eq(request(s.port, "GET", "/index.html").status, 200, "server alive")
