"""보조 E2E: 공격·경계 입력 (기획서 ID 밖, R-xx). make robust가 pool·loop 두 엔진으로 돌린다.

판정 기준은 RFC 9110/9112와 src/의 헤더 주석 계약이다. 업스트림 쪽 이상 응답은 요청마다
정해진 바이트를 그대로 보내는 RawBackend(스레드)로 만든다. 실행기는 tests/e2e/run_e2e.py.
"""
import os
import random
import socket
import threading
import time

import fake_backend
import harness
from harness import Conn, Server, expect, expect_eq, raw, read_fixture, test

CONF = os.path.join(os.path.dirname(os.path.abspath(__file__)), "robust.conf")
POOL_SLACK = 8  # 프록시 테스트: 업스트림 유휴 연결(pool_max_idle 기본 8)이 fd로 남는다
SMUGGLED = b"GET /index.html HTTP/1.1\r\nHost: a\r\n\r\n"


class RawBackend:
    """요청 i번째마다 reply(req, i)가 돌려준 바이트를 그대로 보내는 백엔드.

    reply가 None이면 그 연결을 닫는다. drip > 0이면 1바이트씩 drip초 간격으로 보낸다.
    """

    def __init__(self, reply, drip=0.0):
        self.reply = reply
        self.drip = drip
        self.requests = []
        self._lock = threading.Lock()
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind(("127.0.0.1", 0))
        self._sock.listen(64)
        self.addr = f"127.0.0.1:{self._sock.getsockname()[1]}"

    def __enter__(self):
        threading.Thread(target=self._accept, daemon=True).start()
        return self

    def __exit__(self, *exc):
        try:
            self._sock.shutdown(socket.SHUT_RDWR)  # 막혀 있는 accept를 깨운다
        except OSError:
            pass
        self._sock.close()
        return False

    def _accept(self):
        while True:
            try:
                s, _ = self._sock.accept()
            except OSError:
                return
            threading.Thread(target=self._serve, args=(s,), daemon=True).start()

    def _serve(self, s):
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        conn = fake_backend.Conn(s)
        try:
            while True:
                req = conn.read_request()
                with self._lock:
                    self.requests.append(req)
                    i = len(self.requests) - 1
                data = self.reply(req, i)
                if data is None:
                    return
                if self.drip:
                    for b in data:
                        s.sendall(bytes([b]))
                        time.sleep(self.drip)
                else:
                    s.sendall(data)
        except (EOFError, OSError):
            pass
        finally:
            s.close()


def site():
    """정적 + CGI 서버. 프록시 upstream은 쓰지 않는 주소(127.0.0.1:9)로 채운다."""
    return Server(conf=CONF, subst={"BACKEND1": "127.0.0.1:9"})


def proxied(backend, fd_slack=POOL_SLACK):
    return Server(conf=CONF, subst={"BACKEND1": backend.addr}, fd_slack=fd_slack)


def responses_in(data):
    """원시 응답 바이트에 상태 줄이 몇 개 있는가 (밀반입된 두 번째 응답을 센다)."""
    return sum(1 for line in data.split(b"\n") if line.startswith(b"HTTP/1."))


def one_error(port, req, allowed, what):
    """요청 하나에 오류 응답 하나만 오고 연결이 닫히는지 본다."""
    data = raw(port, req, timeout=3)
    expect(data, f"{what}: no response")
    r = harness.parse_response(data)
    expect(r.status in allowed, f"{what}: expected {allowed}, got {r.status}")
    expect_eq(responses_in(data), 1, f"{what}: responses (smuggled request answered?)")


# ── 요청 밀반입·파싱 ──────────────────────────────────────────────────────────

@test("R-01", "CL + TE request is rejected and the smuggled request never runs")
def r01():
    body = b"0\r\n\r\n" + SMUGGLED
    req = (b"POST /cgi-bin/echo.py HTTP/1.1\r\nHost: a\r\nContent-Length: %d\r\n"
           b"Transfer-Encoding: chunked\r\n\r\n" % len(body)) + body
    with site() as s:
        one_error(s.port, req, (400, 501), "CL+TE")


