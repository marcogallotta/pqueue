# Benchmark

Two complementary tools cover performance: a POSIX structural benchmark for CI
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

### Workload matrix

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

### Output columns

#### Enqueue, peek+pop, outbox submit, mount

| Column | Description |
|---|---|
| `p50_us` `p90_us` `p99_us` `max_us` | Host wall-clock latency percentiles — informational, machine-dependent |
| `write_amp` | Bytes written per payload byte (`bytesWritten / (N × payloadBytes)`) |
| `read_bpp` | Bytes read per record (`bytesRead / N`) |
| `writeFile` `writeAt` `readAt` `remove` | I/O op counts |

#### Idle compaction

| Column | Description |
|---|---|
| `idle_steps` | Total compaction steps across all cycles |
| `idle_noops` | Steps that found nothing to compact |
| `hot_compactions` | Compactions triggered on the enqueue write path (should be 0) |
| `cap_exhausted` | Enqueues rejected because live data filled the store |

### `--strict` mode

Exits 1 on correctness anomalies only. The following must always hold:

- `enqueue`: `writeAt == N`, `readAt == 0`, `remove == 0`
- `peek_pop`: `writeAt == N`, `readAt == N`, `read_bpp == payloadBytes`, `remove == 0`
- `outbox_offline_submit`: `writeAt == N`, `read_bpp == 0`, `remove == 0`
- `mount`: `writeAt == 0`, `writeFile == 0`, `remove == 0`
- `idle_compaction`: `hot_compactions == 0`, `cap_exhausted == 0`

### Regression test

`tools/benchmark_regression.py` compares deterministic fields against the committed
baseline. Numeric fields fail on increase, pass silently on decrease (improvement).
Boolean fields (`ok`) fail if baseline is true and current is false.

Compared fields: `writeFile`, `writeAt`, `readAt`, `remove`, `write_amp`, `read_bpp`,
`idle_steps`, `idle_noops`, `hot_compactions`, `cap_exhausted`.

Not compared: wall-clock times (`p50_us` … `max_us`).

---

## On-device benchmarks — real LittleFS latency

The POSIX benchmark does **not** predict device latency. LittleFS op costs vary with
filesystem load, file size, and flash wear state in ways a fixed-cost model cannot
capture. For real timing numbers, run the on-device tests against actual hardware.

### `env:esp32s3-benchmark` — structured latency benchmark

The primary on-device benchmark. Measures application-visible `Queue` latency across
six scenarios on real LittleFS. Uses the `std::string` Queue API — numbers reflect
the full string-path overhead that production callers pay.

**Config:** `reserved_bytes=2108736`, `max_segments=200`, matching the
`esp32s3-compaction` config for direct comparability.

Two benchmark environments share the same test file; a build flag selects the parameter set:

| Environment | Mode | Approx. runtime | N | burst | cycles | mount preloads |
|---|---|---|---|---|---|---|
| `esp32s3-benchmark` | full | ~17 min | 100 | 100 | 2 | 0 / 50 / 200 / 500 / 1000 |
| `esp32s3-benchmark-fast` | fast | ~1–2 min | 10 | 20 | 1 | 0 / 50 / 200 |

Fast mode is enough to confirm operations succeed and latency is in the right order of
magnitude. Use full mode for release measurements and baseline capture.

```bash
~/venvs/esp/bin/pio test -e esp32s3-benchmark --without-testing
~/venvs/esp/bin/pio device monitor

# or fast mode:
~/venvs/esp/bin/pio test -e esp32s3-benchmark-fast --without-testing
~/venvs/esp/bin/pio device monitor
```

The test runner swallows non-Unity serial lines; always use `--without-testing` +
`pio device monitor`. Press reset after the monitor connects if the device has already
booted.

**Output.** Each scenario emits a machine-readable `bench key=value` line as it
completes (grep-friendly, paste-ready into a spreadsheet). After all tests finish, a
human-readable summary table is printed in ms (example below is pre-whole-segment-replay;
current mount numbers are in the Results discussion):

```
==================== results ====================

  scenario     payload    n     p50      p90      p99      max
  enqueue       256B   100      81ms     139ms     339ms     354ms
  enqueue      1024B   100     168ms     533ms    1139ms    1322ms
  peek_pop      256B   100      84ms     102ms     332ms     332ms
  peek_pop     1024B   100      88ms     104ms     797ms     800ms
  raw_enqueue   256B   100      80ms     136ms     334ms     351ms
  raw_peek_pop  256B   100      83ms     102ms     330ms     330ms

  mount    payload  preload      time
  mount     256B        0        13ms
  mount     256B       50      1713ms
  mount     256B      200     16137ms
  mount     256B      500     30795ms
  mount     256B     1000     47148ms

  scenario           payload  burst  steps  noops    p50      p90      p99      max
  compact_idle        256B    100      4      2     787ms     947ms     947ms     947ms
  compact_idle_heavy  492B    300      4      2    5052ms    6753ms    6753ms    6753ms

=================================================
```

**Scenarios:**

| Scenario | Payload sizes | What is timed |
|---|---|---|
| `enqueue` | 256 B, 1 KB | Each `q.enqueue(std::string)` call; payloads pre-built before the loop |
| `peek_pop` | 256 B, 1 KB | Each `q.peek(std::string)` + `q.pop()` pair |
| `raw_enqueue` | 256 B | Each `q.enqueue(Span)` call; measures raw-buffer path vs string path (full mode only) |
| `raw_peek_pop` | 256 B | Each `q.peek(MutableSpan)` + `q.pop()` pair (full mode only) |
| `mount` | 256 B (fixed) | `q.statsResult()` after construction — captures lazy-mount I/O |
| `compact_idle` | 256 B | Each productive `q.compactIdle(1)` step; workload burst=100/pop=90%/cycles=2 |
| `compact_idle_heavy` | 492 B | Each productive `q.compactIdle(1)` step; heavier workload burst=300/pop=90%/cycles=2 (full mode only) |

