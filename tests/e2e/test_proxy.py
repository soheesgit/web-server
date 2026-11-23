"""기능 테스트: 리버스 프록시 경로 (E-PX-01~17). 원래 표: 기획서 13.3절.

proxy.conf: upstream app = BACKEND1~3 (/api/ strip), admin = BACKEND4 (Host: admin.local). D-016.
"""
import contextlib
import hashlib
import json
import random
import time

import fake_backend
from harness import (Backend, Conn, Server, _parse_head, backends, build_request, expect,
                     expect_eq, request, stream_get, test, within)

HOP = {"connection", "keep-alive", "proxy-connection", "te", "trailer", "transfer-encoding",
       "upgrade", "proxy-authenticate", "proxy-authorization"}
POOL_SLACK = 8 * 4  # 백엔드당 유휴 연결 상한 × 백엔드 수


@contextlib.contextmanager
def proxy(app_modes, extra=(), fd_slack=POOL_SLACK, admin=("id",)):
    """app 백엔드 3대(+ admin 1대)와 프록시 서버를 띄운다. (server, [app1, app2, app3, admin])."""
    specs = [Backend(*m, name=f"app{i + 1}") for i, m in enumerate(app_modes)]
    specs.append(Backend(*admin, name="admin"))
    with backends(*specs) as bs:
        subst = {f"BACKEND{i + 1}": b.addr for i, b in enumerate(bs)}
        with Server(conf="proxy.conf", subst=subst, extra=extra, fd_slack=fd_slack) as s:
            yield s, bs


def echoed(r):
    expect_eq(r.status, 200, "status")
    return json.loads(r.body)


@test("E-PX-01", "round robin over 3 backends, in order")
def _():
    with proxy([("id",)] * 3) as (s, _):
        ids = [request(s.port, "GET", "/api/whoami").header("X-Backend-Id") for _ in range(9)]
        expect_eq(ids, ["app1", "app2", "app3"] * 3, "backend order")


@test("E-PX-02", "prefix strip: backend sees /whoami")
def _():
    with proxy([("echo",)] * 3) as (s, _):
        e = echoed(request(s.port, "GET", "/api/whoami"))
        expect_eq(e["target"], "/whoami", "backend target")
        expect_eq(e["version"], "HTTP/1.1", "backend version")


@test("E-PX-03", "hop-by-hop removed, X-Forwarded-For appended")
def _():
    with proxy([("echo",)] * 3) as (s, _):
        e = echoed(request(s.port, "GET", "/api/x", headers=[
            ("Connection", "keep-alive"), ("Keep-Alive", "timeout=5"),
            ("X-Forwarded-For", "1.1.1.1")]))
        names = [k.lower() for k, _ in e["headers"]]
        bad = sorted(set(names) & HOP)
        expect(not bad, f"hop-by-hop headers reached backend: {bad}")
        xff = [v for k, v in e["headers"] if k.lower() == "x-forwarded-for"]
        expect_eq(xff, ["1.1.1.1, 127.0.0.1"], "X-Forwarded-For")


@test("E-PX-04", "5MB POST arrives intact")
def _():
    data = random.Random(4).getrandbits(8 * 5 * 1024 * 1024).to_bytes(5 * 1024 * 1024, "little")
    with proxy([("echo",)] * 3) as (s, _):
        e = echoed(request(s.port, "POST", "/api/upload", body=data, timeout=20))
        expect_eq(e["body_len"], len(data), "backend body length")
        expect_eq(e["body_sha256"], hashlib.sha256(data).hexdigest(), "sha256")


@test("E-PX-05", "chunked backend response relayed")
def _():
    with proxy([("chunked",)] * 3) as (s, _):
        r = request(s.port, "GET", "/api/x")
        expect_eq(r.status, 200, "status")
        expect_eq(r.body, b"".join(fake_backend.CHUNKS), "body")


@test("E-PX-06", "close-delimited backend response, then connection closes")
def _():
    with proxy([("close",)] * 3) as (s, _):
        with Conn(s.port) as c:
            c.send(build_request("GET", "/api/x", port=s.port))
            r = c.read_response()
            expect_eq(r.status, 200, "status")
            expect_eq(r.body, fake_backend.CLOSE_BODY, "body")
            expect(c.is_closed(2), "connection not closed after close-delimited body")


@test("E-PX-07", "100MB backend body: same bytes, RSS growth < 20MB")
def _():
    n = 104857600
    with proxy([("big", n)] * 3) as (s, _):
        res = stream_get(s.port, "/api/big", sample_pid=s.proc.pid)
        expect_eq(res["status"], 200, "status")
        expect_eq(res["nbytes"], n, "bytes")
        expect_eq(res["sha256"], fake_backend.big_sha256(n), "sha256")
        expect(res["rss_growth_kb"] < 20 * 1024,
               f"proxy RSS grew {res['rss_growth_kb']} KB (limit 20480)")


@test("E-PX-08", "dead backend: retried, excluded, then back after cooldown")
def _():
    with proxy([("id",)] * 3, extra=["backend_cooldown_ms 1500"]) as (s, bs):
        port2 = bs[1].port
        bs[1].stop()
        ids = []
        for _ in range(30):
            r = request(s.port, "GET", "/api/x")
            expect_eq(r.status, 200, "status while app2 is down")
            ids.append(r.header("X-Backend-Id"))
        expect("app2" not in ids, "app2 answered while down")
        with Backend("id", name="app2", port=port2):
            time.sleep(2.0)
            ids = {request(s.port, "GET", "/api/x").header("X-Backend-Id") for _ in range(9)}
            expect_eq(ids, {"app1", "app2", "app3"}, "backends after recovery")