@test("R-02", "Transfer-Encoding spellings are all rejected")
def r02():
    variants = [b"Transfer-Encoding: chunked", b"Transfer-Encoding : chunked",
                b"Transfer-Encoding:\tchunked", b"transfer-encoding: CHUNKED",
                b"Transfer-Encoding: xchunked", b"Transfer-Encoding: chunked, identity",
                b"Transfer-Encoding: identity", b"Transfer-Encoding:\r\n chunked",
                b"Transfer-Encoding: chunked\r\nTransfer-Encoding: chunked"]
    with site() as s:
        for v in variants:
            req = (b"POST /index.html HTTP/1.1\r\nHost: a\r\n" + v +
                   b"\r\nContent-Length: 5\r\n\r\n0\r\n\r\n" + SMUGGLED)
            one_error(s.port, req, (400, 501), repr(v))


@test("R-03", "malformed or conflicting Content-Length is rejected")
def r03():
    variants = [b"Content-Length: 5\r\nContent-Length: 6", b"Content-Length: +5",
                b"Content-Length: -1", b"Content-Length: 5 5", b"Content-Length: 0x5",
                b"Content-Length: 5.0", b"Content-Length: 5,6", b"Content-Length:",
                b"Content-Length: 99999999999999999999999"]
    with site() as s:
        for v in variants:
            req = b"POST /index.html HTTP/1.1\r\nHost: a\r\n" + v + b"\r\n\r\nhello" + SMUGGLED
            one_error(s.port, req, (400, 413), repr(v))


@test("R-04", "malformed request line and header bytes get 400 and a closed connection")
def r04():
    bad = [b"GET / HTTP/1.1\r\nHost: a\r\nX: 1\r\n folded\r\n\r\n",
           b"GET / HTTP/1.1\r\nHost: a\r\nX: a\rInjected: 1\r\n\r\n",
           b"GET / HTTP/1.1\r\nHost: a\r\nX: a\x00b\r\n\r\n",
           b"GET / HTTP/1.1\r\nHost : a\r\n\r\n",
           b"GET / HTTP/1.1\r\nHost: a\r\nNoColon\r\n\r\n",
           b"GET / HTTP/1.1\r\nHost: a\r\nX@Y: 1\r\n\r\n",
           b"GET / HTTP/1.1\r\nHost: a\r\n: empty\r\n\r\n",
           b"GET\t/ HTTP/1.1\r\nHost: a\r\n\r\n",
           b"GET /\x01 HTTP/1.1\r\nHost: a\r\n\r\n",
           b"GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n",
           b"GET / HTTP/1.1\r\n\r\n"]
    with site() as s:
        for req in bad:
            with Conn(s.port, timeout=3) as c:
                c.send(req + SMUGGLED)
                r = c.read_response()
                expect_eq(r.status, 400, repr(req))
                expect(c.is_closed(timeout=2), f"{req!r}: connection left open after 400")


@test("R-05", "path traversal spellings never leave the document root")
def r05():
    # root는 tests/fixtures/www, 노리는 파일은 그 옆 tests/fixtures/conf/basic.conf
    targets = ["/../conf/basic.conf", "/%2e%2e/conf/basic.conf", "/%2E%2E%2Fconf%2Fbasic.conf",
               "/..%2fconf/basic.conf", "/dir/../../conf/basic.conf", "/./../conf/basic.conf",
               "//../conf/basic.conf", "/dir/%2e%2e/%2e%2e/conf/basic.conf",
               "/%252e%252e/conf/basic.conf", "/%c0%ae%c0%ae/conf/basic.conf",
               "/..\\conf\\basic.conf", "/..%5cconf%5cbasic.conf", "/escape",
               "/escape/", "/%2e%2e%00/conf/basic.conf", "/dir/..%00/x",
               "http://127.0.0.1/../conf/basic.conf"]
    with site() as s:
        for t in targets:
            data = raw(s.port, f"GET {t} HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n"
                       .encode(), timeout=3)
            r = harness.parse_response(data)
            expect(r.status in (400, 403, 404), f"{t}: status {r.status}")
            expect(b"listen" not in r.body, f"{t}: leaked basic.conf")


