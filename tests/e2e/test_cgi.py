"""기능 테스트: CGI 경로 (E-CG-01~10). 원래 표: 기획서 13.2절."""
import hashlib
import threading
import time

from harness import Conn, Server, build_request, expect, expect_eq, request, test, within

BIG = 10485760  # big.sh 출력 (D-020)


@test("E-CG-01", "GET echo.py?x=1 sees QUERY_STRING")
def _():
    with Server(conf="basic.conf") as s:
        r = request(s.port, "GET", "/cgi-bin/echo.py?x=1")
        expect_eq(r.status, 200, "status")
        expect(b"QUERY_STRING=x=1\n" in r.body, f"body lacks QUERY_STRING=x=1: {r.body[:200]!r}")
        expect(b"REQUEST_METHOD=GET\n" in r.body, "body lacks REQUEST_METHOD=GET")


@test("E-CG-02", "form POST body reaches the script")
def _():
    with Server(conf="basic.conf") as s:
        r = request(s.port, "POST", "/cgi-bin/echo.py", body=b"name=kim",
                    headers=[("Content-Type", "application/x-www-form-urlencoded")])
        expect_eq(r.status, 200, "status")
        expect(b"BODY=name=kim\n" in r.body, f"body lacks name=kim: {r.body[-200:]!r}")
        expect(b"CONTENT_LENGTH=8\n" in r.body, "body lacks CONTENT_LENGTH=8")


@test("E-CG-03", "1MB POST is echoed back without deadlock")
def _():
    data = b"0123456789abcdef" * 65536
    with Server(conf="basic.conf") as s:
        r = request(s.port, "POST", "/cgi-bin/echo.py", body=data, timeout=20)
        expect_eq(r.status, 200, "status")
        expect(b"BODY=" + data + b"\n" in r.body, "echoed body differs")


@test("E-CG-04", "Status: 201 from status.sh")
def _():
    with Server(conf="basic.conf") as s:
        r = request(s.port, "GET", "/cgi-bin/status.sh")
        expect_eq(r.status, 201, "status")
        expect_eq(r.header("X-Cgi"), "1", "X-Cgi")


@test("E-CG-05", "incomplete CGI head (broken.sh) is 502")
def _():
    with Server(conf="basic.conf") as s:
        expect_eq(request(s.port, "GET", "/cgi-bin/broken.sh").status, 502, "status")


@test("E-CG-06", "CGI exiting 1 without output is 502")
def _():
    with Server(conf="basic.conf") as s:
        expect_eq(request(s.port, "GET", "/cgi-bin/exit1.sh").status, 502, "status")


@test("E-CG-07", "CGI timeout is 504 and the child is gone")
def _():
    with Server(conf="basic.conf", extra=["cgi_timeout_ms 1000"]) as s:
        t0 = time.monotonic()
        r = request(s.port, "GET", "/cgi-bin/sleep.sh")
        elapsed = time.monotonic() - t0
        expect_eq(r.status, 504, "status")
        within(elapsed, 1.0, "CGI timeout")
        expect(s.wait_no_children(3), f"children left: {s.children()}")


@test("E-CG-08", "10MB CGI output: chunked to 1.1, close-delimited to 1.0")
def _():
    want = hashlib.sha256(b"\0" * BIG).hexdigest()
    with Server(conf="basic.conf") as s:
        r = request(s.port, "GET", "/cgi-bin/big.sh", timeout=20)
        expect_eq(r.status, 200, "1.1 status")
        expect(r.chunked, "1.1 response is not chunked")
        expect_eq(len(r.body), BIG, "1.1 body length")
        expect_eq(hashlib.sha256(r.body).hexdigest(), want, "1.1 sha256")
        r = request(s.port, "GET", "/cgi-bin/big.sh", version="1.0", timeout=20)
        expect_eq(r.status, 200, "1.0 status")
        expect(r.header("Transfer-Encoding") is None, "1.0 response has Transfer-Encoding")
        expect_eq(len(r.body), BIG, "1.0 body length")
        expect_eq(hashlib.sha256(r.body).hexdigest(), want, "1.0 sha256")


@test("E-CG-09", "client disconnect during CGI reaps the child")
def _():
    with Server(conf="basic.conf") as s:
        c = Conn(s.port)
        c.send(build_request("GET", "/cgi-bin/sleep.sh", port=s.port))
        deadline = time.monotonic() + 3
        while not s.children() and time.monotonic() < deadline:
            time.sleep(0.05)
        expect(s.children(), "CGI child never started")
        c.close()
        expect(s.wait_no_children(3), f"children left after disconnect: {s.children()}")


@test("E-CG-10", "20 concurrent CGI requests all 200, no zombies")
def _():
    results = [None] * 20
    with Server(conf="basic.conf") as s:
        def worker(i):
            try:
                r = request(s.port, "GET", f"/cgi-bin/echo.py?i={i}", timeout=20)
                results[i] = r.status == 200 and f"QUERY_STRING=i={i}\n".encode() in r.body
            except Exception:  # noqa: BLE001
                results[i] = False
        ths = [threading.Thread(target=worker, args=(i,)) for i in range(20)]
        for th in ths:
            th.start()
        for th in ths:
            th.join(timeout=25)
        expect(all(results), f"failed requests: {[i for i, ok in enumerate(results) if not ok]}")
        expect(s.wait_no_children(3), f"children left: {s.children()}")
