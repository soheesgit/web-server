"""기능 테스트: 연결 유지 (E-KA-01~07). 원래 표: 기획서 13.4절."""
import time

from harness import Conn, Server, build_request, expect, expect_eq, read_fixture, test, within


@test("E-KA-01", "two requests on one connection")
def _():
    with Server(conf="basic.conf") as s, Conn(s.port) as c:
        c.send(build_request("GET", "/index.html", port=s.port))
        expect_eq(c.read_response().status, 200, "first status")
        expect(not c.is_closed(0.2), "closed after first response")
        c.send(build_request("GET", "/style.css", port=s.port))
        r = c.read_response()
        expect_eq(r.status, 200, "second status")
        expect_eq(r.body, read_fixture("style.css"), "second body")


@test("E-KA-02", "three pipelined requests answered in order")
def _():
    names = ["index.html", "style.css", "app.js"]
    with Server(conf="basic.conf") as s, Conn(s.port) as c:
        c.send(b"".join(build_request("GET", "/" + n, port=s.port) for n in names))
        for n in names:
            r = c.read_response()
            expect_eq(r.status, 200, f"{n} status")
            expect_eq(r.body, read_fixture(n), f"{n} body")


@test("E-KA-03", "HTTP/1.0 request: server closes after response")
def _():
    with Server(conf="basic.conf") as s, Conn(s.port) as c:
        c.send(b"GET /index.html HTTP/1.0\r\n\r\n")
        r = c.read_response()
        expect_eq(r.status, 200, "status")
        expect(c.is_closed(2), "connection not closed")


@test("E-KA-04", "HTTP/1.0 + keep-alive stays open")
def _():
    req = b"GET /index.html HTTP/1.0\r\nConnection: keep-alive\r\n\r\n"
    with Server(conf="basic.conf") as s, Conn(s.port) as c:
        c.send(req)
        r = c.read_response()
        expect_eq(r.status, 200, "first status")
        expect_eq((r.header("Connection") or "").lower(), "keep-alive", "Connection")
        expect(not c.is_closed(0.3), "closed after first response")
        c.send(req)
        expect_eq(c.read_response().status, 200, "second status")


@test("E-KA-05", "Connection: close closes after response")
def _():
    with Server(conf="basic.conf") as s, Conn(s.port) as c:
        c.send(build_request("GET", "/index.html", [("Connection", "close")], port=s.port))
        r = c.read_response()
        expect_eq(r.status, 200, "status")
        expect_eq((r.header("Connection") or "").lower(), "close", "Connection")
        expect(c.is_closed(2), "connection not closed")


@test("E-KA-06", "idle connection closed at the keep-alive timeout")
def _():
    with Server(conf="basic.conf", extra=["keepalive_timeout_ms 1000"]) as s, Conn(s.port) as c:
        c.send(build_request("GET", "/index.html", port=s.port))
        expect_eq(c.read_response().status, 200, "status")
        t0 = time.monotonic()
        closed = c.is_closed(4)
        elapsed = time.monotonic() - t0
        expect(closed, "idle connection not closed")
        within(elapsed, 1.0, "keep-alive timeout")


@test("E-KA-07", "100th response on a connection carries Connection: close")
def _():
    with Server(conf="basic.conf") as s, Conn(s.port) as c:
        req = build_request("GET", "/index.html", port=s.port)
        for i in range(1, 101):
            c.send(req)
            r = c.read_response()
            expect_eq(r.status, 200, f"response {i} status")
            conn_hdr = (r.header("Connection") or "").lower()
            if i < 100:
                expect(conn_hdr != "close", f"response {i} has Connection: close")
            else:
                expect_eq(conn_hdr, "close", "100th Connection")
        try:
            c.send(req)
        except OSError:
            pass
        expect(c.is_closed(2), "connection open after the 100th response")