@test("R-06", "request sent one byte at a time (head and body) is answered correctly")
def r06():
    with site() as s, Conn(s.port, timeout=5) as c:
        c.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        for b in b"GET /index.html HTTP/1.1\r\nHost: a\r\n\r\n":
            c.send(bytes([b]))
            time.sleep(0.002)
        r = c.read_response()
        expect_eq((r.status, r.body), (200, read_fixture("index.html")), "trickled GET")
        body = b"a=1&b=2"
        req = harness.build_request("POST", "/cgi-bin/echo.py", body=body)
        for b in req:
            c.send(bytes([b]))
            time.sleep(0.002)
        r = c.read_response()
        expect_eq(r.status, 200, "trickled POST status")
        expect(b"BODY=a=1&b=2\n" in r.body, f"trickled POST body: {r.body!r}")


@test("R-07", "50 pipelined requests in one write come back in order")
def r07():
    plan = [("GET", "/index.html", "index.html"), ("HEAD", "/style.css", "style.css"),
            ("GET", "/app.js", "app.js"), ("GET", "/dir/", "dir/index.html"),
            ("GET", "/logo.png", "logo.png")]
    seq = [plan[i % len(plan)] for i in range(50)]
    data = b"".join(harness.build_request(m, t) for m, t, _ in seq)
    with site() as s, Conn(s.port, timeout=5) as c:
        c.send(data)
        for i, (m, t, f) in enumerate(seq):
            r = c.read_response(m)
            want = read_fixture(f)
            expect_eq(r.status, 200, f"#{i} {m} {t} status")
            expect_eq(r.body, b"" if m == "HEAD" else want, f"#{i} {m} {t} body")
            expect_eq(r.header("Content-Length"), str(len(want)), f"#{i} {t} Content-Length")


@test("R-08", "a malformed request inside a pipeline ends the connection after its 400")
def r08():
    data = (harness.build_request("GET", "/index.html") +
            b"GET / HTTP/1.1\r\nHost: a\r\nBad Header\r\n\r\n" +
            harness.build_request("GET", "/style.css"))
    with site() as s:
        out = raw(s.port, data, timeout=3)
    rs = harness.parse_responses(out, ["GET", "GET"])
    expect_eq([r.status for r in rs], [200, 400], "statuses")
    expect_eq(responses_in(out), 2, "responses after the bad request")


@test("R-09", "pipelined POST body boundary: the next request is parsed after exactly CL bytes")
def r09():
    body = b"x" * 1000 + b"GET /secret HTTP/1.1\r\n\r\n"
    data = (harness.build_request("POST", "/cgi-bin/echo.py", body=body) +
            harness.build_request("GET", "/app.js"))
    with site() as s, Conn(s.port, timeout=5) as c:
        c.send(data)
        r1 = c.read_response()
        r2 = c.read_response()
    expect(r1.body.endswith(b"BODY=" + body + b"\n"), "POST echoed its whole body")
    expect_eq((r2.status, r2.body), (200, read_fixture("app.js")), "request after the body")


@test("R-10", "client half-close after the request still gets the full response")
def r10():
    with site() as s:
        out = raw(s.port, harness.build_request("GET", "/index.html"), timeout=3,
                  half_close=True)
    r = harness.parse_response(out)
    expect_eq((r.status, r.body), (200, read_fixture("index.html")), "response")


# ── CGI ───────────────────────────────────────────────────────────────────────