@test("E-PX-09", "all backends down: 502, then 503 once excluded")
def _():
    with proxy([("id",)] * 3) as (s, bs):
        for b in bs[:3]:
            b.stop()
        first = request(s.port, "GET", "/api/x").status
        expect_eq(first, 502, "first status")
        seen = [request(s.port, "GET", "/api/x").status for _ in range(5)]
        expect(set(seen) <= {502, 503}, f"unexpected statuses {seen}")
        expect(503 in seen, f"never 503 after exclusion: {seen}")


@test("E-PX-10", "silent backend: 504 at the response timeout")
def _():
    with proxy([("silent",)] * 3, extra=["upstream_response_timeout_ms 1000"]) as (s, _):
        t0 = time.monotonic()
        r = request(s.port, "GET", "/api/x")
        elapsed = time.monotonic() - t0
        expect_eq(r.status, 504, "status")
        within(elapsed, 1.0, "upstream response timeout")


@test("E-PX-11", "broken status line is 502")
def _():
    with proxy([("broken",)] * 3) as (s, _):
        expect_eq(request(s.port, "GET", "/api/x").status, 502, "status")


@test("E-PX-12", "body cut mid-way: client sees short body, proxy survives")
def _():
    n = 1000000
    with proxy([("truncate", n)] * 3) as (s, _):
        with Conn(s.port, timeout=10) as c:
            c.send(build_request("GET", "/api/x", port=s.port))
            _, status, _, hdrs = _parse_head(c.rd.read_head())
            expect_eq(status, 200, "status")
            body = c.rd.read_to_eof()
            cl = [v for k, v in hdrs if k.lower() == "content-length"]
            if cl:
                expect(len(body) < int(cl[0]), "body was not cut short")
            else:
                expect(len(body) < n, "body was not cut short")
        expect_eq(request(s.port, "GET", "/index.html").status, 200, "proxy alive")


@test("E-PX-13", "POST to a backend that drops: 502, sent once")
def _():
    with proxy([("drop",)] * 3) as (s, bs):
        r = request(s.port, "POST", "/api/x", body=b"p" * 100)
        expect_eq(r.status, 502, "status")
        total = sum(b.stats()["requests"] for b in bs[:3])
        expect_eq(total, 1, "POSTs received by backends")


@test("E-PX-14", "100 sequential requests reuse upstream connections")
def _():
    with proxy([("id",)] * 3) as (s, bs):
        for _ in range(100):
            r = request(s.port, "GET", "/api/x", headers=[("Connection", "close")])
            expect_eq(r.status, 200, "status")
        conns = sum(b.stats()["connections"] for b in bs[:3])
        expect(conns < 100, f"backends accepted {conns} connections for 100 requests")


@test("E-PX-15", "Host: admin.local goes to the admin backend")
def _():
    with proxy([("id",)] * 3) as (s, _):
        r = request(s.port, "GET", "/anything", headers=[("Host", "admin.local")])
        expect_eq(r.status, 200, "status")
        expect_eq(r.header("X-Backend-Id"), "admin", "backend")


@test("E-PX-16", "backend 404 and 500 passed through")
def _():
    with proxy([("id",)] * 3) as (s, _):
        for code in (404, 500):
            r = request(s.port, "GET", f"/api/__status/{code}")
            expect_eq(r.status, code, "status")
            expect_eq(r.body, f"status {code}\n".encode(), "body")


@test("E-PX-17", "client disconnect mid-relay closes the upstream connection")
def _():
    with proxy([("big", 104857600)] * 3, fd_slack=0) as (s, _):
        res = stream_get(s.port, "/api/big", limit=1 << 20)
        expect_eq(res["status"], 200, "status")
        expect_eq(request(s.port, "GET", "/index.html").status, 200, "proxy alive")


# E-FW-01은 기획서 밖의 포워드 프록시(D-041)를 확인한다. 기획서 표에 없는 ID다.
@test("E-FW-01", "forward proxy: absolute-form GET reaches the named host")
def _():
    with backends(Backend("echo")) as (b,), Server(conf="forward.conf", fd_slack=8) as s:
        e = echoed(request(s.port, "GET", f"http://{b.addr}/hello?x=1"))
        expect_eq(e["target"], "/hello?x=1", "origin-form target at the destination")
        hosts = [v for k, v in e["headers"] if k.lower() == "host"]
        expect_eq(hosts, [b.addr], "Host at the destination")
        # 같은 목적지는 keep-alive 풀을 함께 쓴다: 두 번째도 같은 백엔드가 받는다
        expect_eq(echoed(request(s.port, "GET", f"http://{b.addr}/again"))["target"], "/again",
                  "second request")
        # 목적지가 없는 origin-form 요청은 포워드할 수 없다
        expect_eq(request(s.port, "GET", "/hello").status, 400, "origin-form status")
        # 닫힌 포트로 가는 요청은 502 (다른 목적지로 재시도하지 않는다)
        expect_eq(request(s.port, "GET", "http://127.0.0.1:1/x").status, 502, "dead destination")
