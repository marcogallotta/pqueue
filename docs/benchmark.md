# Benchmark

Two complementary benchmarks cover performance: a POSIX structural benchmark for CI
regression, and on-device tests for real LittleFS latency.

---

## POSIX benchmark — structural regression (CI)

Runs on an in-memory POSIX filesystem. Deterministic and machine-independent for the
fields that matter: I/O op counts, write amplification, and idle compaction invariants.
Wall-clock times are host-local and informational only.

```bash
make -j12 benchmark          # build only
make benchmark-markdown      # print markdown table to stdout
make update-benchmark-baseline  # regenerate data/benchmark-results-posix.json
```

CI builds the benchmark, runs it with `--json --strict`, and diffs the output against
`data/benchmark-results-posix.json` using `tools/benchmark_regression.py`. Only
deterministic fields are compared; wall-clock times are excluded.

To update the baseline after an intentional change:

```bash
make update-benchmark-baseline
```

---

## On-device benchmark — real LittleFS latency

The POSIX benchmark does **not** predict device latency. LittleFS op costs vary with
filesystem load, file size, and flash wear state in ways a fixed-cost model cannot
capture. For real timing numbers, run the on-device tests against actual hardware.

### `env:esp32s3-benchmark` — structured latency benchmark

The primary on-device benchmark. Measures application-visible `Queue` latency across
six scenarios on real LittleFS. Uses the `std::string` Queue API — numbers reflect
the full string-path overhead that production callers pay.

Two environments share the same test file; a build flag selects the parameter set:

| Environment | Mode | Approx. runtime | When to use |
|---|---|---|---|
| `esp32s3-benchmark` | full | ~10 min | Release measurement, baseline capture |
| `esp32s3-benchmark-fast` | fast | ~1–2 min | Quick sanity check after a code change |

Fast mode: `N=10`, `burst=20`, `cycles=1`, mount up to 200 records. Enough to confirm
operations succeed and latency is in the right order of magnitude — not enough for a
reliable distribution.

```bash
~/venvs/esp/bin/pio test -e esp32s3-benchmark --without-testing
~/venvs/esp/bin/pio device monitor

# or fast mode:
~/venvs/esp/bin/pio test -e esp32s3-benchmark-fast --without-testing
~/venvs/esp/bin/pio device monitor
```

The test runner (`pio test` without `--without-testing`) swallows non-Unity serial
lines. Always use `--without-testing` + `pio device monitor` to see the benchmark
output. Press the reset button after the monitor connects if the device has already
booted. The first `bench config` line reports `mode=full` or `mode=fast`.

**Config:** `reserved_bytes=2108736`, `max_segments=200`, matching the
`esp32s3-idle-sanity` and `esp32s3-compaction` configs for direct comparability.

**Scenarios and output columns:**

| Scenario | Output line | Key fields |
|---|---|---|
| `enqueue` 256 B, 1 KB | `scenario=enqueue payload_b=N n=100` | `p50_us` `p90_us` `p99_us` `max_us` |
| `peek_pop` 256 B, 1 KB | `scenario=peek_pop payload_b=N n=100` | `p50_us` `p90_us` `p99_us` `max_us` |
| `mount` | `scenario=mount payload_b=256 preload=N` | `mount_us` — time for `statsResult()` after construction; captures lazy-mount I/O |
| `compact_idle` per step | `scenario=compact_idle payload_b=256 cycle=N step=N` | `dt_us` — one productive `compactIdle(1)` step |
| `compact_idle` summary | `scenario=compact_idle payload_b=256 summary` | `p50_us` `p90_us` `p99_us` `max_us` `total_us` `productive` `noops` |

Mount sub-cases: preload = 0, 50, 200, 1000 records. The 1000-record preload takes
~30 s; progress is printed every 100 records.

Compaction workload: burst=100 / pop=90% / rec=256 B / cycles=2. Lighter than the
`idle-sanity` heavy case; use it to characterise step latency without the wear of the
500-burst workload.

All output lines are prefixed `bench ` with `key=value` fields — grep-friendly and
paste-ready into a spreadsheet.

### `env:esp32s3-idle-sanity` — heavy compaction evidence run

Measures worst-case `compactIdle(1)` step latency under the heavy workload
(burst=500/pop=90%/rec=492B/cycles=3). Kept as the authoritative evidence run;
`esp32s3-benchmark` is the structured release benchmark.

```bash
~/venvs/esp/bin/pio test -e esp32s3-idle-sanity --without-testing
~/venvs/esp/bin/pio device monitor
```

---

## Workload matrix

| Scenario | N | Payload sizes |
|---|---|---|
| enqueue | 1000 | 64 B, 256 B, 1 KB, 2 KB |
| peek+pop | 1000 | 64 B, 256 B, 1 KB, 2 KB |
| outbox offline submit | 1000 | 256 B, 1 KB |
| mount | — | 0, 100, 1000 pre-loaded records |
| idle compaction step | burst=500 / cycles=3 / pop=90% | 256 B, 492 B |

Each cell performs N operations exactly once per run. `--repeat K` runs the full cell K
times; reported wall-clock percentiles are computed across all K × N samples.

**Payload size and store config.** Each scenario configures the store with `recordSizeBytes`
set large enough to hold the largest payload under test.

**Outbox "offline" definition.** The outbox offline submit scenario uses a fake transport
that returns `RetryLater` (never succeeds). This measures the failed-send + durable-enqueue
path, not a live send.

**Mount capacity.** The mount scenarios use a fixed payload of 256 B. The setup store is
configured with capacity sufficient for 1000 records at that size.

---

## Output columns

### Enqueue, peek+pop, outbox submit, mount

| Column | Description |
|---|---|
| `p50_us` `p90_us` `p99_us` `max_us` | Host wall-clock latency percentiles — informational, machine-dependent |
| `write_amp` | Bytes written per payload byte (`bytesWritten / (N × payloadBytes)`) |
| `read_bpp` | Bytes read per record (`bytesRead / N`) |
| `writeFile` `writeAt` `readAt` `remove` | I/O op counts |

### Idle compaction

| Column | Description |
|---|---|
| `idle_steps` | Total compaction steps across all cycles |
| `idle_noops` | Steps that found nothing to compact |
| `hot_compactions` | Compactions triggered on the enqueue write path (should be 0) |
| `cap_exhausted` | Enqueues rejected because live data filled the store |

---

## `--strict` mode

Exits 1 on correctness anomalies only. The following must always hold:

- `enqueue`: `writeAt == N`, `readAt == 0`, `remove == 0`
- `peek_pop`: `writeAt == N`, `readAt == N`, `read_bpp == payloadBytes`, `remove == 0`
- `outbox_offline_submit`: `writeAt == N`, `read_bpp == 0`, `remove == 0`
- `mount`: `writeAt == 0`, `writeFile == 0`, `remove == 0`
- `idle_compaction`: `hot_compactions == 0`, `cap_exhausted == 0`

---

## Regression test

`tools/benchmark_regression.py` compares deterministic fields against the committed
baseline. Numeric fields fail on increase, pass silently on decrease (improvement).
Boolean fields (`ok`) fail if baseline is true and current is false.

Compared fields: `writeFile`, `writeAt`, `readAt`, `remove`, `write_amp`, `read_bpp`,
`idle_steps`, `idle_noops`, `hot_compactions`, `cap_exhausted`.

Not compared: wall-clock times (`p50_us` … `max_us`).

---

## Simulator

`tools/pqueue_compaction_sim.cpp` is a separate correctness sweep tool. It catches
deadlocks and strategy failures across a parameter matrix and is not replaced by the
benchmark. Build: `make -j12 sim`.
