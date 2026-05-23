# Benchmark

Repeatable benchmark report for the append-log store. One command produces a
table of latency percentiles, write amplification, and I/O op counts for the
scenarios that matter at launch: enqueue, peek+pop, mount, outbox submit, and
idle compaction.

Separate from `tools/pqueue_profiling.cpp`, which is an interactive developer
tool for investigating specific workloads. The benchmark binary is a stable
one-shot report suitable for README, docs, and CI regression.

---

## Status

**Implemented:** binary skeleton, `--json` / `--markdown` / `--strict` / `--repeat K`, enqueue scenario (64B / 256B / 1KB / 2KB), peek+pop scenario (64B / 256B / 1KB / 2KB), outbox offline submit scenario (256B / 1KB), `write_amp`, `read_bpp`, per-op simulated latency sampling, strict-mode invariant checks.

**Pending:** mount scenario; idle compaction scenario and its dedicated output columns.

---

## Build and run

```bash
make -j12 benchmark
./build/pqueue-benchmark [--markdown] [--json] [--strict] [--repeat K]
```

`--repeat K` repeats each workload cell K times and aggregates across all runs.
Omitting `--repeat` runs each cell once. Use `--repeat 5` or higher when you
need stable percentile estimates.

---

## Workload matrix

| Scenario | N | Payload sizes |
|---|---|---|
| enqueue | 1000 | 64 B, 256 B, 1 KB, 2 KB |
| peek+pop | 1000 | 64 B, 256 B, 1 KB, 2 KB |
| outbox offline submit | 1000 | 256 B, 1 KB |
| mount | — | 0, 100, 1000 pre-loaded records |
| idle compaction step | burst=500 / cycles=3 / pop=90% | 256 B, 492 B |

Each cell performs N operations exactly once per run. `--repeat K` runs the
full cell K times; reported percentiles are computed across all K × N samples.

**Payload size and store config.** Each scenario configures the store with
`recordSizeBytes` set large enough to hold the largest payload under test. This
ensures every scenario benchmarks queue performance, not enqueue rejection.

**Outbox "offline" definition.** The outbox offline submit scenario uses a fake
transport that returns `RetryLater` (never succeeds). This ensures the benchmark
measures the failed-send + durable-enqueue path, not a live send that happens to
succeed.

**Mount capacity.** The mount scenarios use a fixed payload of 256 B regardless
of the workload matrix payload column. The setup store is configured with
capacity sufficient for 1000 records at that size. This prevents segment
pressure or capacity rejection during setup from contaminating the mount timing.

---

## Output columns

### Enqueue, peek+pop, outbox submit, mount

| Column | Description |
|---|---|
| `p50_us` `p90_us` `p99_us` `max_us` | Host wall-clock latency percentiles |
| `sim_p99_ms` `sim_max_ms` | Simulated LittleFS latency per operation (× 100 = predicted device ms at default calibration) |
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
| `fs_floor_hit` | Enqueues rejected by `minFreeBytes` FS floor |
| `max_step_sim_ms` | Worst single `compactIdle(1)` step in simulated time |
| `total_idle_sim_ms` | Sum of all idle compaction steps in simulated time |

Multiply simulated ms by 100 for predicted on-device ms at default calibration
(ESP32S3 with QSPI flash, `--multiplier 1.0`).

---

## Host timing vs simulated latency

**Host percentiles** (`p50_us` … `max_us`) are wall-clock measurements on the
POSIX in-memory FS. They vary by host machine and are primarily useful for
catching structural regressions on a fixed CI machine (e.g. an O(n) path
becoming O(n²)).

**Simulated latency** (`sim_*`) is deterministic and machine-independent. It is
the primary basis for launch claims and on-device predictions. These are the
numbers worth publishing in README or docs.

The simulation uses `CountingFileSystem` with the calibrated latency model from
`tools/pqueue_profiling.cpp` (`littleFsSimLatency()`). Extending the model to
enqueue, peek+pop, mount, and outbox scenarios requires threading
`CountingFileSystem::setLatency()` through those workload setup paths — the
same pattern used for compaction scenarios in the profiling tool.

