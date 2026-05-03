#!/usr/bin/env python3
"""Analisa CSV de avaliação do Aerochord.

Uso:
    python3 tools/analyze_eval.py aerochord_eval_*.csv
"""

import sys
import csv
import statistics
import collections


def main(path):
    rows = []
    with open(path) as f:
        for r in csv.DictReader(f):
            rows.append({**r, 'latency_ms': float(r['latency_ms'])})

    if not rows:
        print("Arquivo vazio.")
        return

    latencies = sorted(r['latency_ms'] for r in rows)
    n = len(latencies)

    p50 = latencies[n // 2]
    p95 = latencies[int(n * 0.95)]
    mean = statistics.mean(latencies)
    below30 = sum(1 for l in latencies if l < 30) / n * 100

    print(f"Arquivo  : {path}")
    print(f"Amostras : {n}")
    print(f"Média    : {mean:.2f} ms")
    print(f"P50      : {p50:.2f} ms")
    print(f"P95      : {p95:.2f} ms")
    print(f"< 30 ms  : {below30:.1f}%")
    print(f"Meta TCC : {'OK (P95 < 30 ms)' if p95 < 30 else 'FALHOU (P95 >= 30 ms)'}")

    by_type = collections.Counter(r['midi_type'] for r in rows)
    print("\nDistribuição por tipo MIDI:")
    for t, c in sorted(by_type.items()):
        pct = c / n * 100
        print(f"  {t:15s}: {c:5d}  ({pct:.1f}%)")


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(f"Uso: {sys.argv[0]} <aerochord_eval_*.csv>")
        sys.exit(1)
    for path in sys.argv[1:]:
        main(path)
        if len(sys.argv) > 2:
            print()
