#!/usr/bin/env python3
"""LLM 트래픽 부하 생성기 (Python 3 표준 라이브러리만).

열린 루프(--rate, 포아송 도착) 또는 닫힌 루프(--concurrency, 동시 N개 유지)로
bench/fake_llm.py 형식의 요청을 보내고, 요청마다 한 줄 JSON을 남긴다.

시각은 time.clock_gettime(CLOCK_MONOTONIC) (ms)로 재서 fake_llm의 기록과 비교할 수 있다.

    loadgen.py --target 127.0.0.1:8080 --scenario B --rate 4 --duration 60 --out b.jsonl
    loadgen.py --target ... --scenario long --concurrency 32 --duration 30 --out e1.jsonl
    loadgen.py --target ... --scenario A --rate 2 --duration 10 --abort-after-ms 500 ...

요청 한 줄: id, cls, status, t_send, head_ms, ttft_ms, itl_med_ms, itl_max_ms, total_ms,
out_tokens, backend, backend_timing(fake_llm 마지막 이벤트), aborted, error, warmup.
끝나면 stderr에 요약표(클래스별 p50/p90/p99)를 쓴다. 처음 --warmup초 동안 보낸 요청은
기록하되 요약에서 뺀다 (warmup: true).
"""
import argparse
import asyncio
import json
import random
import statistics
import sys
import time

# 시나리오: (클래스 이름, 비중, 입력 토큰, 출력 토큰, 세션 방식)
SCENARIOS = {
    "A": [("short", 1.0, 500, 100, None)],
    # B의 긴 요청은 기획서(5만/2000)의 1/10: 노트북 1B 보정 계수에 맞춘 크기 (bench/README.md)
    "B": [("short", 0.9, 500, 100, None), ("long", 0.1, 5000, 500, None)],
    # C: 대화 32개가 번갈아 이어진다. 턴마다 앞부분이 그대로 반복된다
    "C": [("turn", 1.0, 2000, 200, "multiturn")],
    "long": [("stream", 1.0, 200, 1500, None)],  # E1: 오래 열린 스트림
    "tiny": [("tiny", 1.0, 50, 20, None)],
    "prefill": [("prefill", 1.0, 3000, 100, None)],  # E0: prefill 도중 끊기
}


def now_ms():
    return time.clock_gettime(time.CLOCK_MONOTONIC) * 1000.0


