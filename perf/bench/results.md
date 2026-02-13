# Benchmark Results

**Backend:** epoll | **Duration:** 1s per benchmark | **CPU affinity:** P-cores 0-11

> Higher is better for throughput. Lower is better for latency.
> Deltas are Corosio vs the respective Asio variant (positive = Corosio wins).

## Socket Throughput — Unidirectional

| Buffer | Corosio | Asio Coro | Asio CB | Delta (vs Coro) | Delta (vs CB) |
|-------:|--------:|----------:|--------:|-----------------:|--------------:|
| 1 KB | 494.69 MB/s | 367.31 MB/s | 374.93 MB/s | **+34.7%** | **+31.9%** |
| 4 KB | 1.84 GB/s | 1.46 GB/s | 1.46 GB/s | **+26.1%** | **+25.7%** |
| 16 KB | 6.00 GB/s | 5.05 GB/s | 5.27 GB/s | **+18.8%** | **+13.8%** |
| 64 KB | 10.34 GB/s | 10.59 GB/s | 10.15 GB/s | -2.4% | **+1.9%** |
| 128 KB | 11.95 GB/s | 11.35 GB/s | 11.37 GB/s | **+5.3%** | **+5.1%** |
| 256 KB | 11.10 GB/s | 10.44 GB/s | 11.20 GB/s | **+6.4%** | -0.8% |
| 512 KB | 10.67 GB/s | 9.64 GB/s | 9.65 GB/s | **+10.8%** | **+10.6%** |
| 1 MB | 10.58 GB/s | 10.11 GB/s | 10.32 GB/s | **+4.6%** | **+2.5%** |

## Socket Throughput — Bidirectional (combined)

| Buffer | Corosio | Asio Coro | Asio CB | Delta (vs Coro) | Delta (vs CB) |
|-------:|--------:|----------:|--------:|-----------------:|--------------:|
| 1 KB | 483.03 MB/s | 585.29 MB/s | 562.04 MB/s | -17.5% | -14.1% |
| 4 KB | 1.71 GB/s | 2.08 GB/s | 2.11 GB/s | -17.8% | -18.9% |
| 16 KB | 5.89 GB/s | 6.17 GB/s | 6.44 GB/s | -4.6% | -8.6% |
| 64 KB | 10.24 GB/s | 9.61 GB/s | 9.92 GB/s | **+6.6%** | **+3.3%** |
| 128 KB | 11.41 GB/s | 10.16 GB/s | 10.39 GB/s | **+12.3%** | **+9.8%** |
| 256 KB | 10.34 GB/s | 8.82 GB/s | 8.81 GB/s | **+17.2%** | **+17.4%** |
| 512 KB | 10.17 GB/s | 9.14 GB/s | 9.13 GB/s | **+11.3%** | **+11.4%** |
| 1 MB | 10.12 GB/s | 9.78 GB/s | 9.77 GB/s | **+3.4%** | **+3.6%** |

## Socket Throughput — Multithread (32 connections)

| Threads | Buffer | Corosio | Asio Coro | Asio CB | Delta (vs Coro) | Delta (vs CB) |
|--------:|-------:|--------:|----------:|--------:|-----------------:|--------------:|
| 2 | 64 KB | 10.55 GB/s | 9.03 GB/s | 9.22 GB/s | **+16.9%** | **+14.5%** |
| 2 | 128 KB | 9.92 GB/s | 7.34 GB/s | 7.43 GB/s | **+35.2%** | **+33.6%** |
| 2 | 256 KB | 8.59 GB/s | 6.59 GB/s | 6.57 GB/s | **+30.4%** | **+30.8%** |
| 2 | 512 KB | 8.14 GB/s | 6.08 GB/s | 6.54 GB/s | **+34.0%** | **+24.5%** |
| 4 | 64 KB | 17.93 GB/s | 13.96 GB/s | 13.64 GB/s | **+28.4%** | **+31.5%** |
| 4 | 128 KB | 16.42 GB/s | 13.24 GB/s | 13.58 GB/s | **+24.0%** | **+20.9%** |
| 4 | 256 KB | 13.98 GB/s | 9.07 GB/s | 10.45 GB/s | **+54.2%** | **+33.7%** |
| 4 | 512 KB | 12.16 GB/s | 9.18 GB/s | 9.21 GB/s | **+32.4%** | **+32.0%** |
| 8 | 64 KB | 25.42 GB/s | 21.99 GB/s | 19.32 GB/s | **+15.6%** | **+31.6%** |
| 8 | 128 KB | 21.05 GB/s | 18.70 GB/s | 16.71 GB/s | **+12.6%** | **+26.0%** |
| 8 | 256 KB | 17.41 GB/s | 15.04 GB/s | 12.85 GB/s | **+15.7%** | **+35.5%** |
| 8 | 512 KB | 15.06 GB/s | 11.72 GB/s | 11.85 GB/s | **+28.4%** | **+27.0%** |

## Socket Latency — Ping-Pong (round-trip)

