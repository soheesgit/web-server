#!/usr/bin/env python3
"""가짜 LLM 백엔드 (Python 3 표준 라이브러리만). 기획서 FR-FB-01~06.

GPU 없이 LLM 서빙의 핵심 현상을 흉내 낸다. 계수의 근거는
bench/calib/results-2026-11-24-m4-llama3.2-1b/SUMMARY.md.

  - 배치: 매 단계 실행 중인 요청이 모두 토큰 하나씩 진행. 단계 시간 = step_base +
    step_per_seq × (배치 크기 - 1) + 그 단계에 새로 들어온 요청들의 prefill 시간
    (긴 prefill이 진행 중인 decode를 막는다 — 보정 [4])
  - prefill: 캐시 안 된 입력 토큰 × prefill_ms_per_token
  - 슬롯·KV 상한: 슬롯이 차거나 (입력 + 지금까지 출력) 합이 kv_cap을 넘으면 내부 대기.
    단계 중 KV가 넘치면 가장 최근에 들어온 요청을 선점해 대기열 앞으로 (다시 prefill)
  - prefix 캐시: 세션별 LRU. 적중한 앞부분 토큰은 prefill에서 뺀다

사용법:
    fake_llm.py --port N --id NAME [--slots 4] [--kv-cap 32768] [--time-scale 1.0] ...

기동하면 stdout에 "READY <port>" 한 줄을 쓴다.

요청: POST (경로 무관), 본문 JSON
    {"input_tokens": 500, "max_tokens": 100, "stream": true,
     "session": "c-1", "prefix_tokens": 400}
    session·prefix_tokens는 선택: 같은 세션의 앞부분 prefix_tokens가 캐시에 있으면 적중.
응답 (stream): 200, text/event-stream, chunked. 헤더 X-Backend-Id, X-Queue-Waiting,
    X-KV-Usage, X-Cache-Hit-Tokens. 토큰마다 `data: {"i": k}`, 마지막에
    `data: {"done": true, "usage": {...}, "timing": {...}}`와 `data: [DONE]`.
응답 (stream false): 200 JSON {"usage": ..., "timing": ...}
GET /__stats: 누적 통계 JSON. GET /__events: 끊김 감지 기록 JSON (E0용).
"""
import argparse
import asyncio
import collections
import json
import sys
import time


def now_ms():
    # 프로세스 사이에서 비교할 수 있는 시계 (macOS의 time.monotonic은 프로세스마다 기준이 다르다)
    return time.clock_gettime(time.CLOCK_MONOTONIC) * 1000.0


class Req:
    def __init__(self, rid, input_tokens, max_tokens, session, prefix_tokens):
        self.rid = rid
        self.input_tokens = input_tokens
        self.max_tokens = max_tokens
        self.session = session
        self.prefix_tokens = prefix_tokens
        self.generated = 0
        self.cached = 0  # 이번 prefill에서 캐시로 건너뛴 토큰
        self.out = asyncio.Queue()  # 토큰 번호, 끝이면 None
        self.cancelled = False
        self.arrived = now_ms()
        self.admitted = None
        self.first_token = None
        self.prefill_ms = 0.0
        self.preempted = 0

    def kv(self):
        return self.input_tokens + self.generated


