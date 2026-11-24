#!/usr/bin/env python3
"""벤치 실행기: 가짜 LLM 백엔드 N대 + httpd + loadgen을 띄우고 결과를 모은다.

    run_bench.py e0                      # 프록시 지연, 끊김 전파 (직접 / loop / pool)
    run_bench.py e1                      # 오래 열린 스트림 동시성 (loop / pool)
    run_bench.py scenario --name B --rate 3 --duration 60 --engine loop [--repeat 3]

결과는 bench/results/<시각>-<실험>/ 에 loadgen JSONL, 백엔드 /__stats·/__events, 서버 로그,
summary.txt로 남는다. 저장소 루트에서 실행하고 build/httpd가 있어야 한다.
"""
import argparse
import json
import os
import socket
import statistics
import subprocess
import sys
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BENCH = os.path.join(ROOT, "bench")
HTTPD = os.path.join(ROOT, "build", "httpd")
PY = sys.executable
BACKEND_BASE = 19101
GATEWAY_PORT = 18080


def free(port):
    with socket.socket() as s:
        return s.connect_ex(("127.0.0.1", port)) != 0


def wait_port(port, timeout=10):
    end = time.time() + timeout
    while time.time() < end:
        if not free(port):
            return
        time.sleep(0.05)
    raise RuntimeError(f"port {port} did not open")


