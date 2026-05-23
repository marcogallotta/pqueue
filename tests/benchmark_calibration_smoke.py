#!/usr/bin/env python3
"""
Smoke test: verify that --calibration-file actually changes sim output.
Runs the benchmark twice (default vs 2x calibration) and asserts that
at least one sim field in the first result row differs by roughly 2x.
"""
import json
import subprocess
import sys

BINARY = "./build/pqueue-benchmark"

def run(extra_args):
    cmd = [BINARY, "--json", "--strict", "--repeat", "1"] + extra_args
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"benchmark failed: {r.stderr}", file=sys.stderr)
        sys.exit(1)
    return json.loads(r.stdout)

def sim_fields(result):
    return result["sim_p99_ms"], result["sim_max_ms"]

default_out  = run([])
calibrated   = run(["--calibration-file", "tests/data/calibration-test-2x.json"])

default_row    = default_out["results"][0]
calibrated_row = calibrated["results"][0]

d_p99, d_max = sim_fields(default_row)
c_p99, c_max = sim_fields(calibrated_row)

if d_p99 == 0 and d_max == 0:
    print("ERROR: default sim values are both zero — benchmark may be broken", file=sys.stderr)
    sys.exit(1)

ratio_p99 = c_p99 / d_p99 if d_p99 > 0 else float("inf")
ratio_max = c_max / d_max if d_max > 0 else float("inf")

print(f"default:    sim_p99_ms={d_p99:.3f}  sim_max_ms={d_max:.3f}")
print(f"2x calibr:  sim_p99_ms={c_p99:.3f}  sim_max_ms={c_max:.3f}")
print(f"ratio:      p99={ratio_p99:.2f}x  max={ratio_max:.2f}x")

lo, hi = 1.5, 2.5
ok = (lo <= ratio_p99 <= hi) or (lo <= ratio_max <= hi)
if not ok:
    print(f"FAIL: expected ~2x ratio in [{lo}, {hi}], got p99={ratio_p99:.2f} max={ratio_max:.2f}")
    print("      --calibration-file is not changing sim output as expected")
    sys.exit(1)

print("PASS: calibration file changes sim output as expected")
