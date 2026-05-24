# On-device benchmark results -- ESP32-S3

Hardware: ESP32-S3, QSPI flash, LittleFS  
Config: `reserved_bytes=2108736`, `max_segments=200`  
Environment: `esp32s3-benchmark` (full mode)  
Parameters: N=100, burst=100, cycles=2, heavy_burst=300, heavy_cycles=2

---

## 2026-05-24

### Enqueue / peek+pop

  scenario     payload    n     p50      p90      p99      max
  -----------  -------  ---  ------   ------   ------   ------
  enqueue       256B   100    80ms    141ms    336ms    354ms
  enqueue      1024B   100   165ms    539ms   1138ms   1316ms
  peek_pop      256B   100    83ms    101ms    330ms    333ms
  peek_pop     1024B   100    88ms    103ms    797ms    799ms
  raw_enqueue   256B   100    80ms    139ms    337ms    354ms
  raw_peek_pop  256B   100    81ms    102ms    328ms    329ms

Raw-buffer vs string-path difference is within noise at all percentiles.

### Mount

  mount    payload  preload      time
  -----   --------  -------  --------
  mount     256B        0        13ms
  mount     256B       50       442ms
  mount     256B      200      4925ms
  mount     256B      500     14650ms
  mount     256B     1000     27903ms

Mount time is super-linear in record count. 50->200 (4x) = ~11x time; 200->500 (2.5x) = ~3x time; 500->1000 (2x) = ~1.9x time. Root cause not yet investigated.

### Compaction (compactIdle)

  scenario           payload  burst  steps  noops    p50      p90      p99      max
  -----------------  -------  -----  -----  -----  ------   ------   ------   ------
  compact_idle        256B    100      4      2     783ms    942ms    942ms    942ms
  compact_idle_heavy  492B    300      4      2    5058ms   6745ms   6745ms   6745ms

Heavy workload (492B/burst=300) produces ~7x higher compaction latency than the light case (256B/burst=100). Both converge in 4 steps with 2 noops.