Mount preload of 1000 records takes ~30 s to set up; progress is printed every 100
records.

---

## Results discussion

The dominant cost in every operation is LittleFS I/O: open, flush, and close each carry
fixed overhead that dwarfs the actual read or write. Everything below follows from that.

### Enqueue

p50 is the steady-state append cost — open the tail segment, write the record, flush,
close. At 256 B this is ~81 ms; at 1024 B it is ~169 ms, because a larger payload means
a larger file write and higher flush cost.

p99 and max reflect rotation events. The enqueue path checks capacity before writing:
when the payload would overflow the tail segment, `rotateSegment` fires first (seal
current segment + create new header + publish manifest, ~45 ms total), then the record
lands in the fresh segment at low cost. Per-record size on disk = 24 B event overhead +
payload, giving:

  payload   record on disk   records/segment   rotation frequency
  -------   --------------   ---------------   ------------------
  256 B     ~280 B           ~14               ~1 in 14  (~7%)
  1024 B    ~1048 B           3                ~1 in 3   (~33%)

At 256 B, rotation cost appears only at p99 and max (~7 events in N=100). At 1024 B,
~33 of 100 samples are rotations, so rotation cost pushes p90 up as well.

### Peek+pop

p50 is nearly payload-size independent: 83 ms at 256 B, 88 ms at 1024 B. Peek reads
the payload from a sealed segment (~5 ms); pop appends a fixed 20-byte POP tombstone to
the tail (~60 ms). The tombstone cost dominates and does not scale with payload size —
the opposite of enqueue, where writing the full payload drives p50.

The distribution is tighter than enqueue. p90 sits only ~20 ms above p50 because a
20-byte tombstone almost never overflows the tail segment. In this benchmark it cannot
overflow at all: after 100 enqueues the active tail holds 2 records at 256 B (580 B) or
1 record at 1024 B (1068 B); 100 POP tombstones add 2000 B, leaving the tail at 2580 B
and 3068 B respectively — both well under 4 KB.

p99 and max are single-sample outliers (p99 ≈ max) with no queue-level explanation —
most likely a LittleFS flash-state event (block erase or CoW) on that one write.

The headline result: drain throughput is payload-size independent. The burst-drain
workload performs as well at 1024 B as at 256 B.

### Compaction (compactIdle)

Each `compactIdle(1)` step selects the range with the highest dead/total byte ratio,
collects all live records from it, rewrites them into new segment(s), publishes the
manifest, and removes the old segments. Cost scales with the number of segment files
opened and removed, and the volume of live data rewritten.

The light workload (256 B/burst=100) completes in 787–947 ms per step. The heavy workload
(492 B/burst=300) takes 5052–6753 ms per step — a single productive step in the heavy
case can span dozens of input segment files, which is why the cost is seconds rather than
milliseconds.

**6.7 s is the caller's planning number.** `compactIdle(1)` blocks for up to ~7 s under
the heavy workload. Callers that cannot tolerate a single stall of that length should
call `compactIdle` more frequently to spread the work across smaller idle windows.

### Mount latency

Mount does a full replay on every boot: reads the manifest, then scans every event in
every active segment to reconstruct the in-RAM record index (`records_`). The manifest
stores segment layout only — record-level metadata (sequence, generation, payload offset
and size) is never persisted and must be derived by replaying events. `scanSegments` reads
each segment with a single `readFile` call (~5 ms flat for 4 KB), then parses all events
from the in-RAM buffer — one whole-segment read per segment regardless of event count.

Post whole-segment replay fast benchmark:

  preload   time      per-record
  -------   --------  ----------
  0         13ms      —
  50        405ms     ~8ms
  200       4566ms    ~23ms

The 500/1000-record post-fix mount cases have not been remeasured; avoid rerunning the
full hardware benchmark unless release evidence is needed.

The 13 ms baseline is manifest reads plus an empty directory scan. The 50→200 jump is
anomalous (4× records, 11.3× time) — likely LittleFS filesystem-state cost growing with
segment count/file count; the exact source has not been isolated.

The next candidate is a persisted checkpoint index: store `{seq, gen, offset, size}` per
record, validate against the manifest epoch on mount, and replay only events written after
the checkpoint.

### Raw-buffer API vs std::string

`enqueue(Span)` and `peek(MutableSpan)` produce identical latency to their `std::string`
counterparts at every percentile (within 1 ms). Neither path is zero-copy: `enqueue(Span)`
copies into a `std::string` internally; `peek(MutableSpan)` reads into a temporary
`std::string` then copies out. A 256 B memcpy costs ~1 µs; a LittleFS open+flush costs
~24 ms. The copy disappears into noise.

The result confirms the `std::string` API overhead is not the latency issue. The
raw-buffer API is for callers that want to avoid a caller-side allocation, not for
hot-path latency.

### Tuning summary

  operation     lever                       effect
  -----------   --------------------------  -----------------------------------------------
  enqueue       smaller payload             fewer rotations; lower p50 and p99
  enqueue       larger segment (e.g. 8 KB)  fewer rotations but higher steady-state flush
                                            cost; trade-off needs a targeted benchmark
  peek_pop      —                           no lever; tombstone write is the minimum;
                                            drain throughput is already payload-independent
  compactIdle   call more frequently        spreads work across more idle windows;
                                            per-step cost is unchanged

---

## Simulator

`tools/pqueue_compaction_sim.cpp` is a separate correctness sweep tool. It catches
deadlocks and strategy failures across a parameter matrix and is not replaced by the
benchmark. Build: `make -j12 sim`.