def cgi_env(body):
    env = {}
    for line in body.split(b"\n"):
        if b"=" in line and not line.startswith(b"BODY="):
            k, v = line.split(b"=", 1)
            env[k.decode()] = v.decode("latin-1")
    return env


@test("R-11", "CGI environment: meta-variables set, Proxy header never becomes HTTP_PROXY")
def r11():
    with site() as s:
        r = harness.request(s.port, "POST", "/cgi-bin/echo.py?a=1&b=%20x",
                            headers=[("Proxy", "http://evil:1"), ("X-Custom", "v 1"),
                                     ("Content-Type", "text/x")], body=b"hello")
    env = cgi_env(r.body)
    expect_eq(r.status, 200, "status")
    for k, v in [("REQUEST_METHOD", "POST"), ("QUERY_STRING", "a=1&b=%20x"),
                 ("CONTENT_LENGTH", "5"), ("CONTENT_TYPE", "text/x"), ("HTTP_X_CUSTOM", "v 1"),
                 ("SCRIPT_NAME", "/cgi-bin/echo.py"), ("SERVER_PROTOCOL", "HTTP/1.1")]:
        expect_eq(env.get(k), v, k)
    expect("HTTP_PROXY" not in env, f"httpoxy: HTTP_PROXY={env.get('HTTP_PROXY')!r}")
    expect("HTTP_CONTENT_LENGTH" not in env, "Content-Length duplicated as HTTP_CONTENT_LENGTH")


@test("R-12", "HEAD to a CGI sends no body, so the next pipelined response parses")
def r12():
    data = harness.build_request("HEAD", "/cgi-bin/echo.py") + \
        harness.build_request("GET", "/index.html")
    with site() as s, Conn(s.port, timeout=5) as c:
        c.send(data)
        r1 = c.read_response("HEAD")
        expect_eq(r1.status, 200, "HEAD status")
        if (r1.header("Connection") or "").lower() == "close":
            expect(c.is_closed(timeout=2), "said close but kept the connection")  # RFC 9112 9.6
            return
        r2 = c.read_response("GET")
    expect_eq((r2.status, r2.body), (200, read_fixture("index.html")), "next response")


@test("R-13", "binary POST body (NUL, CR, LF, 0xff) reaches the CGI byte for byte")
def r13():
    body = bytes(random.Random(13).randrange(256) for _ in range(4096))
    with site() as s:
        r = harness.request(s.port, "POST", "/cgi-bin/echo.py", body=body)
    expect_eq(r.status, 200, "status")
    expect(r.body.endswith(b"BODY=" + body + b"\n"), "body changed on the way to the CGI")


@test("R-14", "Expect: 100-continue POST works whether or not the server sends 100")
def r14():
    with site() as s, Conn(s.port, timeout=5) as c:
        c.send(b"POST /cgi-bin/echo.py HTTP/1.1\r\nHost: a\r\nExpect: 100-continue\r\n"
               b"Content-Length: 4\r\n\r\n")
        time.sleep(0.3)
        c.send(b"ping")
        r = c.read_response()
        while 100 <= r.status < 200:
            r = c.read_response()
    expect_eq(r.status, 200, "final status")
    expect(r.body.endswith(b"BODY=ping\n"), f"body {r.body!r}")


# ── 업스트림 이상 응답 (프록시) ────────────────────────────────────────────────

def ok(body):
    return b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n%s" % (len(body), body)


def final_response(c, method="GET"):
    r = c.read_response(method)
    while 100 <= r.status < 200:
        r = c.read_response(method)
    return r


@test("R-15", "upstream response preceded by an empty line, head ending exactly at the read")
def r15():
    replies = [b"\r\nHTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n",
               b"\nHTTP/1.1 204 No Content\r\n\r\n"]
    for reply in replies:
        with RawBackend(lambda req, i, reply=reply: reply) as b, proxied(b) as s:
            r = harness.request(s.port, "GET", "/up/x")
            expect(r.status in (200, 204), f"{reply!r}: status {r.status}")


