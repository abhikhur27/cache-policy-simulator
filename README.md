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
- Cold vs reload miss breakdown
- Eviction count under each policy
- Hit-rate percentage for each policy
- Trace profile summary:
  - unique-key count
  - reuse rate
  - hottest key frequency
- Final cache contents
- Winner summary showing hit-count delta

## Example workload

`sample_trace.txt` includes mixed locality to show where LRU usually outperforms FIFO.

## Example verification

```bash
g++ -std=c++17 -O2 -Wall -Wextra -pedantic cache_policy_sim.cpp -o cache_policy_sim
./cache_policy_sim sample_trace.txt 4
```

That run should print the trace profile first, then separate FIFO/LRU result blocks and a final winner summary.

## Portfolio Positioning

- Project type: C++ command-line utility
- Verification path: g++ -std=c++17 -O2 -Wall -Wextra -pedantic cache_policy_sim.cpp -o cache_policy_sim && ./cache_policy_sim sample_trace.txt 4