class Engine:
    def __init__(self, a):
        self.a = a
        self.waiting = collections.deque()
        self.running = []
        self.kv_used = 0
        self.cache = collections.OrderedDict()  # session → 캐시된 앞부분 토큰 수
        self.cache_used = 0
        self.wake = asyncio.Event()
        self.stats = collections.Counter()
        self.stats["max_running"] = 0
        self.events = []  # 끊김 감지 기록

    # ── prefix 캐시 ──
    def cache_lookup(self, r):
        if not r.session or self.a.cache_tokens <= 0:
            return 0
        have = self.cache.get(r.session, 0)
        if have:
            self.cache.move_to_end(r.session)
        return min(have, r.prefix_tokens, r.input_tokens)

    def cache_store(self, r):
        if not r.session or self.a.cache_tokens <= 0:
            return
        n = min(r.input_tokens + r.generated, self.a.cache_tokens)
        self.cache_used -= self.cache.pop(r.session, 0)
        self.cache[r.session] = n
        self.cache_used += n
        while self.cache_used > self.a.cache_tokens:
            _, old = self.cache.popitem(last=False)
            self.cache_used -= old

    # ── 스케줄러 ──
    def submit(self, r):
        self.waiting.append(r)
        self.stats["requests"] += 1
        self.wake.set()

    def usage(self):
        return self.kv_used / self.a.kv_cap if self.a.kv_cap else 0.0

    def _admit(self):
        new_prefill = 0
        while (self.waiting and len(self.running) < self.a.slots and
               self.kv_used + self.waiting[0].kv() <= self.a.kv_cap):
            r = self.waiting.popleft()
            if r.cancelled:
                continue
            hit = self.cache_lookup(r)
            # 선점됐다 돌아온 요청은 지금까지의 출력까지 다시 계산한다
            uncached = r.kv() - hit
            r.cached = hit
            r.prefill_ms += uncached * self.a.prefill_ms_per_token
            if r.admitted is None:
                r.admitted = now_ms()
            new_prefill += uncached
            self.kv_used += r.kv()
            self.running.append(r)
            self.stats["prefill_tokens"] += uncached
            self.stats["cache_hit_tokens"] += hit
        self.stats["max_running"] = max(self.stats["max_running"], len(self.running))
        return new_prefill

    def _drop(self, r):
        self.running.remove(r)
        self.kv_used -= r.kv()

    async def run(self):
        a = self.a
        while True:
            for r in [r for r in self.running if r.cancelled]:
                self._drop(r)
            if not self.running and not self.waiting:
                self.wake.clear()
                await self.wake.wait()
                continue
            new_prefill = self._admit()
            if not self.running:
                # 대기 중인 요청 하나가 KV 상한보다 크다: 거절할 방법이 없으니 잠깐 쉰다
                await asyncio.sleep(0.01)
                continue
            step = (a.step_base_ms + a.step_per_seq_ms * (len(self.running) - 1) +
                    new_prefill * a.prefill_ms_per_token)
            await asyncio.sleep(step * a.time_scale / 1000.0)
            t = now_ms()
            for r in list(self.running):
                if r.cancelled:
                    continue
                r.generated += 1
                self.kv_used += 1
                if r.first_token is None:
                    r.first_token = t
                r.out.put_nowait(r.generated)
                if r.generated >= r.max_tokens:
                    self._drop(r)
                    self.cache_store(r)
                    r.out.put_nowait(None)
                    self.stats["completed"] += 1
            # KV가 넘치면 가장 최근에 들어온 요청부터 선점 (FR-FB-04)
            while self.kv_used > a.kv_cap and len(self.running) > 1:
                victim = self.running[-1]
                self._drop(victim)
                victim.preempted += 1
                self.waiting.appendleft(victim)
                self.stats["preemptions"] += 1


# ── HTTP ─────────────────────────────────────────────────────────────────

async def read_request(reader):
    head = await reader.readuntil(b"\r\n\r\n")
    lines = head.decode("latin-1").split("\r\n")
    method, target, _ = lines[0].split(" ", 2)
    headers = {}
    for ln in lines[1:]:
        if ":" in ln:
            k, v = ln.split(":", 1)
            headers[k.strip().lower()] = v.strip()
    n = int(headers.get("content-length", "0") or 0)
    body = await reader.readexactly(n) if n else b""
    return method, target, headers, body


def chunk(data):
    return b"%x\r\n%s\r\n" % (len(data), data)


def sse(obj):
    return chunk(b"data: " + json.dumps(obj).encode() + b"\n\n")


async def watch_disconnect(reader, r, eng):
    """요청 본문을 다 읽은 뒤 상대가 연결을 닫으면 요청을 취소하고 시각을 남긴다."""
    try:
        data = await reader.read(1)
    except (ConnectionError, asyncio.CancelledError):
        data = b""
    if not data and not r.cancelled and r.generated < r.max_tokens:
        r.cancelled = True
        eng.events.append({"rid": r.rid, "event": "client_gone", "t_ms": now_ms(),
                           "generated": r.generated})
        eng.stats["cancelled"] += 1
        eng.wake.set()


