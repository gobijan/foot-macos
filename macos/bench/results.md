# Benchmark results

Measured on 2026-10-25 with `make bench` ([macos/bench/bench.py](bench.py), method: [README.md](README.md)).

* Mac Studio (Mac13,1): Apple M1 Max, 8 performance and 2 efficiency cores, 32 GB
* macOS 27.0.1 (26A434); display: LG HDR WQHD+, 3840 x 1600 @ 144 Hz, scale 1; power: AC Power
* foot 1.28.0-macos.2-2-g61bb508f, kitty 0.49.2, Ghostty 1.3.2-main+f54340ad7
* Each terminal with its default configuration, font Menlo 14.
* Throughput: the median of 9 runs per workload (3 launches of each terminal, alternating, 3 runs each), ± half their range.
* Startup: the median of 5 launches, after one to warm up. Idle: one window, for 30 s.
* Load average (1 minute): 23.12 at the start, 3.95 at the end.
* Busy meanwhile: coreduetd (97%) at the start, dasd (88%), com.apple.Safari.SafeBrowsing.Service (71%) at the end.

## Throughput, 160x50

| MiB/s, 160x50 (higher is better) | foot | kitty | Ghostty |
| --- | --: | --: | --: |
| ASCII text | 90 ±12% | 117 ±2% | **240 ±7%** |
| Unicode text | 62 ±3% | 78 ±2% | **90 ±0%** |
| Colored cells | 112 ±2% | 124 ±1% | **126 ±0%** |
| Scrolling | **48 ±1%** | 9 ±0% | 46 ±4% |
| Scrolling region | 50 ±1% | 24 ±0% | **66 ±0%** |
| Cursor motion | 92 ±2% | 105 ±1% | **202 ±0%** |
| Window (pixels) | 1280×850 | 1440×800 | 1280×800 |

## Throughput, 400x88

| MiB/s, 400x88 (higher is better) | foot | kitty | Ghostty |
| --- | --: | --: | --: |
| ASCII text | 74 ±8% | 104 ±4% | **223 ±2%** |
| Unicode text | 51 ±2% | 74 ±1% | **91 ±1%** |
| Colored cells | 91 ±4% | 110 ±1% | **125 ±1%** |
| Scrolling | 22 ±1% | 4 ±1% | **40 ±7%** |
| Scrolling region | **22 ±2%** | 11 ±0% | 17 ±1% |
| Cursor motion | 81 ±3% | 96 ±1% | **202 ±1%** |
| Window (pixels) | 3200×1496 | 3600×1408 | 3200×1408 |

## Startup, and an idle window

| Lower is better | foot | kitty | Ghostty |
| --- | --: | --: | --: |
| Startup, until the command runs (ms) | **75 ±11%** | 683 ±1% | 155 ±1% |
| Memory, idle window 160x50 (MiB) | **31** | 86 | 62 |
| CPU, idle window (%) | **0.04** | 0.29 | 0.65 |
| Wakeups, idle window (per second) | **0.3** | 2.3 | 16.2 |