| Msg Size | Metric | Corosio | Asio Coro | Asio CB | Delta (vs Coro) | Delta (vs CB) |
|---------:|:-------|--------:|----------:|--------:|-----------------:|--------------:|
| 1 B | mean | 4.74 us | 4.82 us | 4.44 us | **+1.7%** | -6.7% |
| 1 B | p99 | 6.00 us | 5.90 us | 4.99 us | -1.7% | -20.3% |
| 64 B | mean | 4.65 us | 4.80 us | 4.13 us | **+3.1%** | -12.6% |
| 64 B | p99 | 6.20 us | 5.98 us | 5.36 us | -3.7% | -15.6% |
| 1 KB | mean | 4.61 us | 4.86 us | 4.20 us | **+5.3%** | -9.7% |
| 1 KB | p99 | 5.93 us | 6.13 us | 5.21 us | **+3.3%** | -13.8% |

## Socket Latency — Concurrent Pairs (64-byte messages)

| Pairs | Metric | Corosio | Asio Coro | Asio CB | Delta (vs Coro) | Delta (vs CB) |
|------:|:-------|--------:|----------:|--------:|-----------------:|--------------:|
| 1 | mean | 4.07 us | 4.69 us | 4.53 us | **+13.3%** | **+10.1%** |
| 1 | p99 | 4.77 us | 5.94 us | 5.53 us | **+19.7%** | **+13.9%** |
| 4 | mean | 15.75 us | 17.16 us | 16.50 us | **+8.2%** | **+4.6%** |
| 4 | p99 | 18.39 us | 20.43 us | 20.26 us | **+10.0%** | **+9.2%** |
| 16 | mean | 63.22 us | 67.99 us | 61.96 us | **+7.0%** | -2.0% |
| 16 | p99 | 75.13 us | 80.88 us | 71.80 us | **+7.1%** | -4.6% |

## HTTP Server — Single Connection

| Library | Throughput | Mean Latency | p99 Latency |
|:--------|----------:|-------------:|------------:|
| Corosio | 206.74 Kops/s | 4.81 us | 6.15 us |
| Asio Coro | 218.70 Kops/s | 4.55 us | 5.75 us |
| Asio CB | 229.39 Kops/s | 4.34 us | 5.14 us |
| **Delta (vs Coro)** | **-5.5%** | **-5.9%** | -7.1% |
| **Delta (vs CB)** | -9.9% | -11.1% | -19.7% |

## HTTP Server — Concurrent Connections

| Conns | Metric | Corosio | Asio Coro | Asio CB | Delta (vs Coro) | Delta (vs CB) |
|------:|:-------|--------:|----------:|--------:|-----------------:|--------------:|
| 1 | Kops/s | 212.04 | 222.92 | 216.50 | -4.9% | -2.1% |
| 4 | Kops/s | 223.06 | 226.38 | 226.66 | -1.5% | -1.6% |
| 16 | Kops/s | 232.12 | 235.45 | 232.40 | -1.4% | -0.1% |
| 32 | Kops/s | 229.01 | 228.11 | 232.29 | **+0.4%** | -1.4% |

## HTTP Server — Multi-threaded (32 connections)

| Threads | Metric | Corosio | Asio Coro | Asio CB | Delta (vs Coro) | Delta (vs CB) |
|--------:|:-------|--------:|----------:|--------:|-----------------:|--------------:|
| 1 | Kops/s | 217.21 | 225.94 | 216.96 | -3.9% | **+0.1%** |
| 2 | Kops/s | 338.03 | 319.28 | 332.08 | **+5.9%** | **+1.8%** |
| 4 | Kops/s | 483.82 | 434.30 | 479.46 | **+11.4%** | **+0.9%** |
| 8 | Kops/s | 585.94 | 476.88 | 516.60 | **+22.9%** | **+13.4%** |
| 16 | Kops/s | 665.76 | 425.14 | 480.94 | **+56.6%** | **+38.4%** |

## Timer — Schedule/Cancel

| Library | Throughput | Delta (vs Coro) | Delta (vs CB) |
|:--------|----------:|-----------------:|--------------:|
| Corosio | 50.47 Mops/s | **+37.7%** | **+36.7%** |
| Asio Coro | 36.66 Mops/s | — | — |
| Asio CB | 36.92 Mops/s | — | — |

## Timer — Fire Rate

| Library | Throughput | Delta (vs Coro) | Delta (vs CB) |
|:--------|----------:|-----------------:|--------------:|
| Corosio | 7.11 Mops/s | **+1886.5%** | **+1875.5%** |
| Asio Coro | 358.06 Kops/s | — | — |
| Asio CB | 360.05 Kops/s | — | — |

## Timer — Concurrent

