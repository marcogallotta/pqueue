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

`env:esp32s3-idle-sanity` measures worst-case `compactIdle(1)` step latency on real
LittleFS under the heavy workload (burst=500/pop=90%/rec=492B/cycles=3). Flash and
run it to get authoritative device numbers.

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