def pct(xs, p):
    xs = sorted(x for x in xs if x is not None)
    if not xs:
        return None
    k = (len(xs) - 1) * p
    lo, hi = int(k), min(int(k) + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


class Gen:
    def __init__(self, a):
        self.a = a
        self.rng = random.Random(a.seed)
        self.mix = SCENARIOS[a.scenario]
        self.sessions = {}  # 대화 → 지금까지 쌓인 토큰
        self.n = 0
        self.out = open(a.out, "w") if a.out else None
        self.rows = []
        self.t_start = now_ms()

    def pick(self):
        r = self.rng.random() * sum(m[1] for m in self.mix)
        for m in self.mix:
            r -= m[1]
            if r <= 0:
                return m
        return self.mix[-1]

    def make(self):
        name, _, inp, out, mode = self.pick()
        self.n += 1
        req = {"input_tokens": inp, "max_tokens": out, "stream": True}
        if mode == "multiturn":
            conv = f"c{self.rng.randrange(self.a.conversations)}"
            hist = self.sessions.get(conv, 0)
            req.update(session=conv, prefix_tokens=hist, input_tokens=hist + inp)
            self.sessions[conv] = hist + inp + out
        return self.n, name, req

    def record(self, row):
        row["warmup"] = row.get("t_send", self.t_start) < self.t_start + self.a.warmup * 1000
        self.rows.append(row)
        if self.out:
            self.out.write(json.dumps(row) + "\n")


async def one(g, rid, cls, req):
    a = g.a
    host, port = a.target.rsplit(":", 1)
    body = json.dumps(req).encode()
    row = {"id": rid, "cls": cls, "status": None, "t_send": now_ms(), "head_ms": None,
           "ttft_ms": None, "itl_med_ms": None, "itl_max_ms": None, "total_ms": None,
           "out_tokens": 0, "backend": None, "backend_timing": None, "aborted": False,
           "error": None, "input_tokens": req["input_tokens"]}
    t0 = row["t_send"]
    writer = None
    try:
        reader, writer = await asyncio.open_connection(host, int(port))
        writer.write(b"POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\n"
                     b"Content-Length: %d\r\nConnection: close\r\n\r\n%s"
                     % (a.path.encode(), host.encode(), len(body), body))
        await writer.drain()
        head = await reader.readuntil(b"\r\n\r\n")
        row["head_ms"] = now_ms() - t0
        lines = head.decode("latin-1").split("\r\n")
        row["status"] = int(lines[0].split(" ")[1])
        hdr = {}
        for ln in lines[1:]:
            if ":" in ln:
                k, v = ln.split(":", 1)
                hdr[k.strip().lower()] = v.strip()
        row["backend"] = hdr.get("x-backend-id")
        if row["status"] != 200 or "event-stream" not in hdr.get("content-type", ""):
            await reader.read()  # 에러 본문은 버린다
            return
        times = []
        deadline = t0 + a.abort_after_ms if a.abort_after_ms else None
        buf = b""
        while True:
            if deadline is not None:
                left = (deadline - now_ms()) / 1000.0
                if left <= 0:
                    row["aborted"] = True
                    row["t_abort"] = now_ms()
                    return
                data = await asyncio.wait_for(reader.read(65536), timeout=left)
            else:
                data = await reader.read(65536)
            if not data:
                break
            t = now_ms()
            buf += data
            # chunked 틀은 무시하고 SSE 줄만 본다 (data: 줄이 청크 경계를 넘어도 버퍼로 잇는다)
            while b"\n\n" in buf:
                ev, buf = buf.split(b"\n\n", 1)
                for ln in ev.split(b"\n"):
                    ln = ln.strip()
                    if not ln.startswith(b"data: "):
                        continue
                    payload = ln[6:]
                    if payload == b"[DONE]":
                        continue
                    obj = json.loads(payload)
                    if "i" in obj:
                        times.append(t)
                    elif obj.get("done"):
                        row["backend_timing"] = obj.get("timing")
                        row["backend"] = obj.get("backend", row["backend"])
        row["out_tokens"] = len(times)
        if times:
            row["ttft_ms"] = times[0] - t0
            gaps = [y - x for x, y in zip(times, times[1:])]
            if gaps:
                row["itl_med_ms"] = statistics.median(gaps)
                row["itl_max_ms"] = max(gaps)
    except asyncio.TimeoutError:
        row["aborted"] = True
        row["t_abort"] = now_ms()
    except (OSError, asyncio.IncompleteReadError, ValueError) as e:
        row["error"] = type(e).__name__
    finally:
        row["total_ms"] = now_ms() - t0
        if writer is not None:
            writer.close()
        g.record(row)


async def open_loop(g):
    a = g.a
    end = now_ms() + a.duration * 1000
    tasks = set()
    while now_ms() < end:
        await asyncio.sleep(g.rng.expovariate(a.rate))
        if len(tasks) >= a.max_inflight:
            g.record({"id": None, "cls": "dropped", "status": None, "error": "max_inflight"})
            continue
        rid, cls, req = g.make()
        t = asyncio.ensure_future(one(g, rid, cls, req))
        tasks.add(t)
        t.add_done_callback(tasks.discard)
    if tasks:
        await asyncio.wait(tasks)


async def closed_loop(g):
    a = g.a
    end = now_ms() + a.duration * 1000

    async def worker():
        while now_ms() < end:
            rid, cls, req = g.make()
            await one(g, rid, cls, req)

    await asyncio.gather(*(worker() for _ in range(a.concurrency)))


def summary(g):
    rows = [r for r in g.rows if r.get("cls") != "dropped" and not r["warmup"]]
    dropped = sum(1 for r in g.rows if r.get("cls") == "dropped")
    warm = sum(1 for r in g.rows if r["warmup"] and r.get("cls") != "dropped")
    print(f"requests {len(rows)} (warmup excluded {warm})  dropped {dropped}", file=sys.stderr)
    print(f"{'class':>8} {'n':>5} {'ok':>5} {'err':>4} {'5xx':>4}  "
          f"{'ttft p50':>9} {'p90':>8} {'p99':>8}  {'total p50':>9} {'p99':>8}  {'itl_max p99':>11}",
          file=sys.stderr)
    for cls in sorted({r["cls"] for r in rows}):
        rs = [r for r in rows if r["cls"] == cls]
        ok = [r for r in rs if r["status"] == 200 and not r["error"]]

        def f(v):
            return f"{v:.0f}" if v is not None else "-"
        print(f"{cls:>8} {len(rs):>5} {len(ok):>5} {sum(1 for r in rs if r['error']):>4} "
              f"{sum(1 for r in rs if (r['status'] or 0) >= 500):>4}  "
              f"{f(pct([r['ttft_ms'] for r in ok], .5)):>9} {f(pct([r['ttft_ms'] for r in ok], .9)):>8} "
              f"{f(pct([r['ttft_ms'] for r in ok], .99)):>8}  "
              f"{f(pct([r['total_ms'] for r in ok], .5)):>9} {f(pct([r['total_ms'] for r in ok], .99)):>8}  "
              f"{f(pct([r['itl_max_ms'] for r in ok], .99)):>11}", file=sys.stderr)
    per = {}
    for r in rows:
        if r.get("backend"):
            per[r["backend"]] = per.get(r["backend"], 0) + 1
    if per:
        print("per backend: " + "  ".join(f"{k}={v}" for k, v in sorted(per.items())),
              file=sys.stderr)


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--target", required=True, help="host:port")
    p.add_argument("--path", default="/v1/completions")
    p.add_argument("--scenario", default="A", choices=sorted(SCENARIOS))
    p.add_argument("--rate", type=float, help="초당 요청 (열린 루프)")
    p.add_argument("--concurrency", type=int, help="동시 요청 수 (닫힌 루프)")
    p.add_argument("--duration", type=float, default=30, help="초 (도착을 만드는 시간)")
    p.add_argument("--warmup", type=float, default=10, help="초. 이 동안 보낸 요청은 요약에서 뺌")
    p.add_argument("--conversations", type=int, default=32, help="시나리오 C의 대화 수")
    p.add_argument("--abort-after-ms", type=float, default=0, help="요청 뒤 이만큼 지나면 끊음")
    p.add_argument("--max-inflight", type=int, default=2000)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--out", help="요청별 JSONL")
    a = p.parse_args()
    if (a.rate is None) == (a.concurrency is None):
        p.error("--rate 또는 --concurrency 중 하나만")
    g = Gen(a)
    asyncio.run(open_loop(g) if a.rate else closed_loop(g))
    if g.out:
        g.out.close()
    summary(g)


if __name__ == "__main__":
    main()