| Timers | Metric | Corosio | Asio Coro | Asio CB | Delta (vs Coro) | Delta (vs CB) |
|-------:|:-------|--------:|----------:|--------:|-----------------:|--------------:|
| 10 | Kops/s | 28.64 | 28.88 | 29.04 | -0.8% | -1.4% |
| 10 | mean | 554.11 us | 552.55 us | 551.51 us | -0.3% | -0.5% |
| 100 | Kops/s | 256.57 | 257.30 | 257.17 | -0.3% | -0.2% |
| 100 | mean | 551.95 us | 551.21 us | 551.36 us | -0.1% | -0.1% |
| 1000 | Kops/s | 2.51 M | 2.52 M | 2.53 M | -0.6% | -0.9% |
| 1000 | mean | 555.28 us | 553.67 us | 552.83 us | -0.3% | -0.4% |

## Accept Churn — Sequential

| Library | Throughput | Mean Latency | p99 Latency | Delta (vs Coro) | Delta (vs CB) |
|:--------|----------:|-------------:|------------:|-----------------:|--------------:|
| Corosio | 60.35 Kops/s | 16.54 us | 33.08 us | **+3.4%** | -0.7% |
| Asio Coro | 58.39 Kops/s | 17.08 us | 24.46 us | — | — |
| Asio CB | 60.80 Kops/s | 16.42 us | 23.49 us | — | — |

## Accept Churn — Concurrent

| Loops | Metric | Corosio | Asio Coro | Asio CB | Delta (vs Coro) | Delta (vs CB) |
|------:|:-------|--------:|----------:|--------:|-----------------:|--------------:|
| 1 | Kops/s | 64.45 | 60.57 | 65.96 | **+6.4%** | -2.3% |
| 4 | Kops/s | 63.04 | 63.28 | 64.94 | -0.4% | -2.9% |
| 16 | Kops/s | 65.02 | 47.98 | 62.72 | **+35.5%** | **+3.7%** |

## Accept Churn — Burst

| Burst | Metric | Corosio | Asio Coro | Asio CB | Delta (vs Coro) | Delta (vs CB) |
|------:|:-------|--------:|----------:|--------:|-----------------:|--------------:|
| 10 | Kops/s | 83.49 | 81.83 | 86.58 | **+2.0%** | -3.6% |
| 10 | mean | 119.71 us | 122.03 us | 115.47 us | **+1.9%** | -3.7% |
| 100 | Kops/s | 73.65 | 78.26 | 78.28 | -5.9% | -5.9% |
| 100 | mean | 1.36 ms | 1.28 ms | 1.28 ms | -6.2% | -6.3% |

## Fan-Out — Fork-Join

| Fan-out | Metric | Corosio | Asio Coro | Asio CB | Delta (vs Coro) | Delta (vs CB) |
|--------:|:-------|--------:|----------:|--------:|-----------------:|--------------:|
| 1 | Kops/s | 215.70 | 179.14 | 237.79 | **+20.4%** | -9.3% |
| 1 | mean | 4.61 us | 5.56 us | 4.19 us | **+17.1%** | -10.2% |
| 4 | Kops/s | 57.25 | 55.58 | 62.84 | **+3.0%** | -8.9% |
| 4 | mean | 17.45 us | 17.97 us | 15.89 us | **+2.9%** | -9.8% |
| 16 | Kops/s | 14.53 | 14.70 | 15.89 | -1.2% | -8.5% |
| 16 | mean | 68.78 us | 67.99 us | 62.91 us | -1.2% | -9.3% |
| 64 | Kops/s | 3.54 | 3.66 | 3.71 | -3.4% | -4.8% |
| 64 | mean | 282.69 us | 272.93 us | 269.19 us | -3.6% | -5.0% |

## Fan-Out — Nested

| Groups x Subs | Metric | Corosio | Asio Coro | Asio CB | Delta (vs Coro) | Delta (vs CB) |
|:--------------|:-------|--------:|----------:|--------:|-----------------:|--------------:|
| 4x4 (16) | Kops/s | 14.02 | 13.30 | 14.89 | **+5.3%** | -5.9% |
| 4x4 (16) | mean | 71.32 us | 75.12 us | 67.12 us | **+5.1%** | -6.2% |
| 4x16 (64) | Kops/s | 3.38 | 3.43 | 3.63 | -1.4% | -6.9% |
| 4x16 (64) | mean | 295.93 us | 291.71 us | 275.44 us | -1.4% | -7.4% |

## Fan-Out — Concurrent Parents (fan-out=16)

| Parents | Metric | Corosio | Asio Coro | Asio CB | Delta (vs Coro) | Delta (vs CB) |
|--------:|:-------|--------:|----------:|--------:|-----------------:|--------------:|
| 1 | Kops/s | 14.16 | 13.91 | 15.10 | **+1.8%** | -6.2% |
| 1 | mean | 70.60 us | 71.85 us | 66.20 us | **+1.7%** | -6.6% |
| 4 | Kops/s | 13.49 | 13.84 | 14.62 | -2.5% | -7.7% |
| 4 | mean | 296.44 us | 288.83 us | 273.54 us | -2.6% | -8.4% |
| 16 | Kops/s | 12.31 | 12.72 | 13.62 | -3.2% | -9.6% |
| 16 | mean | 1.30 ms | 1.31 ms | 1.17 ms | **+0.7%** | -10.6% |