@test("R-16", "interim 1xx responses before the final one are handled")
def r16():
    reply = (b"HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Early Hints\r\nLink: </s.css>\r\n\r\n" +
             ok(b"final"))
    for drip in (0.0, 0.001):
        with RawBackend(lambda req, i: reply, drip=drip) as b, proxied(b) as s, \
                Conn(s.port) as c:
            c.send(harness.build_request("GET", "/up/x"))
            r = final_response(c)
            expect_eq((r.status, r.body), (200, b"final"), f"drip {drip}")


@test("R-17", "chunked upstream body with extensions and trailers, then keep-alive reuse")
def r17():
    chunked = (b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
               b"4;name=val\r\nWiki\r\n5;x\r\npedia\r\n0\r\nX-Trailer: t\r\n\r\n")
    for drip in (0.0, 0.001):
        with RawBackend(lambda req, i: chunked if i % 2 == 0 else ok(b"two"), drip=drip) as b, \
                proxied(b) as s, Conn(s.port) as c:
            c.send(harness.build_request("GET", "/up/a"))
            r1 = final_response(c)
            c.send(harness.build_request("GET", "/up/b"))
            r2 = final_response(c)
            expect_eq((r1.status, r1.body), (200, b"Wikipedia"), f"chunked body, drip {drip}")
            expect_eq((r2.status, r2.body), (200, b"two"), f"next response, drip {drip}")


@test("R-18", "bytes after the upstream body never become the next client response")
def r18():
    def reply(req, i):
        return ok(b"ok") + b"HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nforged" if i == 0 \
            else ok(b"two")
    with RawBackend(reply) as b, proxied(b) as s, Conn(s.port) as c:
        c.send(harness.build_request("GET", "/up/a"))
        r1 = final_response(c)
        c.send(harness.build_request("GET", "/up/b"))
        r2 = final_response(c)
    expect_eq((r1.status, r1.body), (200, b"ok"), "first")
    expect(r2.body != b"forged", "leftover upstream bytes were served as the second response")
    expect_eq(r2.status, 200, "second status")


@test("R-19", "upstream sending both Content-Length and chunked: TE wins or 502, never CL")
def r19():
    reply = (b"HTTP/1.1 200 OK\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n"
             b"5\r\nhello\r\n0\r\n\r\n")
    with RawBackend(lambda req, i: reply) as b, proxied(b) as s:
        r = harness.request(s.port, "GET", "/up/x")
    expect(r.status == 502 or r.body == b"hello", f"got {r.status} {r.body!r}")
    expect(not (r.header("Content-Length") and r.chunked), "client got both CL and chunked")


@test("R-20", "upstream body sent to a HEAD request is not passed on as the next response")
def r20():
    def reply(req, i):
        return ok(b"hello") if req.line.startswith("HEAD") else ok(b"two")
    with RawBackend(reply) as b, proxied(b) as s, Conn(s.port) as c:
        c.send(harness.build_request("HEAD", "/up/a"))
        r1 = c.read_response("HEAD")
        c.send(harness.build_request("GET", "/up/b"))
        r2 = final_response(c)
    expect_eq(r1.status, 200, "HEAD status")
    expect_eq((r2.status, r2.body), (200, b"two"), "GET after HEAD")


@test("R-21", "bare CR inside an upstream header never reaches the client")
def r21():
    reply = b"HTTP/1.1 200 OK\r\nX-A: a\rInjected: 1\r\nContent-Length: 2\r\n\r\nok"
    with RawBackend(lambda req, i: reply) as b, proxied(b) as s:
        out = raw(s.port, harness.build_request("GET", "/up/x", [("Connection", "close")]),
                  timeout=3)
    head = out.split(b"\r\n\r\n", 1)[0]
    expect(b"\rInjected" not in head and b"\nInjected" not in head, f"head {head!r}")


