"""포워드 프록시 응답 캐시 E2E (D-047). E-FW-02~E-FW-07은 기획서 밖의 ID다 (E-FW-01과 같은 이유).

원본 서버(Origin)가 경로마다 정해진 Cache-Control을 보내고 경로별 요청 횟수를 센다. 본문은 "v<횟수>"라서
캐시에서 나온 응답은 옛 횟수를, 원본까지 간 응답은 새 횟수를 담는다.
"""
import socket
import threading
import time

import fake_backend
import harness
from harness import Server, expect, expect_eq, test

MAX60 = [("Cache-Control", "max-age=60")]


def body_of(path, n):
    if path in ("/a", "/b", "/c"):
        return (f"{path}{n}" * 400)[:1200].encode()
    if path == "/big":
        return bytes((i * 7) % 251 for i in range(2000))
    if path == "/huge":
        return bytes((i * 31 + 5) % 256 for i in range(500_000))
    return f"v{n}".encode()


ROUTES = {
    "/cc": MAX60,
    "/ttl": [("Cache-Control", "max-age=1")],
    "/smax": [("Cache-Control", "max-age=1, s-maxage=60")],
    "/aged": [("Cache-Control", "max-age=5"), ("Age", "5")],
    "/nostore": [("Cache-Control", "no-store, max-age=60")],
    "/private": [("Cache-Control", "private, max-age=60")],
    "/nocache": [("Cache-Control", "no-cache, max-age=60")],
    "/nocc": [],
    "/cookie": MAX60 + [("Set-Cookie", "a=b")],
    "/vary": MAX60 + [("Vary", "Accept")],
    "/a": MAX60, "/b": MAX60, "/c": MAX60, "/big": MAX60, "/huge": MAX60,
}


class Origin:
    """경로별 요청 횟수를 세는 원본 서버 (스레드)."""

    def __init__(self):
        self.counts = {}
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
            self._sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self._sock.close()
        return False

    def count(self, path):
        with self._lock:
            return self.counts.get(path, 0)

    def _accept(self):
        while True:
            try:
                s, _ = self._sock.accept()
            except OSError:
                return
            threading.Thread(target=self._serve, args=(s,), daemon=True).start()

    def _serve(self, s):
        conn = fake_backend.Conn(s)
        try:
            while True:
                req = conn.read_request()
                path = req.target
                with self._lock:
                    self.counts[path] = n = self.counts.get(path, 0) + 1
                status = 404 if path == "/missing" else 200
                headers = MAX60 if path == "/missing" else ROUTES.get(path, [])
                body = b"post" if req.method == "POST" else body_of(path, n)
                head = [f"HTTP/1.1 {status} X", f"Content-Length: {len(body)}"]
                head += [f"{k}: {v}" for k, v in headers]
                data = ("\r\n".join(head) + "\r\n\r\n").encode()
                s.sendall(data if req.method == "HEAD" else data + body)
        except (EOFError, OSError):
            pass
        finally:
            s.close()


def fwd(extra=()):
    return Server(conf="forward.conf", fd_slack=8, extra=list(extra))


def get(s, o, path, headers=(), method="GET"):
    return harness.request(s.port, method, f"http://{o.addr}{path}", headers=list(headers))


def xcache(r):
    return r.header("X-Cache")


@test("E-FW-02", "forward cache: second GET is served from the cache with Age and X-Cache: HIT")
def _():
    with Origin() as o, fwd() as s:
        r1 = get(s, o, "/cc")
        expect_eq((r1.status, r1.body, xcache(r1)), (200, b"v1", "MISS"), "first")
        expect(r1.header("Age") is None, "a fresh fetch has no Age")
        r2 = get(s, o, "/cc")
        expect_eq((r2.status, r2.body, xcache(r2)), (200, b"v1", "HIT"), "second")
        expect(r2.header("Age") is not None, "hit carries Age")
        expect_eq(r2.header("Cache-Control"), "max-age=60", "stored headers are replayed")
        expect_eq(o.count("/cc"), 1, "origin requests")
        # 같은 연결에서 이어 보내는 요청도 적중한다
        with harness.Conn(s.port) as c:
            for _ in range(3):
                c.send(harness.build_request("GET", f"http://{o.addr}/cc", port=s.port))
                expect_eq(c.read_response().body, b"v1", "keep-alive hit")
        expect_eq(o.count("/cc"), 1, "origin requests after keep-alive hits")
        # 호스트 대소문자는 같은 키다
        host = o.addr.replace("127.0.0.1", "127.0.0.1")
        r3 = harness.request(s.port, "GET", f"HTTP://{host}/cc")
        expect_eq(xcache(r3), "HIT", "scheme case-insensitive")