class Stack:
    """백엔드 n대와 (선택) httpd를 띄우고 정리한다."""

    def __init__(self, outdir, n_backends, backend_args=(), engine=None, workers=None):
        self.outdir = outdir
        self.procs = []
        self.ports = [BACKEND_BASE + i for i in range(n_backends)]
        for i, port in enumerate(self.ports):
            if not free(port):
                raise RuntimeError(f"port {port} busy")
            p = subprocess.Popen([PY, "-I", os.path.join(BENCH, "fake_llm.py"), "--port", str(port),
                                  "--id", f"b{i + 1}", *backend_args],
                                 stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
            if not p.stdout.readline().startswith("READY"):
                raise RuntimeError("backend failed")
            self.procs.append(p)
        self.gateway = None
        if engine:
            conf = os.path.join(outdir, "httpd.conf")
            with open(conf, "w") as f:
                f.write(f"listen 127.0.0.1:{GATEWAY_PORT}\n")
                f.write(f"root {os.path.join(ROOT, 'tests', 'fixtures', 'www')}\n")
                f.write(f"engine {engine}\n")
                if workers:
                    f.write(f"workers {workers}\n")
                f.write("upstream llm " + " ".join(f"127.0.0.1:{p}" for p in self.ports) + "\n")
                f.write("route / proxy llm\n")
                # 긴 prefill(수십 초) 동안 본문이 멈춰 있어도 끊지 않도록 (FR-LLM-02 전까지)
                f.write("upstream_response_timeout_ms 600000\n")
                f.write("body_timeout_ms 600000\n")
                f.write("queue_len 4096\n")
                f.write(f"access_log {os.path.join(outdir, 'access.log')}\n")
                f.write(f"error_log {os.path.join(outdir, 'error.log')}\n")
            if not free(GATEWAY_PORT):
                raise RuntimeError(f"port {GATEWAY_PORT} busy")
            self.gateway = subprocess.Popen([HTTPD, "-c", conf], stdout=subprocess.DEVNULL,
                                            stderr=subprocess.DEVNULL)
            wait_port(GATEWAY_PORT)

    @property
    def target(self):
        port = GATEWAY_PORT if self.gateway else self.ports[0]
        return f"127.0.0.1:{port}"

    def backend_json(self, path):
        out = {}
        for i, port in enumerate(self.ports):
            with urllib.request.urlopen(f"http://127.0.0.1:{port}{path}", timeout=5) as r:
                out[f"b{i + 1}"] = json.loads(r.read())
        return out

    def close(self):
        for p in ([self.gateway] if self.gateway else []) + self.procs:
            p.terminate()
        for p in ([self.gateway] if self.gateway else []) + self.procs:
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                p.kill()


def loadgen(target, out, *args):
    """loadgen을 별도 프로세스로 돌리고 요약(stderr)을 돌려준다."""
    r = subprocess.run([PY, "-I", os.path.join(BENCH, "loadgen.py"), "--target", target,
                        "--out", out, *map(str, args)], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(r.stderr)
    return r.stderr


def rows(path):
    with open(path) as f:
        return [json.loads(l) for l in f if l.strip()]


def med(xs):
    xs = [x for x in xs if x is not None]
    return statistics.median(xs) if xs else float("nan")


def p99(xs):
    xs = sorted(x for x in xs if x is not None)
    return xs[min(len(xs) - 1, int(len(xs) * 0.99))] if xs else float("nan")


def new_outdir(name):
    d = os.path.join(BENCH, "results", time.strftime("%Y%m%d-%H%M%S") + "-" + name)
    os.makedirs(d, exist_ok=True)
    return d


# ── E0: 프록시가 더하는 지연, 끊김 전파 ─────────────────────────────────────

def e0(a):
    out = new_outdir("e0")
    lines = ["E0: 백엔드 1대, 동시 1개 (경합 없음). 지연 = 클라이언트 측정 - 백엔드 측정",
             "끊김 → 백엔드 감지 (ms, 중앙/최대, 끊은 수→감지 수): stream = 토큰 흐르는 중, "
             "prefill = 첫 토큰 전",
             f"{'path':>8} {'n':>4} {'ttft_med':>9} {'overhead_med':>13} {'overhead_p99':>13}"
             f" {'itl_med':>8} {'itl_max_p99':>12} {'stream med/max':>27} {'prefill med/max':>27}"]
    for mode in ("direct", "loop", "pool"):
        st = Stack(out, 1, engine=None if mode == "direct" else mode)
        try:
            f = os.path.join(out, f"{mode}.jsonl")
            loadgen(st.target, f, "--scenario", "tiny", "--concurrency", 1, "--duration", a.duration)
            rs = [r for r in rows(f) if r["status"] == 200]
            over = [r["ttft_ms"] - r["backend_timing"]["ttft_backend_ms"]
                    for r in rs if r.get("backend_timing") and r["ttft_ms"] is not None]
            # 끊김 전파: 끊은 시각과 백엔드가 알아챈 시각. 요청 사이 간격을 둬(열린 루프, 겹침 없음)
            # 순서로 짝짓는다. stream = 토큰이 흐르는 중, prefill = 첫 토큰 전 (prefill 약 2.7초)
            lags = {}
            for kind, scen, rate, after in (("stream", "long", 1, 400), ("prefill", "prefill", 0.25, 300)):
                fa = os.path.join(out, f"{mode}-abort-{kind}.jsonl")
                seen = len(st.backend_json("/__events")["b1"])
                loadgen(st.target, fa, "--scenario", scen, "--rate", rate, "--duration", 8,
                        "--abort-after-ms", after)
                time.sleep(3.5)  # prefill 중 끊긴 요청은 첫 토큰 뒤에야 감지된다
                ab = sorted((r for r in rows(fa) if r.get("aborted")), key=lambda r: r["t_abort"])
                ev = [e for e in st.backend_json("/__events")["b1"][seen:] if e["event"] == "client_gone"]
                lags[kind] = ([e["t_ms"] - r["t_abort"] for r, e in zip(ab, ev)], len(ab), len(ev))
            json.dump(st.backend_json("/__stats"), open(os.path.join(out, f"{mode}-stats.json"), "w"))
        finally:
            st.close()
        lines.append(f"{mode:>8} {len(rs):>4} {med([r['ttft_ms'] for r in rs]):>9.1f}"
                     f" {med(over):>13.2f} {p99(over):>13.2f}"
                     f" {med([r['itl_med_ms'] for r in rs]):>8.1f} {p99([r['itl_max_ms'] for r in rs]):>12.1f}"
                     + "".join(f" {med(l):>12.1f}/{max(l) if l else float('nan'):>7.1f} ({na}→{ne})"
                               for l, na, ne in (lags["stream"], lags["prefill"])))
    report(out, lines)


# ── E1: 오래 열린 스트림 동시성 ─────────────────────────────────────────────

def e1(a):
    out = new_outdir("e1")
    # 백엔드가 병목이 되지 않게: 슬롯을 크게, 배치 비용 0, prefill 짧게
    backend = ["--slots", "1024", "--step-per-seq-ms", "0", "--step-base-ms", "20",
               "--prefill-ms-per-token", "0.1", "--kv-cap", "100000000"]
    lines = [f"E1: 백엔드 4대(병목 아님), 스트림 1500토큰(약 30초), pool workers={a.workers}",
             f"{'engine':>6} {'conc':>5} {'n':>4} {'ok':>4} {'ttft_med':>9} {'ttft_p99':>9}"
             f" {'total_med':>10} {'itl_max_p99':>12}"]
    for engine in ("loop", "pool"):
        for conc in a.levels:
            st = Stack(out, 4, backend, engine=engine, workers=a.workers)
            try:
                f = os.path.join(out, f"{engine}-c{conc}.jsonl")
                loadgen(st.target, f, "--scenario", "long", "--concurrency", conc,
                        "--duration", a.duration)
            finally:
                st.close()
            rs = rows(f)
            ok = [r for r in rs if r["status"] == 200 and not r["error"]]
            lines.append(f"{engine:>6} {conc:>5} {len(rs):>4} {len(ok):>4}"
                         f" {med([r['ttft_ms'] for r in ok]):>9.0f} {p99([r['ttft_ms'] for r in ok]):>9.0f}"
                         f" {med([r['total_ms'] for r in ok]):>10.0f}"
                         f" {p99([r['itl_max_ms'] for r in ok]):>12.0f}")
    report(out, lines)


# ── 시나리오 (E2) ──────────────────────────────────────────────────────────

def scenario(a):
    out = new_outdir(f"{a.name}-{a.engine}-r{a.rate}")
    lines = [f"scenario {a.name}, engine {a.engine}, rate {a.rate}/s, duration {a.duration}s, "
             f"backends 4, seed 1..{a.repeat}"]
    for i in range(a.repeat):
        st = Stack(out, 4, engine=a.engine)
        try:
            f = os.path.join(out, f"run{i + 1}.jsonl")
            summ = loadgen(st.target, f, "--scenario", a.name, "--rate", a.rate,
                           "--duration", a.duration, "--seed", i + 1)
            json.dump(st.backend_json("/__stats"), open(os.path.join(out, f"run{i + 1}-stats.json"), "w"))
        finally:
            st.close()
        lines.append(f"--- run {i + 1}")
        lines.append(summ.rstrip())
    report(out, lines)


def report(out, lines):
    text = "\n".join(lines) + "\n"
    with open(os.path.join(out, "summary.txt"), "w") as f:
        f.write(text)
    print(text + f"\n결과: {out}")


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    s0 = sub.add_parser("e0")
    s0.add_argument("--duration", type=float, default=10)
    s1 = sub.add_parser("e1")
    s1.add_argument("--levels", type=int, nargs="+", default=[4, 8, 16, 32, 64])
    s1.add_argument("--workers", type=int, default=8)
    s1.add_argument("--duration", type=float, default=20)
    s2 = sub.add_parser("scenario")
    s2.add_argument("--name", default="A")
    s2.add_argument("--rate", type=float, required=True)
    s2.add_argument("--duration", type=float, default=60)
    s2.add_argument("--engine", default="loop", choices=["loop", "pool"])
    s2.add_argument("--repeat", type=int, default=1)
    a = p.parse_args()
    if not os.path.exists(HTTPD):
        sys.exit("build/httpd가 없다: make 먼저")
    {"e0": e0, "e1": e1, "scenario": scenario}[a.cmd](a)


if __name__ == "__main__":
    main()
