# Performance

Real application-visible `Queue` latency on ESP32-S3 LittleFS. Numbers are from the
on-device benchmark suite. A POSIX structural benchmark exists for CI regression; see
`docs/internals.md`.

---

## Run the hardware benchmark

Two environments share the same test file:

| Environment | Mode | Approx. runtime | N | burst | mount preloads |
|---|---|---|---|---|---|
| `esp32s3-benchmark` | full | ~17 min | 100 | 100 | 0 / 50 / 200 / 500 / 1000 |
| `esp32s3-benchmark-fast` | fast | ~1–2 min | 10 | 20 | 0 / 50 / 200 |

Fast mode is enough to confirm operations succeed and latency is in the right order of
magnitude. Use full mode for release measurements.

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

**Config:** `reserved_bytes=2108736`, `max_segments=200`.

---

## Scenarios

| Scenario | Payload sizes | What is timed |
|---|---|---|
| `enqueue` | 256 B, 1 KB | Each `q.enqueue()` call; payloads pre-built before the loop |
| `peek_pop` | 256 B, 1 KB | Each `q.peek()` + `q.pop()` pair |
| `raw_enqueue` | 256 B | Each `q.enqueue(Span)` call (full mode only) |
| `raw_peek_pop` | 256 B | Each `q.peek(MutableSpan)` + `q.pop()` pair (full mode only) |
| `mount` | 256 B (fixed) | First `q.statsResult()` after construction — captures lazy-mount I/O |
| `compact_idle` | 256 B | Each productive `q.compactIdle(1)` step; burst=100/pop=90% |
| `compact_idle_heavy` | 492 B | Each productive `q.compactIdle(1)` step; burst=300/pop=90% (full mode only) |

Columns: `p50/p90/p99/max` are wall-clock latency percentiles in ms across N operations.
Mount preload of 1000 records takes ~30 s to set up; progress is printed every 100 records.

---

## Representative results

**Operations** — full run, N=100:

```
  scenario     payload    n     p50      p90      p99      max
  ----------  --------  ---  ------   ------   ------   ------
  enqueue       256B   100      81ms     139ms     339ms     354ms
  enqueue      1024B   100     168ms     533ms    1139ms    1322ms
  peek_pop      256B   100      84ms     102ms     332ms     332ms
  peek_pop     1024B   100      88ms     104ms     797ms     800ms
  raw_enqueue   256B   100      80ms     136ms     334ms     351ms
  raw_peek_pop  256B   100      83ms     102ms     330ms     330ms

  scenario           payload  burst  steps  noops    p50      p90      p99      max
  compact_idle        256B    100      4      2     787ms     947ms     947ms     947ms
  compact_idle_heavy  492B    300      4      2    5052ms    6753ms    6753ms    6753ms
```

**Mount** — fast run, 256 B payload:

```
  preload   time
  -------   ----
  0         13ms
  50        405ms
  200       4566ms
```

---

## Results discussion

LittleFS file I/O dominates: open/close, flush on writes, and metadata lookup costs dwarf
the payload copy or read. Everything below follows from that.

### Enqueue

p50 is the steady-state append cost. At 256 B this is ~81 ms; at 1024 B it is ~169 ms —
larger payloads cost more to write and flush.

p99 and max reflect segment rollover events. When the current segment is full, the queue
seals it, creates a new one, and publishes the manifest (~45 ms extra), then the record
lands in the fresh segment. Per-record size on disk = 24 B overhead + payload, giving:

  payload   records/segment   rollover frequency
  -------   ---------------   ------------------
  256 B     ~14               ~1 in 14  (~7%)
  1024 B     3                ~1 in 3   (~33%)

At 256 B, rollover cost appears only at p99 and max. At 1024 B, ~33 of 100 samples are
rollovers, so rollover cost pushes p90 up as well.

### Peek+pop

p50 is nearly payload-size independent: 83 ms at 256 B, 88 ms at 1024 B. A pop writes
a small durable pop marker regardless of payload size; that write dominates and does not
scale with payload.

The headline result: drain throughput is payload-size independent. The burst-drain
workload performs as well at 1024 B as at 256 B.

Rare flash/filesystem tail outliers exist (p99 ≈ max in some cases) with no queue-level
cause.

### Compaction (compactIdle)

Each `compactIdle(1)` step selects the range with the highest dead/total byte ratio,
collects all live records, rewrites them into new segments, and removes the old ones. Cost
scales with the number of segment files opened and removed, and the volume of live data
rewritten.

The light workload (256 B/burst=100) completes in 787–947 ms per step. The heavy workload
(492 B/burst=300) takes 5052–6753 ms per step — a single step can span dozens of input
segments.

**6.7 s is the planning number.** `compactIdle(1)` can block for up to ~7 s under the
heavy workload. Callers that cannot tolerate a stall of that length should call
`compactIdle` more frequently during idle time to keep individual steps small.

### Mount

Mount cost grows with backlog and segment history. A fully drained queue
mounts in ~13 ms. A large backlog produces multi-second mount.

If your use case can accumulate a large backlog across a reboot, treat mount latency as
part of your boot budget. Draining the queue before shutdown is the most effective way to
keep boot fast.

### Raw buffer vs std::string

`enqueue(Span)` and `peek(MutableSpan)` produce identical latency to their `std::string`
counterparts at every percentile. The raw-buffer API avoids a caller-side allocation; it
is not a latency optimization.

---

## Tuning

  operation     lever                         effect
  -----------   ----------------------------  ---------------------------------------------
  enqueue       smaller payload               fewer rollovers; lower p50 and p99
  enqueue       larger segment (e.g. 8 KB)    fewer rollovers but higher steady-state flush
                                              cost; trade-off needs a targeted benchmark
  peek_pop      —                             no lever; drain throughput is payload-independent
  compactIdle   call more frequently          prevents large cleanup batches; once a large
                                              backlog exists, a single step can still be
                                              expensive
  mount         drain before shutdown         keeps backlog near zero; fast boot
