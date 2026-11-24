#!/usr/bin/env python3
"""Ollama 보정 측정: prefill 대 입력 길이, decode 대 동시성, prefix 캐시.

표준 라이브러리만. 결과는 표로 출력하고 JSONL로 남긴다.
"""
import http.client
import json
import statistics
import sys
import threading
import time
import uuid

import os
HOST, MODEL = "127.0.0.1", "llama3.2:1b"
PORT = int(os.environ.get("OLLAMA_PORT", "11434"))
OUT = sys.argv[1] if len(sys.argv) > 1 else "calib.jsonl"
WORDS = ("alpha bravo charlie delta echo foxtrot golf hotel india juliet kilo lima mike "
         "november oscar papa quebec romeo sierra tango uniform victor whiskey xray").split()


def filler(n_words, nonce):
    # 앞에 nonce를 둬 prefix 캐시가 맞지 않게 한다
    body = " ".join(WORDS[i % len(WORDS)] for i in range(n_words))
    return f"[{nonce}] {body}\nRepeat the last word."


def generate(prompt, num_predict, num_ctx=8192):
    """스트리밍 요청 하나. 클라이언트 시각과 서버 내부 시간을 함께 돌려준다."""
    conn = http.client.HTTPConnection(HOST, PORT, timeout=600)
    body = json.dumps({"model": MODEL, "prompt": prompt, "stream": True,
                       "options": {"num_predict": num_predict, "num_ctx": num_ctx,
                                   "temperature": 0}})
    t0 = time.monotonic()
    conn.request("POST", "/api/generate", body, {"Content-Type": "application/json"})
    resp = conn.getresponse()
    t_head = time.monotonic()
    t_first = None
    token_times = []
    final = None
    for line in resp:
        if not line.strip():
            continue
        d = json.loads(line)
        now = time.monotonic()
        if d.get("response"):
            token_times.append(now)
            if t_first is None:
                t_first = now
        if d.get("done"):
            final = d
            break
    t_end = time.monotonic()
    conn.close()
    gaps = [b - a for a, b in zip(token_times, token_times[1:])]
    return {
        "ttft_ms": (t_first - t0) * 1000 if t_first else None,
        "head_ms": (t_head - t0) * 1000,
        "total_ms": (t_end - t0) * 1000,
        "itl_ms": statistics.median(gaps) * 1000 if gaps else None,
        "prompt_tokens": final.get("prompt_eval_count"),
        "cached_tokens": final.get("prompt_eval_cached_count"),
        "prefill_ms": final.get("prompt_eval_duration", 0) / 1e6,
        "out_tokens": final.get("eval_count"),
        "decode_ms": final.get("eval_duration", 0) / 1e6,
        "load_ms": final.get("load_duration", 0) / 1e6,
    }


def log(rec):
    with open(OUT, "a") as f:
        f.write(json.dumps(rec) + "\n")


def med(xs):
    xs = [x for x in xs if x is not None]
    return statistics.median(xs) if xs else float("nan")


def exp_prefill(lengths=(32, 128, 512, 1024, 2048, 4096), reps=3):
    print("\n[1] prefill 대 입력 길이 (num_predict=1, 캐시 회피)")
    print(f"{'words':>6} {'tokens':>7} {'prefill_ms':>11} {'ms/token':>9} {'ttft_ms':>8}")
    for n in lengths:
        rows = []
        for _ in range(reps):
            r = generate(filler(n, uuid.uuid4().hex), 1)
            r.update(exp="prefill", words=n)
            log(r)
            rows.append(r)
        tok = med([r["prompt_tokens"] for r in rows])
        pf = med([r["prefill_ms"] for r in rows])
        print(f"{n:>6} {tok:>7.0f} {pf:>11.1f} {pf / tok:>9.3f} {med([r['ttft_ms'] for r in rows]):>8.1f}")


def exp_concurrency(levels=(1, 2, 4, 8), out_tokens=128):
    print(f"\n[2] 동시 요청 수 대 토큰 간격 (짧은 입력, 출력 {out_tokens} 토큰)")
    print(f"{'conc':>5} {'ttft_ms(med/max)':>18} {'itl_ms':>7} {'decode_ms/tok':>14} {'total_ms(max)':>14} {'tok/s(all)':>10}")
    for k in levels:
        res = [None] * k
        start = time.monotonic()

        def run(i):
            res[i] = generate(f"[{uuid.uuid4().hex}] Write a long, detailed essay about the "
                              "history of ships. Use many paragraphs.", out_tokens)

        ths = [threading.Thread(target=run, args=(i,)) for i in range(k)]
        for t in ths:
            t.start()
        for t in ths:
            t.join()
        wall = time.monotonic() - start
        for r in res:
            r.update(exp="concurrency", conc=k)
            log(r)
        ttfts = [r["ttft_ms"] for r in res]
        per_tok = med([r["decode_ms"] / r["out_tokens"] for r in res if r["out_tokens"]])
        total_tokens = sum(r["out_tokens"] or 0 for r in res)
        print(f"{k:>5} {med(ttfts):>9.0f}/{max(ttfts):>7.0f} {med([r['itl_ms'] for r in res]):>7.1f}"
              f" {per_tok:>14.2f} {max(r['total_ms'] for r in res):>14.0f} {total_tokens / wall:>10.1f}")


def exp_cache(words=2048, reps=3):
    print(f"\n[3] prefix 캐시 (같은 {words}단어 프롬프트 반복)")
    print(f"{'run':>4} {'tokens':>7} {'cached':>7} {'prefill_ms':>11} {'ttft_ms':>8}")
    p = filler(words, uuid.uuid4().hex)
    for i in range(reps + 1):
        r = generate(p, 1)
        r.update(exp="cache", run=i)
        log(r)
        print(f"{i:>4} {r['prompt_tokens']:>7} {r['cached_tokens'] or 0:>7} {r['prefill_ms']:>11.1f} {r['ttft_ms']:>8.1f}")


def exp_ctx(lengths=(32, 512, 1536), levels=(1, 4), out_tokens=64):
    """컨텍스트 길이가 토큰 간격에 주는 영향 (단독·배치)."""
    print(f"\n[4] 컨텍스트 길이 대 토큰 간격 (출력 {out_tokens} 토큰)")
    print(f"{'words':>6} {'tokens':>7} {'conc':>5} {'itl_ms':>7} {'decode_ms/tok':>14}")
    for n in lengths:
        for k in levels:
            res = [None] * k

            def run(i):
                res[i] = generate(filler(n, uuid.uuid4().hex) +
                                  " Then write a long essay about ships.", out_tokens)

            ths = [threading.Thread(target=run, args=(i,)) for i in range(k)]
            for t in ths:
                t.start()
            for t in ths:
                t.join()
            for r in res:
                r.update(exp="ctx", words=n, conc=k)
                log(r)
            per_tok = med([r["decode_ms"] / r["out_tokens"] for r in res if r["out_tokens"]])
            print(f"{n:>6} {med([r['prompt_tokens'] for r in res]):>7.0f} {k:>5}"
                  f" {med([r['itl_ms'] for r in res]):>7.1f} {per_tok:>14.2f}")


if __name__ == "__main__":
    generate("warm up", 1)  # 모델 적재
    which = sys.argv[2:] or ["prefill", "concurrency", "cache", "ctx"]
    if "prefill" in which:
        exp_prefill()
    if "concurrency" in which:
        exp_concurrency()
    if "cache" in which:
        exp_cache()
    if "ctx" in which:
        exp_ctx()