@test("E-FW-03", "forward cache: responses that must not be stored are fetched every time")
def _():
    with Origin() as o, fwd() as s:
        for path in ("/nostore", "/private", "/nocache", "/nocc", "/cookie", "/vary", "/missing",
                     "/aged"):
            r1, r2 = get(s, o, path), get(s, o, path)
            expect_eq(xcache(r1), "MISS", f"{path} first")
            expect_eq(xcache(r2), "MISS", f"{path} second is not a hit")
            expect_eq(o.count(path), 2, f"{path} origin requests")
        # HEAD는 캐시를 쓰지도 채우지도 않는다
        get(s, o, "/cc")
        h = get(s, o, "/cc", method="HEAD")
        expect_eq((h.status, h.body, xcache(h)), (200, b"", None), "HEAD")
        expect_eq(o.count("/cc"), 2, "HEAD reached the origin")


@test("E-FW-04", "forward cache: entries expire; s-maxage wins over max-age")
def _():
    with Origin() as o, fwd() as s:
        expect_eq(get(s, o, "/ttl").body, b"v1", "first")
        expect_eq(xcache(get(s, o, "/ttl")), "HIT", "inside max-age")
        time.sleep(1.3)
        r = get(s, o, "/ttl")
        expect_eq((r.body, xcache(r)), (b"v2", "MISS"), "after max-age")
        # s-maxage=60이 max-age=1보다 우선한다
        get(s, o, "/smax")
        time.sleep(1.3)
        expect_eq(xcache(get(s, o, "/smax")), "HIT", "s-maxage")


@test("E-FW-05", "forward cache: Authorization, no-cache, POST and Range follow the request rules")
def _():
    with Origin() as o, fwd() as s:
        get(s, o, "/cc")  # v1 저장
        r = get(s, o, "/cc", [("Authorization", "Basic eDp5")])
        expect_eq((r.body, xcache(r)), (b"v2", None), "Authorization bypasses and is not stored")
        r = get(s, o, "/cc", [("Range", "bytes=0-1")])
        expect_eq(r.body, b"v3", "Range bypasses")
        r = get(s, o, "/cc", [("Cache-Control", "no-cache")])
        expect_eq((r.body, xcache(r)), (b"v4", "MISS"), "no-cache revalidates at the origin")
        r = get(s, o, "/cc")
        expect_eq((r.body, xcache(r)), (b"v4", "HIT"), "the refreshed copy is served")
        r = get(s, o, "/cc", [("Pragma", "no-cache")])
        expect_eq(r.body, b"v5", "Pragma: no-cache")
        r = get(s, o, "/cc", [("Cache-Control", "no-store")])
        expect_eq((r.body, xcache(r)), (b"v6", None), "no-store neither reads nor stores")
        expect_eq(get(s, o, "/cc").body, b"v5", "the earlier copy is still there")
        # POST는 같은 URL의 항목을 지운다
        expect_eq(get(s, o, "/cc", method="POST").body, b"post", "POST reaches the origin")
        r = get(s, o, "/cc")
        expect_eq((r.body, xcache(r)), (b"v8", "MISS"), "after POST the entry is gone")


@test("E-FW-06", "forward cache: entry limit, total limit with least-recently-used eviction, and off")
def _():
    with Origin() as o, fwd(["cache_entry_max 1500"]) as s:
        get(s, o, "/big"), get(s, o, "/big")
        expect_eq(o.count("/big"), 2, "2000-byte body is over cache_entry_max")
        get(s, o, "/a"), get(s, o, "/a")
        expect_eq(o.count("/a"), 1, "1200-byte body fits")
    with Origin() as o, fwd(["cache_max_bytes 3500"]) as s:
        get(s, o, "/a"), get(s, o, "/b")           # 항목 둘이 들어간다
        expect_eq(xcache(get(s, o, "/a")), "HIT", "a is now the most recent")
        get(s, o, "/c")                             # 셋째가 들어오며 가장 오래된 b를 지운다
        expect_eq(xcache(get(s, o, "/a")), "HIT", "a survived")
        expect_eq(xcache(get(s, o, "/c")), "HIT", "c is stored")
        r = get(s, o, "/b")
        expect_eq((xcache(r), o.count("/b")), ("MISS", 2), "b was evicted")
    with Origin() as o, fwd(["cache_max_bytes 0"]) as s:
        r1, r2 = get(s, o, "/cc"), get(s, o, "/cc")
        expect_eq((xcache(r1), xcache(r2), o.count("/cc")), (None, None, 2), "cache off")


@test("E-FW-07", "forward cache: a 500 KB body comes back byte for byte, also under concurrency")
def _():
    with Origin() as o, fwd() as s:
        want = body_of("/huge", 1)
        r1 = get(s, o, "/huge")
        expect_eq((r1.body == want, xcache(r1)), (True, "MISS"), "first")
        errors = []

        def hit():
            try:
                r = get(s, o, "/huge")
                if r.body != want or xcache(r) != "HIT":
                    errors.append(f"{len(r.body)}B {xcache(r)}")
            except Exception as e:  # noqa: BLE001  (스레드 예외를 모아 본 스레드에서 실패시킨다)
                errors.append(repr(e))

        ths = [threading.Thread(target=hit) for _ in range(8)]
        for t in ths:
            t.start()
        for t in ths:
            t.join(20)
        expect(not errors, "; ".join(errors[:3]))
        expect_eq(o.count("/huge"), 1, "origin requests")
