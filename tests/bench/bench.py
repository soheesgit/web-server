#!/usr/bin/env python3
"""v1 스레드 풀(pool)과 v2 이벤트 루프(loop)를 같은 조건으로 잰다.

실험 1 idle: 아무것도 보내지 않는 연결 K개를 붙여 둔 채, 새 연결로 GET을 보내 응답까지 걸린
             시간과 상태 코드를 잰다. 연결 하나가 워커 하나를 붙잡는 모델(v1)과 아닌 모델(v2)의
             차이가 드러난다.
실험 2 load: 동시 연결 C개가 keep-alive로 GET을 반복한다. 초당 요청 수와 지연(p50·p99)을 잰다.

사용: python3 tests/bench/bench.py --bin build/httpd [--idle 0,100,1000] [--conns 32,256]
결과는 표로 출력한다. 클라이언트도 같은 기계에서 돌기 때문에 절대값보다 두 엔진의 차이를 본다.
지연은 200 응답만 센다. 재연결은 블로킹 connect라 동시 연결이 수천 개면 클라이언트가 병목이 된다.
"""
import argparse
import multiprocessing as mp
import os
import re
import selectors
import socket
import subprocess
import time

REQ = b"GET /index.html HTTP/1.1\r\nHost: bench\r\n\r\n"
CL_RE = re.compile(rb"\r\ncontent-length:\s*(\d+)", re.I)
REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def start_server(binary, engine, port):
    proc = subprocess.Popen([binary, "-p", str(port), "-r", os.path.join(REPO, "tests/fixtures/www"),
                             "-e", engine], cwd=REPO, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            return proc
        except OSError:
            time.sleep(0.05)
    proc.kill()
    raise RuntimeError("server did not start")


def response_done(buf):
    """응답 하나가 끝났으면 (전체 길이, 상태 코드, 닫힘 여부), 아니면 None."""
    end = buf.find(b"\r\n\r\n")
    if end < 0:
        return None
    head = buf[:end]
    m = CL_RE.search(head)
    total = end + 4 + (int(m.group(1)) if m else 0)
    if len(buf) < total:
        return None
    status = int(head[9:12])
    return total, status, b"connection: close" in head.lower()


def probe(port, timeout):
    """새 연결로 GET 한 번. (지연 ms, 상태) — 시간 안에 못 받으면 상태 None."""
    t0 = time.monotonic()
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        s.settimeout(max(0.01, timeout - (time.monotonic() - t0)))
        s.sendall(REQ)
        buf = b""
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
            done = response_done(buf)
            if done:
                s.close()
                return (time.monotonic() - t0) * 1000, done[1]
        s.close()
        return (time.monotonic() - t0) * 1000, "closed"
    except (socket.timeout, OSError):
        return (time.monotonic() - t0) * 1000, None


def idle_test(binary, engine, idle, probes, timeout):
    port = free_port()
    proc = start_server(binary, engine, port)
    holders = []
    try:
        for _ in range(idle):
            holders.append(socket.create_connection(("127.0.0.1", port), timeout=5))
        time.sleep(0.5)  # 엔진이 accept해 워커·루프에 올릴 시간
        return [probe(port, timeout) for _ in range(probes)]
    finally:
        for h in holders:
            h.close()
        proc.terminate()
        proc.wait(timeout=10)


def load_worker(port, nconns, seconds, out):
    sel = selectors.DefaultSelector()
    lat = []
    errors = 0
    non200 = 0

    def open_conn():
        s = socket.create_connection(("127.0.0.1", port))
        s.setblocking(False)
        s.sendall(REQ)
        sel.register(s, selectors.EVENT_READ, {"buf": b"", "t0": time.monotonic()})

    for _ in range(nconns):
        open_conn()
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        for key, _ in sel.select(timeout=0.1):
            s, st = key.fileobj, key.data
            try:
                chunk = s.recv(65536)
            except BlockingIOError:
                continue
            except OSError:
                chunk = b""
            if not chunk:
                sel.unregister(s)
                s.close()
                errors += 1 if st["buf"] else 0
                open_conn()
                continue
            st["buf"] += chunk
            done = response_done(st["buf"])
            if not done:
                continue
            now = time.monotonic()
            if done[1] == 200:
                lat.append((now - st["t0"]) * 1000)
            else:
                non200 += 1
            if done[2]:  # 서버가 닫는 응답이면 새 연결로 이어 간다
                sel.unregister(s)
                s.close()
                open_conn()
                continue
            st["buf"] = st["buf"][done[0]:]
            st["t0"] = now
            s.sendall(REQ)
    for key in list(sel.get_map().values()):
        key.fileobj.close()
    out.put((lat, errors, non200))


def load_test(binary, engine, conns, seconds, procs):
    port = free_port()
    proc = start_server(binary, engine, port)
    try:
        q = mp.Queue()
        per = [conns // procs + (1 if i < conns % procs else 0) for i in range(procs)]
        workers = [mp.Process(target=load_worker, args=(port, n, seconds, q)) for n in per if n]
        for w in workers:
            w.start()
        lat, errors, non200 = [], 0, 0
        for _ in workers:
            l, e, n = q.get()
            lat += l
            errors += e
            non200 += n
        for w in workers:
            w.join()
        lat.sort()
        pick = (lambda p: lat[min(len(lat) - 1, int(len(lat) * p))]) if lat else (lambda p: 0)
        return len(lat) / seconds, pick(0.5), pick(0.99), errors, non200
    finally:
        proc.terminate()
        proc.wait(timeout=10)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=os.path.join(REPO, "build", "httpd"))
    ap.add_argument("--idle", default="0,100,1000")
    ap.add_argument("--probes", type=int, default=5)
    ap.add_argument("--probe-timeout", type=float, default=12.0)
    ap.add_argument("--conns", default="32,256")
    ap.add_argument("--seconds", type=float, default=5.0)
    ap.add_argument("--procs", type=int, default=4)
    a = ap.parse_args()

    print("## 실험 1 idle: 유휴 연결 K개가 붙어 있을 때 새 GET의 응답 시간")
    print("| 엔진 | 유휴 K | 응답 시간 ms (중앙값 / 최대) | 상태 |")
    print("|---|---|---|---|")
    for k in [int(x) for x in a.idle.split(",")]:
        for engine in ("pool", "loop"):
            res = idle_test(a.bin, engine, k, a.probes, a.probe_timeout)
            ms = sorted(r[0] for r in res)
            codes = ",".join(str(r[1]) if r[1] is not None else "timeout" for r in res)
            print(f"| {engine} | {k} | {ms[len(ms) // 2]:.1f} / {ms[-1]:.1f} | {codes} |", flush=True)

    print()
    print(f"## 실험 2 load: 동시 연결 C개 keep-alive GET, {a.seconds:g}초")
    print("| 엔진 | 동시 C | 200 응답/초 | p50 ms | p99 ms | 200 아닌 응답 | 끊긴 응답 |")
    print("|---|---|---|---|---|---|---|")
    for c in [int(x) for x in a.conns.split(",")]:
        for engine in ("pool", "loop"):
            rps, p50, p99, err, bad = load_test(a.bin, engine, c, a.seconds, a.procs)
            print(f"| {engine} | {c} | {rps:,.0f} | {p50:.2f} | {p99:.2f} | {bad} | {err} |",
                  flush=True)


if __name__ == "__main__":
    main()
