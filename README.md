# Cache Policy Simulator (C++)

Command-line cache simulator for comparing FIFO vs LRU hit behavior on real access traces.

## Why this exists

When tuning a workflow cache, policy choice changes miss pressure and downstream latency. This tool gives a fast, reproducible way to compare two common eviction strategies on the same trace file.

## Build

```bash
g++ -std=c++17 -O2 -Wall -Wextra -pedantic cache_policy_sim.cpp -o cache_policy_sim
```

## Run

```bash
./cache_policy_sim sample_trace.txt 4
```

Arguments:

- `trace_file`: Text file with integer keys (space or comma separated)
- `cache_capacity`: Positive integer cache size

## Output

- Hits / misses for FIFO and LRU
- Hit-rate percentage for each policy
- Final cache contents
- Winner summary showing hit-count delta

## Example workload

`sample_trace.txt` includes mixed locality to show where LRU usually outperforms FIFO.
