"""기능 테스트: 정적 파일 경로 (E-ST-01~12). 원래 표: 기획서 13.1절."""
import threading

from harness import (Server, expect, expect_eq, parse_response, raw, read_fixture, request,
                     skip_if_root, stream_get, test)
import harness

ASSETS = [
    ("/style.css", "style.css", "text/css; charset=utf-8"),
    ("/app.js", "app.js", "text/javascript; charset=utf-8"),
    ("/logo.png", "logo.png", "image/png"),
    ("/photo.jpg", "photo.jpg", "image/jpeg"),
]


@test("E-ST-01", "GET / returns index.html")
def _():
    with Server(conf="basic.conf") as s:
        r = request(s.port, "GET", "/")
        body = read_fixture("index.html")
        expect_eq(r.status, 200, "status")
        expect_eq(r.header("Content-Type"), "text/html; charset=utf-8", "Content-Type")
        expect_eq(r.header("Content-Length"), str(len(body)), "Content-Length")
        expect_eq(r.body, body, "body")


@test("E-ST-02", "css, js, png, jpg with right Content-Type and bytes")
def _():
    with Server(conf="basic.conf") as s:
        for target, name, ctype in ASSETS:
            r = request(s.port, "GET", target)
            expect_eq(r.status, 200, f"{target} status")
            expect_eq(r.header("Content-Type"), ctype, f"{target} Content-Type")
            expect_eq(harness.sha256_bytes(r.body), harness.sha256_bytes(read_fixture(name)),
                      f"{target} sha256")


@test("E-ST-03", "HEAD has GET's headers and no body")
def _():
    with Server(conf="basic.conf") as s:
        g = request(s.port, "GET", "/index.html")
        h = request(s.port, "HEAD", "/index.html")
        expect_eq(h.status, 200, "HEAD status")
        for name in ("Content-Type", "Content-Length", "Last-Modified"):
            expect_eq(h.header(name), g.header(name), f"HEAD {name}")
        data = raw(s.port, b"HEAD /index.html HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n")
        expect(b"\r\n\r\n" in data, "no complete head")
        expect_eq(data.split(b"\r\n\r\n", 1)[1], b"", "bytes after HEAD response head")


@test("E-ST-04", "missing file is 404 with HTML body")
def _():
    with Server(conf="basic.conf") as s:
        r = request(s.port, "GET", "/nope.html")
        expect_eq(r.status, 404, "status")
        expect((r.header("Content-Type") or "").startswith("text/html"), "Content-Type not HTML")
        expect(b"404" in r.body, "body lacks 404")


@test("E-ST-05", "secret.txt (mode 000) and escape symlink are 403")
def _():
    with Server(conf="basic.conf") as s:
        expect_eq(request(s.port, "GET", "/escape").status, 403, "/escape status")
        skip_if_root()
        expect_eq(request(s.port, "GET", "/secret.txt").status, 403, "/secret.txt status")


@test("E-ST-06", "traversal and %00 are 400 or 403, never 200")
def _():
    targets = ["/../../etc/passwd", "/%2e%2e/%2e%2e/etc/passwd", "/a%00.html"]
    with Server(conf="basic.conf") as s:
        for t in targets:
            data = raw(s.port, f"GET {t} HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n"
                       .encode())
            r = parse_response(data)
            expect(r.status in (400, 403), f"{t}: expected 400 or 403, got {r.status}")


@test("E-ST-07", "/dir redirects to /dir/, /dir/ serves index")
def _():
    with Server(conf="basic.conf") as s:
        r = request(s.port, "GET", "/dir")
        expect_eq(r.status, 301, "/dir status")
        expect_eq(r.header("Location"), "/dir/", "Location")
        r = request(s.port, "GET", "/dir/")
        expect_eq(r.status, 200, "/dir/ status")
        expect_eq(r.body, read_fixture("dir/index.html"), "/dir/ body")


@test("E-ST-08", "100MB big.bin: same bytes, RSS growth < 20MB")
def _():
    with Server(conf="basic.conf") as s:
        res = stream_get(s.port, "/big.bin", sample_pid=s.proc.pid)
        expect_eq(res["status"], 200, "status")
        expect_eq(res["nbytes"], harness.BIG_SIZE, "bytes")
        expect_eq(res["sha256"], harness.BIG_SHA256, "sha256")
        expect(res["rss_growth_kb"] < 20 * 1024,
               f"server RSS grew {res['rss_growth_kb']} KB (limit 20480)")


@test("E-ST-09", "20 assets in parallel all 200")
def _():
    files = [("/index.html", "index.html")] + [(t, n) for t, n, _ in ASSETS]
    results = [None] * 20
    with Server(conf="basic.conf") as s:
        def worker(i):
            t, n = files[i % len(files)]
            try:
                r = request(s.port, "GET", t)
                results[i] = (r.status == 200 and r.body == read_fixture(n), f"{t} {r.status}")
            except Exception as e:  # noqa: BLE001
                results[i] = (False, f"{t} {type(e).__name__}: {e}")
        ths = [threading.Thread(target=worker, args=(i,)) for i in range(20)]
        for th in ths:
            th.start()
        for th in ths:
            th.join(timeout=20)
        bad = [msg for ok, msg in (r or (False, "no result") for r in results) if not ok]
        expect(not bad, f"failed: {bad}")


@test("E-ST-10", "percent-encoded UTF-8 file name")
def _():
    with Server(conf="basic.conf") as s:
        r = request(s.port, "GET", "/%ED%95%9C%EA%B8%80.txt")
        expect_eq(r.status, 200, "status")
        expect_eq(r.body, "한글\n".encode(), "body")


@test("E-ST-11", "POST to a static file is 405 with Allow")
def _():
    with Server(conf="basic.conf") as s:
        r = request(s.port, "POST", "/index.html", body=b"x")
        expect_eq(r.status, 405, "status")
        expect_eq(r.header("Allow"), "GET, HEAD", "Allow")


@test("E-ST-12", "If-Modified-Since gives 304", optional=True)
def _():
    with Server(conf="basic.conf") as s:
        lm = request(s.port, "GET", "/index.html").header("Last-Modified")
        expect(lm is not None, "no Last-Modified")
        r = request(s.port, "GET", "/index.html", headers=[("If-Modified-Since", lm)])
        expect_eq(r.status, 304, "status")
        expect_eq(r.body, b"", "body")