async def handle(reader, writer, eng, a, ids):
    try:
        while True:
            try:
                method, target, headers, body = await read_request(reader)
            except (asyncio.IncompleteReadError, ConnectionError, ValueError):
                return
            if method == "GET" and target.startswith("/__"):
                if target.startswith("/__stats"):
                    st = dict(eng.stats, running=len(eng.running), waiting=len(eng.waiting),
                              kv_used=eng.kv_used, kv_cap=a.kv_cap, cache_used=eng.cache_used)
                    payload = json.dumps(st).encode()
                else:
                    payload = json.dumps(eng.events).encode()
                writer.write(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                             b"Content-Length: %d\r\n\r\n%s" % (len(payload), payload))
                await writer.drain()
                continue
            try:
                q = json.loads(body or b"{}")
                r = Req(next(ids), int(q.get("input_tokens", 100)), int(q.get("max_tokens", 50)),
                        q.get("session"), int(q.get("prefix_tokens", 0)))
            except (ValueError, TypeError):
                writer.write(b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n")
                await writer.drain()
                continue
            stream = q.get("stream", True)
            # 보고 값은 이 요청을 넣기 전의 상태
            hdr = ("HTTP/1.1 200 OK\r\n"
                   f"X-Backend-Id: {a.id}\r\n"
                   f"X-Queue-Waiting: {len(eng.waiting)}\r\n"
                   f"X-KV-Usage: {eng.usage():.3f}\r\n"
                   f"X-Cache-Hit-Tokens: {eng.cache_lookup(r)}\r\n")
            eng.submit(r)
            if stream:
                writer.write((hdr + "Content-Type: text/event-stream\r\nCache-Control: no-cache\r\n"
                              "Transfer-Encoding: chunked\r\n\r\n").encode())
                await writer.drain()
            watcher = asyncio.ensure_future(watch_disconnect(reader, r, eng))
            try:
                while True:
                    k = await r.out.get()
                    if k is None:
                        break
                    if stream:
                        writer.write(sse({"i": k}))
                        await writer.drain()
            except ConnectionError:
                r.cancelled = True
                eng.wake.set()
                return
            # 끝 조각을 쓰기 전에 끊김 감시를 멈춘다 (다음 요청 바이트를 먹지 않게)
            if watcher.done():
                return
            t_end = now_ms()
            watcher.cancel()
            try:
                await watcher
            except asyncio.CancelledError:
                pass
            final = {
                "done": True, "backend": a.id,
                "usage": {"input_tokens": r.input_tokens, "output_tokens": r.generated,
                          "cached_tokens": r.cached},
                "timing": {"queue_ms": (r.admitted or t_end) - r.arrived,
                           "prefill_ms": r.prefill_ms * a.time_scale,
                           "ttft_backend_ms": (r.first_token or t_end) - r.arrived,
                           "total_backend_ms": t_end - r.arrived,
                           "preempted": r.preempted},
            }
            if stream:
                writer.write(sse(final) + chunk(b"data: [DONE]\n\n") + b"0\r\n\r\n")
            else:
                payload = json.dumps(final).encode()
                writer.write((hdr + "Content-Type: application/json\r\n"
                              f"Content-Length: {len(payload)}\r\n\r\n").encode() + payload)
            await writer.drain()
    except ConnectionError:
        return
    finally:
        try:
            writer.close()
        except Exception:
            pass


async def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--id", default="llm")
    p.add_argument("--slots", type=int, default=4)
    p.add_argument("--kv-cap", type=int, default=32768, help="토큰")
    p.add_argument("--cache-tokens", type=int, default=65536, help="prefix 캐시 용량, 0이면 끔")
    p.add_argument("--prefill-ms-per-token", type=float, default=0.9)
    p.add_argument("--step-base-ms", type=float, default=17.0)
    p.add_argument("--step-per-seq-ms", type=float, default=6.3)
    p.add_argument("--time-scale", type=float, default=1.0, help="모든 대기 시간에 곱함")
    a = p.parse_args()

    eng = Engine(a)
    ids = iter(range(1, 1 << 62))
    server = await asyncio.start_server(lambda r, w: handle(r, w, eng, a, ids),
                                        "127.0.0.1", a.port, backlog=512)
    asyncio.ensure_future(eng.run())
    print(f"READY {a.port}", flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
