#!/usr/bin/env python3
"""run_bench scenario 결과 묶기: 실행(run*.jsonl) 여러 개를 클래스별로 요약한다.

    analyze.py bench/results/<dir> [<dir> ...]

실행마다 값을 내고, 실행 사이의 중앙값과 [최소~최대]를 보인다. 워밍업 요청은 뺀다.
짧은 요청의 TTFT는 백엔드가 알려 준 시간으로 나눈다:
  queue = 백엔드 내부 대기열에서 기다린 시간 (슬롯·KV가 없어서)
  block = 백엔드 TTFT − queue − 자기 prefill (다른 요청의 prefill이 단계를 막은 시간 포함)
  gw    = 클라이언트 TTFT − 백엔드 TTFT (게이트웨이 + 네트워크)
세션이 있는 요청(시나리오 C)은 다음도 본다:
  cache  = 백엔드 통계의 캐시 적중 토큰 / (적중 + prefill 토큰)
  spill  = 키의 1순위 백엔드가 아닌 곳으로 간 비율 (게이트웨이 lb.c와 같은 해시로 재계산)
  skew   = 백엔드별 prefill 토큰의 최대 / 평균
"""
import glob
import json
import os
import statistics
import sys


def pct(xs, p):
    xs = sorted(x for x in xs if x is not None)
    if not xs:
        return None
    k = (len(xs) - 1) * p
    lo, hi = int(k), min(int(k) + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


M64 = (1 << 64) - 1


def mix64(h):
    h ^= h >> 30
    h = (h * 0xbf58476d1ce4e5b9) & M64
    h ^= h >> 27
    h = (h * 0x94d049bb133111eb) & M64
    return h ^ (h >> 31)


def fnv_hash(b):
    """src/upstream/internal_upstream.h의 upstream_hash와 같다."""
    h = 1469598103934665603
    for c in b:
        h = ((h ^ c) * 1099511628211) & M64
    return mix64(h)


def first_choice(key, backends):
    kh = fnv_hash(key.encode())
    return max(backends, key=lambda bid: mix64(kh ^ fnv_hash(backends[bid].encode())))


# run_bench가 띄우는 백엔드: b1 = 127.0.0.1:19101, b2 = :19102, ...
BACKENDS = {f"b{i + 1}": f"127.0.0.1:{19101 + i}" for i in range(4)}


def max_inflight(rs):
    """백엔드별 동시에 걸린 요청 수의 최댓값 (클라이언트 시각 기준)."""
    out = {}
    for b in sorted({r["backend"] for r in rs if r.get("backend")}):
        ev = []
        for r in rs:
            if r.get("backend") == b and r.get("total_ms") is not None:
                ev += [(r["t_send"], 1), (r["t_send"] + r["total_ms"], -1)]
        cur = peak = 0
        for _, d in sorted(ev):
            cur += d
            peak = max(peak, cur)
        out[b] = peak
    return out


def run_stats(path):
    rs = [json.loads(l) for l in open(path) if l.strip()]
    rs = [r for r in rs if r.get("cls") != "dropped" and not r.get("warmup")]
    ok = [r for r in rs if r["status"] == 200 and not r["error"]]
    st = {"n": len(rs), "err": len(rs) - len(ok)}
    for cls in sorted({r["cls"] for r in ok}):
        c = [r for r in ok if r["cls"] == cls]
        bt = [r["backend_timing"] for r in c if r.get("backend_timing")]
        st[cls] = {
            "n": len(c),
            "ttft50": pct([r["ttft_ms"] for r in c], .5),
            "ttft99": pct([r["ttft_ms"] for r in c], .99),
            "total99": pct([r["total_ms"] for r in c], .99),
            "itlmax99": pct([r["itl_max_ms"] for r in c], .99),
            "queue99": pct([t["queue_ms"] for t in bt], .99),
            "block99": pct([t["ttft_backend_ms"] - t["queue_ms"] - t["prefill_ms"] for t in bt], .99),
            "gw99": pct([r["ttft_ms"] - r["backend_timing"]["ttft_backend_ms"]
                         for r in c if r.get("backend_timing") and r["ttft_ms"] is not None], .99),
        }
    per = {}
    for r in ok:
        per[r["backend"]] = per.get(r["backend"], 0) + 1
    st["per_backend"] = per
    st["peak_inflight"] = max_inflight(ok)
    keyed = [r for r in ok if r.get("session") and r.get("backend")]
    if keyed:
        st["spill"] = 100.0 * sum(1 for r in keyed
                                  if first_choice(r["session"], BACKENDS) != r["backend"]) / len(keyed)
    stats_path = path.replace(".jsonl", "-stats.json")
    if os.path.exists(stats_path):
        bs = json.load(open(stats_path))
        hit = sum(b.get("cache_hit_tokens", 0) for b in bs.values())
        pre = sum(b.get("prefill_tokens", 0) for b in bs.values())
        st["cache"] = 100.0 * hit / (hit + pre) if hit + pre else None
        loads = [b.get("prefill_tokens", 0) for b in bs.values()]
        st["skew"] = max(loads) / (sum(loads) / len(loads)) if sum(loads) else None
        st["prefill_by_backend"] = {k: b.get("prefill_tokens", 0) for k, b in sorted(bs.items())}
    return st


def agg(vals):
    vals = [v for v in vals if v is not None]
    if not vals:
        return "-"
    m = statistics.median(vals)
    return f"{m:.0f} [{min(vals):.0f}~{max(vals):.0f}]"


def main():
    for d in sys.argv[1:]:
        runs = sorted(glob.glob(os.path.join(d, "run*.jsonl")))
        sts = [run_stats(p) for p in runs]
        print(f"\n## {os.path.basename(d.rstrip('/'))}  ({len(runs)} runs)")
        print(f"요청 수 {agg([s['n'] for s in sts])}, 실패 {agg([s['err'] for s in sts])}")
        classes = sorted({k for s in sts for k in s if isinstance(s[k], dict) and "ttft50" in s[k]})
        print(f"| class | n | TTFT p50 | TTFT p99 | total p99 | 토큰 간격 최대 p99 |"
              f" 대기열 p99 | 막힘 p99 | 게이트웨이 p99 |")
        print("| --- " * 9 + "|")
        for c in classes:
            g = [s[c] for s in sts if c in s]
            print(f"| {c} | {agg([x['n'] for x in g])} | {agg([x['ttft50'] for x in g])} |"
                  f" {agg([x['ttft99'] for x in g])} | {agg([x['total99'] for x in g])} |"
                  f" {agg([x['itlmax99'] for x in g])} | {agg([x['queue99'] for x in g])} |"
                  f" {agg([x['block99'] for x in g])} | {agg([x['gw99'] for x in g])} |")
        if any("cache" in s for s in sts):
            print(f"캐시 적중률(토큰) {agg([s.get('cache') for s in sts])}%,"
                  f" 넘침 {agg([s.get('spill') for s in sts])}%,"
                  f" prefill 쏠림(최대/평균) {agg([100 * s['skew'] if s.get('skew') else None for s in sts])}%")
        for i, s in enumerate(sts):
            print(f"run{i + 1}: 백엔드별 요청 {s['per_backend']}  최대 동시 {s['peak_inflight']}"
                  + (f"  prefill {s['prefill_by_backend']}" if "prefill_by_backend" in s else ""))


if __name__ == "__main__":
    main()