**Per-operation latency sampling.** `sim_p99_ms` and `sim_max_ms` require
per-operation simulated cost, not just an aggregate total. The implementation
must snapshot `CountingFileSystem::counters().simLatencyUs` before and after
each individual API call (each `enqueue`, `pop`, `mount`, etc.) to get a
per-call simulated cost sample. Aggregating across the whole cell and dividing
is not sufficient — it loses the distribution.

---

## Mount benchmark

**Setup:** enqueue N records into a fresh store on a temp directory using a
fixed 256 B payload, then construct a new store instance pointing at the same
directory and call `mount()`. Only the `mount()` call is timed; setup cost is
excluded.

**Sizes:** 0, 100, 1000 pre-loaded records.

The setup store is configured with capacity sufficient for 1000 records at
256 B so no enqueue fails during setup regardless of which size is being timed.

Unmount / destructor time is not measured.

---

## Output formats

**`--json`** is the primary output format. Implement it from the start; it is
the basis for CI regression diffs. Schema:

```json
{
  "config": {
    "gitHash": "...",
    "maxSegmentBytes": 4096,
    "maxSegments": 200,
    "maxTotalBytes": 1048576,
    "maxOutputSegments": 8,
    "platform": "posix"
  },
  "results": [
    {
      "scenario": "enqueue",
      "payloadBytes": 256,
      "records": 1000,
      "repeat": 1,
      "p50_us": 0, "p90_us": 0, "p99_us": 0, "max_us": 0,
      "sim_p99_ms": 0.0, "sim_max_ms": 0.0,
      "write_amp": 0.0, "read_bpp": 0.0,
      "writeFile": 0, "writeAt": 0, "readAt": 0, "remove": 0,
      "ok": true
    }
  ]
}
```

**`--markdown`** produces a table for pasting into README or docs. The report
header includes the same fields as `config` above.

---

## `--strict` mode

Exits 1 on correctness anomalies only:

- `hot_compactions > 0` in the idle compaction scenario
- any workload row where `ok` is false

No write-amplification or latency thresholds are hardcoded. Numeric regression
limits belong in CI configuration once baseline data exists.

---

## Regression thresholds

Per-metric regression limits (e.g. "sim_p99 must not increase by more than 5%")
live in CI configuration, not in the binary. CI stores a baseline `--json`
snapshot and diffs against each run.

---

## Related: I/O count regression test

A separate piece of work, distinct from this benchmark binary: promote the
red-flag checks in `tools/pqueue_profiling.cpp` to hard failures and add a CI
step that runs `./build/pqueue-profiling all` with `--strict`. The I/O counts
it checks (`writeAt`, `readAt`, metadata ops) are fully deterministic against
the in-memory FS — exact regression targets, no baseline snapshot needed.

---

## Profiling tool removal

`tools/pqueue_profiling.cpp` is marked for deletion once both this benchmark
binary and the I/O count regression test are complete. Its scenarios are
superseded by the benchmark workload matrix; its red-flag logic is superseded
by the regression test. The `compaction`/`compaction-sim` burst modes (external
trigger, custom dead-ratio logic) are not replicated in the benchmark and should
be evaluated for value before the file is removed.

`tools/pqueue_compaction_sim.cpp` is **not** deleted. It is a correctness sweep
tool that catches deadlocks and strategy failures across a parameter matrix. The
benchmark does not replace it.

---

## Release results

Actual benchmark output lives in `docs/benchmark-results.md`. That file is
generated by running:

```bash
./build/pqueue-benchmark --markdown
```

and committing the output. It records the numbers that back any launch claims
in README or docs, and serves as the human-readable baseline. The JSON
equivalent (`--json`) is the machine-readable baseline used by CI.

`docs/benchmark-results.md` also contains an interpretation guide: what each
metric means in practice, what values are acceptable, and what to investigate
if a number looks wrong.

---

## Calibration sampler

Out of scope for this task. A separate on-device tool would measure actual
LittleFS op latencies and output a `--multiplier` value for the simulator.
Deferred post-launch.
