"""기능 테스트: 잘못된 요청 (E-ER-01~14). 원래 표: 기획서 13.5절."""
import random
import time

from harness import (Conn, Server, expect, expect_eq, parse_response, raw, read_fixture, request,
                     test, within)


def one(port, data, timeout=5):
    """원시 요청을 보내고 (응답, 서버가 닫았는가)를 돌려준다."""
    with Conn(port, timeout=timeout) as c:
        c.send(data)
        r = c.read_response()
        return r, c.is_closed(2)


def expect_close(r, closed):
    expect_eq((r.header("Connection") or "").lower(), "close", "Connection")
    expect(closed, "server did not close the connection")


@test("E-ER-01", "garbage request line: 400 + close")
def _():
    with Server(conf="basic.conf") as s:
        r, closed = one(s.port, b"GARBAGE\r\n\r\n")
        expect_eq(r.status, 400, "status")
        expect_close(r, closed)


@test("E-ER-02", "HTTP/1.1 without Host: 400")
def _():
    with Server(conf="basic.conf") as s:
        r, _ = one(s.port, b"GET / HTTP/1.1\r\n\r\n")
        expect_eq(r.status, 400, "status")


@test("E-ER-03", "HTTP/2.0 request line: 505")
def _():
    with Server(conf="basic.conf") as s:
        r, _ = one(s.port, b"GET / HTTP/2.0\r\nHost: a\r\n\r\n")
        expect_eq(r.status, 505, "status")


@test("E-ER-04", "PUT and DELETE: 501")
def _():
    with Server(conf="basic.conf") as s:
        for m in ("PUT", "DELETE"):
            expect_eq(request(s.port, m, "/index.html").status, 501, f"{m} status")


@test("E-ER-05", "10KB URI: 414")
def _():
    with Server(conf="basic.conf") as s:
        expect_eq(request(s.port, "GET", "/" + "a" * 10240).status, 414, "status")


@test("E-ER-06", "1MB of headers: 431")
def _():
    lines = "".join(f"X-Pad-{i}: {'a' * 1000}\r\n" for i in range(1024))
    data = f"GET / HTTP/1.1\r\nHost: a\r\n{lines}\r\n".encode()
    with Server(conf="basic.conf") as s:
        r = parse_response(raw(s.port, data))
        expect_eq(r.status, 431, "status")


@test("E-ER-07", "Content-Length 100MB: 413 + close before the body")
def _():
    data = b"POST /index.html HTTP/1.1\r\nHost: a\r\nContent-Length: 104857600\r\n\r\n"
    with Server(conf="basic.conf") as s:
        r, closed = one(s.port, data)
        expect_eq(r.status, 413, "status")
        expect_close(r, closed)


@test("E-ER-08", "Content-Length + Transfer-Encoding: 400 + close")
def _():
    data = (b"POST /index.html HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n"
            b"Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n")
    with Server(conf="basic.conf") as s:
        r, closed = one(s.port, data)
        expect_eq(r.status, 400, "status")
        expect_close(r, closed)


@test("E-ER-09", "chunked request body: 501")
def _():
    data = b"POST /index.html HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n"
    with Server(conf="basic.conf") as s:
        r, _ = one(s.port, data)
        expect_eq(r.status, 501, "status")


@test("E-ER-10", "body shorter than Content-Length: 408 at the body timeout")
def _():
    data = b"POST /cgi-bin/echo.py HTTP/1.1\r\nHost: a\r\nContent-Length: 100\r\n\r\n0123456789"
    with Server(conf="basic.conf", extra=["body_timeout_ms 1000"]) as s:
        t0 = time.monotonic()
        r, closed = one(s.port, data, timeout=6)
        elapsed = time.monotonic() - t0
        expect_eq(r.status, 408, "status")
        expect_close(r, closed)
        within(elapsed, 1.0, "body timeout")


@test("E-ER-11", "partial head: 408 at the header timeout")
def _():
    with Server(conf="basic.conf", extra=["header_timeout_ms 1000"]) as s:
        t0 = time.monotonic()
        r, closed = one(s.port, b"GET / HTTP/1.1\r\nHost: a\r\n", timeout=6)
        elapsed = time.monotonic() - t0
        expect_eq(r.status, 408, "status")
        expect_close(r, closed)
        within(elapsed, 1.0, "header timeout")


@test("E-ER-12", "64KB of random bytes: 400 or close, server survives")
def _():
    junk = random.Random(12).getrandbits(8 * 65536).to_bytes(65536, "little")
    with Server(conf="basic.conf") as s:
        data = raw(s.port, junk)
        if data:
            expect_eq(parse_response(data).status, 400, "status")
        expect_eq(request(s.port, "GET", "/index.html").status, 200, "server alive")


@test("E-ER-13", "space before header colon: 400")
def _():
    with Server(conf="basic.conf") as s:
        r, _ = one(s.port, b"GET / HTTP/1.1\r\nHost : a\r\n\r\n")
        expect_eq(r.status, 400, "status")


@test("E-ER-14", "LF-only line endings: 200")
def _():
    with Server(conf="basic.conf") as s:
        r, _ = one(s.port, b"GET /index.html HTTP/1.1\nHost: a\n\n")
        expect_eq(r.status, 200, "status")
        expect_eq(r.body, read_fixture("index.html"), "body")
