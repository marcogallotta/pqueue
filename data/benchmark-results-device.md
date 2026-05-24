# On-device benchmark results — ESP32-S3

Hardware: ESP32-S3, QSPI flash, LittleFS  
Config: `reserved_bytes=2108736`, `max_segments=200`  
Environment: `esp32s3-benchmark` (full mode)  
Parameters: N=100, burst=100, cycles=2, heavy_burst=300, heavy_cycles=2

---

## 2026-05-24

### Enqueue / peek+pop

  scenario     payload    n     p50      p90      p99      max
  -----------  -------  ---  ------   ------   ------   ------
  enqueue       256B   100    81ms    140ms    334ms    353ms
  enqueue      1024B   100   169ms    530ms   1133ms   1312ms
  peek_pop      256B   100    83ms    103ms    331ms    331ms
  peek_pop     1024B   100    88ms    102ms    796ms    797ms
  raw_enqueue   256B   100    80ms    136ms    334ms    351ms
  raw_peek_pop  256B   100    83ms    102ms    330ms    330ms

Raw-buffer vs string-path difference is within noise at all percentiles.

### Mount

  mount    payload  preload      time
  -----   --------  -------  --------
  mount     256B        0        13ms
  mount     256B       50      1713ms
  mount     256B      200     16137ms
  mount     256B      500     30795ms
  mount     256B     1000     47148ms

Mount time is super-linear in record count. 50→200 (4×) = 9.4× time; 200→500 (2.5×) = 1.9× time; 500→1000 (2×) = 1.5× time. Root cause not yet investigated.

### Compaction (compactIdle)

  scenario           payload  burst  steps  noops    p50      p90      p99      max
  -----------------  -------  -----  -----  -----  ------   ------   ------   ------
  compact_idle        256B    100      4      2     787ms    947ms    947ms    947ms
  compact_idle_heavy  492B    300      4      2    5052ms   6753ms   6753ms   6753ms

Heavy workload (492B/burst=300) produces ~7× higher compaction latency than the light case (256B/burst=100). Both converge in 4 steps with 2 noops.
