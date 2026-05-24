#!/usr/bin/env python3
"""
Compare deterministic benchmark fields against a baseline.
Wall-clock times are excluded (machine-dependent).
Numeric fields: fail on increase, silent on decrease (improvement).
Boolean fields: fail if baseline=true and current=false.
Usage: benchmark_regression.py <baseline.json> <current.json>
Exits 0 on pass, 1 on any regression.
"""
import json
import sys

NUMERIC = [
    "writeFile", "writeAt", "readAt", "remove", "listFiles",
    "write_amp", "read_bpp",
    "idle_steps", "idle_noops", "hot_compactions", "cap_exhausted",
]

BOOLEAN = ["ok"]

def row_key(r):
    return (r["scenario"], r["payloadBytes"], r["records"])

def main():
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} baseline.json current.json", file=sys.stderr)
        sys.exit(1)

    with open(sys.argv[1]) as f:
        baseline = {row_key(r): r for r in json.load(f)["results"]}
    with open(sys.argv[2]) as f:
        current = {row_key(r): r for r in json.load(f)["results"]}

    failures = []

    for key in baseline:
        if key not in current:
            failures.append(f"MISSING row {key}")
            continue
        b, c = baseline[key], current[key]
        label = f"[{key[0]} payload={key[1]}B records={key[2]}]"
        for field in NUMERIC:
            if field not in b:
                continue
            bv, cv = b[field], c.get(field)
            if cv is None:
                failures.append(f"MISSING field {field} {label}")
            elif cv > bv:
                failures.append(f"REGRESSION {label} {field}: {bv} -> {cv}")
        for field in BOOLEAN:
            if field not in b:
                continue
            bv, cv = b[field], c.get(field)
            if bv is True and cv is not True:
                failures.append(f"REGRESSION {label} {field}: {bv} -> {cv}")

    for key in current:
        if key not in baseline:
            print(f"NEW row {key} (not in baseline — update baseline to track)")

    if failures:
        for msg in failures:
            print(msg, file=sys.stderr)
        sys.exit(1)

    print(f"ok — {len(current)} rows matched baseline")

if __name__ == "__main__":
    main()
