#!/usr/bin/env python3
"""JSONL에서 Markdown 지표 표와 비교 그래프를 다시 만든다."""

import argparse
import collections
from html import escape
import json
from pathlib import Path
import statistics

ALGORITHMS = ("rr", "least_requests", "p2c")
LABELS = {"rr": "RR", "least_requests": "Least Requests", "p2c": "P2C",
          "homogeneous": "동일", "heterogeneous": "이기종", "fault": "일부 장애",
          "blackout": "전체 장애", "pool_low": "동시 4", "pool_high": "동시 48"}


def load_results(path):
    groups = collections.defaultdict(list)
    with path.open() as f:
        environment = json.loads(next(f))["environment"]
        for line in f:
            row = json.loads(line)
            groups[(row["condition"], row["policy"], row["pool"])].append(row["summary"])
    return environment, groups


def field(rows, key):
    items = [row[key] for row in rows if row[key] is not None]
    if not items:
        return "—"
    stdev = statistics.stdev(items) if len(items) > 1 else 0
    return f"{statistics.mean(items):,.2f} ± {stdev:,.2f}"


def per_backend(rows, key):
    return " / ".join(field([{"v": row[key][i]} for row in rows], "v")
                      for i in range(len(rows[0][key])))


def markdown(groups):
    lines = ["각 셀은 5회 평균 ± 표본 표준편차입니다. 지연과 복구는 ms, RSS는 KiB입니다.",
             "", "## 로드 밸런싱과 장애 복구", "",
             "| 조건 | 정책 | RPS | 평균 지연 | P50 | P95 | P99 | 실패율 (%) | 503 (건) | "
             "복구 (ms) |", "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for env in ("homogeneous", "heterogeneous", "fault", "blackout"):
        for policy in ALGORITHMS:
            rows = groups[env, policy, 8]
            values = [field(rows, key) for key in ("rps", "mean_ms", "p50", "p95", "p99",
                                                   "fail_pct", "503")]
            values.append(field(rows, "recovery_ms") if env in ("fault", "blackout") else "—")
            lines.append("| " + " | ".join([LABELS[env], LABELS[policy], *values]) + " |")

    lines.extend(["", "## 유휴 풀 크기", "",
                  "| 조건 | 풀 상한 | RPS | 평균 지연 | P50 | P95 | P99 | 실패율 (%) | 신규 TCP | "
                  "재사용 (건) | 재사용률 (%) | FD 표본 최대 | RSS 표본 최대 |",
                  "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"])
    for env in ("pool_low", "pool_high"):
        for pool in (1, 8, 50):
            rows = groups[env, "rr", pool]
            values = [field(rows, key) for key in ("rps", "mean_ms", "p50", "p95", "p99",
                                                   "fail_pct", "new_tcp", "reused", "reuse_pct",
                                                   "fd", "rss_kib")]
            lines.append("| " + " | ".join([LABELS[env], str(pool), *values]) + " |")

    lines.extend(["", "## 백엔드 배정과 자원", "",
                  "배정 비중은 0–1 사이 비율이며 A / B / C 순서입니다.",
                  "",
                  "| 조건 | 정책 | 배정 비중 | 최대 활성 요청 | 신규 TCP | 재사용 (건) | "
                  "재사용률 (%) | FD 표본 최대 | RSS 표본 최대 |",
                  "|---|---|---|---|---:|---:|---:|---:|---:|"])
    for env in ("homogeneous", "heterogeneous", "fault", "blackout"):
        for policy in ALGORITHMS:
            rows = groups[env, policy, 8]
            values = [per_backend(rows, key) for key in ("assignment_share", "peak_active")]
            values += [field(rows, key) for key in ("new_tcp", "reused", "reuse_pct", "fd",
                                                    "rss_kib")]
            lines.append("| " + " | ".join([LABELS[env], LABELS[policy], *values]) + " |")
    return "\n".join(lines) + "\n"


def chart(groups):
    palette = ("#37689d", "#008575", "#a85b1f")
    markup = ['<svg xmlns="http://www.w3.org/2000/svg" width="900" height="585" '
              'viewBox="0 0 900 585" role="img" aria-labelledby="title">',
              '<title id="title">Proxy benchmark means and standard deviations</title>',
              '<rect width="900" height="585" fill="#ffffff"/>',
              '<g font-family="system-ui, sans-serif" fill="#183047">']

    def label(x, y, text, size=14, weight=400):
        markup.append(f'<text x="{x}" y="{y}" font-size="{size}" font-weight="{weight}">'
                      f'{escape(text)}</text>')

    def panel(x, y, heading, keys, names, metric, unit):
        label(x, y, heading, 17, 700)
        scores = []
        for key in keys:
            series = [r[metric] for r in groups[key]]
            scores.append((statistics.mean(series), statistics.stdev(series)))
        scale = 190 / (max(mean + sd for mean, sd in scores) * 1.06)
        for i, ((mean, sd), name) in enumerate(zip(scores, names)):
            top = y + 30 + i * 42
            origin = x + 132
            label(x, top + 15, name, 13)
            markup.append(f'<rect x="{origin}" y="{top}" width="{mean*scale:.1f}" height="17" '
                          f'rx="2" fill="{palette[i]}"/>')
            center = top + 8.5
            lo, hi = origin + max(0, mean - sd) * scale, origin + (mean + sd) * scale
            markup.append(f'<path d="M{lo:.1f} {center}H{hi:.1f} M{lo:.1f} {center-4}v8 '
                          f'M{hi:.1f} {center-4}v8" stroke="#183047" fill="none"/>')
            label(origin, top + 34, f"{mean:,.2f} {unit}", 12)

    lb_names = tuple(LABELS[name] for name in ALGORITHMS)
    pool_names = ("1", "8", "50")
    label(35, 38, "C HTTP proxy · 90 trials", 22, 700)
    label(35, 65, "Bar length is the mean. Whiskers show one sample standard deviation.", 13)
    panel(35, 114, "Heterogeneous backends", [("heterogeneous", p, 8) for p in ALGORITHMS],
          lb_names, "rps", "req/s")
    panel(428, 114, "Partial outage · P95 latency", [("fault", p, 8) for p in ALGORITHMS],
          lb_names, "p95", "ms")
    panel(35, 321, "High concurrency · New upstream TCP", [("pool_high", "rr", n) for n in (1, 8, 50)],
          pool_names, "new_tcp", "connections")
    panel(428, 321, "High concurrency · P95 latency", [("pool_high", "rr", n) for n in (1, 8, 50)],
          pool_names, "p95", "ms")
    label(35, 537, "macOS arm64  ·  1 KiB response  ·  72 proxy workers  ·  5 trials per condition", 13)
    markup.append("</g></svg>")
    return "\n".join(markup) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    _, groups = load_results(args.input)
    if len(groups) != 18 or any(len(group) != 5 for group in groups.values()):
        parser.error("18 conditions × 5 repetitions are required")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "proxy-tables.md").write_text(markdown(groups), encoding="utf-8")
    (args.output_dir / "proxy-comparison.svg").write_text(chart(groups), encoding="utf-8")


if __name__ == "__main__":
    main()