@test("R-22", "LF-only upstream response is accepted")
def r22():
    with RawBackend(lambda req, i: b"HTTP/1.1 200 OK\nContent-Length: 2\n\nok") as b, \
            proxied(b) as s:
        r = harness.request(s.port, "GET", "/up/x")
    expect_eq((r.status, r.body), (200, b"ok"), "response")


# ── 동시성과 끊김 ─────────────────────────────────────────────────────────────

@test("R-23", "32 concurrent keep-alive clients get their own bytes (static + CGI)")
def r23():
    files = ["index.html", "style.css", "app.js", "logo.png", "photo.jpg"]
    want = {f: read_fixture(f) for f in files}
    errors = []

    def client(n):
        try:
            with Conn(s.port, timeout=10) as c:
                for k in range(4):
                    f = files[(n + k) % len(files)]
                    c.send(harness.build_request("GET", "/" + f))
                    r = c.read_response()
                    if (r.status, r.body) != (200, want[f]):
                        errors.append(f"client {n} {f}: {r.status} {len(r.body)}B")
                    token = f"t{n}-{k}".encode()
                    c.send(harness.build_request("POST", "/cgi-bin/echo.py", body=token))
                    r = c.read_response()
                    if not r.body.endswith(b"BODY=" + token + b"\n"):
                        errors.append(f"client {n} cgi: {r.body[-40:]!r}")
        except Exception as e:  # noqa: BLE001  (스레드 예외를 모아 본 스레드에서 실패시킨다)
            errors.append(f"client {n}: {type(e).__name__}: {e}")

    with site() as s:
        th = [threading.Thread(target=client, args=(n,)) for n in range(32)]
        for t in th:
            t.start()
        for t in th:
            t.join(25)
    expect(not errors, "; ".join(errors[:5]))


@test("R-25", "large uploads never stall: 40 connections each POST 2 MiB and all get a response")
def r25():
    # v2는 한 번의 이벤트에서 읽기를 8번까지만 하고 돌려보낸다. 마지막 읽기에서 요청이 완성되면 파싱하지
    # 않은 채 다음 이벤트를 기다려, 커널에 읽을 것이 없으니 30초 뒤 408이 났다 (확률적, 2 MiB에서 약 1/10).
    body = bytes((i * 13) % 251 for i in range(2 << 20))
    with site() as s:
        conns = []
        for i in range(40):
            c = Conn(s.port, timeout=5)
            conns.append(c)
            c.send(harness.build_request("POST", "/cgi-bin/echo.py", body=body, port=s.port))
            try:
                r = c.read_response()
            except (TimeoutError, EOFError) as e:
                raise AssertionError(f"upload {i} of 40 got no response: {type(e).__name__}")
            expect_eq(r.status, 200, f"upload {i} status")
            expect(r.body.endswith(b"BODY=" + body + b"\n"), f"upload {i} echoed the whole body")
        for c in conns:
            c.close()


@test("R-24", "abrupt client disconnects leak no fds or children")
def r24():
    with site() as s:
        for _ in range(20):  # 머리 중간에 끊기
            with socket.create_connection(("127.0.0.1", s.port)) as sk:
                sk.sendall(b"GET /index.html HTTP/1.1\r\nHo")
        for _ in range(10):  # 큰 응답을 조금 받고 RST
            sk = socket.create_connection(("127.0.0.1", s.port))
            sk.sendall(harness.build_request("GET", "/big.bin"))
            sk.recv(65536)
            harness.set_linger_rst(sk)
            sk.close()
        for _ in range(10):  # CGI 본문을 덜 보내고 끊기
            with socket.create_connection(("127.0.0.1", s.port)) as sk:
                sk.sendall(b"POST /cgi-bin/echo.py HTTP/1.1\r\nHost: a\r\n"
                           b"Content-Length: 100000\r\n\r\npartial")
        r = harness.request(s.port, "GET", "/index.html")
        expect_eq(r.status, 200, "server still answers")
        expect(s.wait_no_children(5), f"CGI children left: {s.children()}")
