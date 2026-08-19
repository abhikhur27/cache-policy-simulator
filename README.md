# Cache Policy Simulator (C++)

Command-line cache simulator for comparing FIFO, LRU, and an optimal offline baseline on real access traces.

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

Sweep multiple capacities in one pass:

```bash
./cache_policy_sim sample_trace.txt 2-6
./cache_policy_sim sample_trace.txt 2,4,8
```

Write the comparison table to CSV for lab notes or spreadsheet review:

```bash
./cache_policy_sim sample_trace.txt 2-6 --csv-out reports/cache-sweep.csv
```

Write a Markdown brief you can drop into notes or a PR:

```bash
./cache_policy_sim sample_trace.txt 2-6 --markdown-out reports/cache-sweep.md
```

Write a JSON artifact for downstream analysis or automated checks:

```bash
./cache_policy_sim sample_trace.txt 2-6 --json-out reports/cache-sweep.json
```

Inspect locality shifts across a long trace with phase-local windows:

```bash
./cache_policy_sim sample_trace.txt 2-6 --phase-window 6 --markdown-out reports/cache-sweep.md
```

Run the deterministic three-regime fixture to verify that policy advice changes with the workload:

```bash
./cache_policy_sim fixtures/multi_phase_trace.txt 3 --phase-window 12
```

The fixture contains an LRU-favoring locality phase, a FIFO-favoring cyclic scan, and a hot set that fits entirely. The phase table reports the deployable online choice and LRU hit delta; OPT stays visible as an offline ceiling instead of being presented as the operational recommendation.

Surface the specific keys driving reload churn and evictions:

```bash
./cache_policy_sim sample_trace.txt 4 --top-keys 8 --markdown-out reports/cache-brief.md --json-out reports/cache-brief.json
```

Run the built-in regression checks:

```bash
./cache_policy_sim --self-test
```

Arguments:

- `trace_file`: Text file with integer keys (space or comma separated)
- `cache_capacity`: Positive integer cache size, ascending range, or comma-separated list
- `--phase-window N`: Optional phase-local analysis window size in accesses. Uses the largest requested capacity.
- `--top-keys N`: Optional number of per-policy hot/churn keys to include in console, Markdown, and JSON diagnostics.
- `--json-out path`: Optional machine-readable report with sweep and phase-local metrics.
- `--self-test`: Runs deterministic parser/simulation regression checks without a trace file.

## Output

- Hits / misses for FIFO, LRU, and OPT
- Cold vs reload miss breakdown
- Eviction count under each policy
- Hit-rate percentage for each policy
- Trace profile summary:
  - unique-key count
  - reuse rate
  - hottest key frequency
- Final cache contents
- Winner summary showing hit-count delta plus regret versus OPT
- Per-policy key pressure tables showing which keys drive reload misses and evictions
- Capacity-sweep table showing where FIFO/LRU diverge as cache size grows
- Belady anomaly check that flags when FIFO gets worse after adding capacity
- Optional phase-local table that resets the cache per window so changing locality is easy to spot
- FIFO-vs-LRU online choice and signed LRU hit delta for every capacity and phase, including CSV/Markdown/JSON exports
- Phase conclusion counts showing how often LRU leads, FIFO leads, or both tie
- Optional CSV export with one row per policy/capacity pair
- Optional Markdown brief with sweep, phase-local, and per-capacity policy details
- Optional JSON export with sweep metrics, final-cache state, and phase-local results

## Example workload

`sample_trace.txt` includes mixed locality to show where LRU usually outperforms FIFO.

## Example verification

```bash
zig c++ -std=c++17 -O2 -Wall -Wextra -pedantic cache_policy_sim.cpp -o cache_policy_sim
./cache_policy_sim --self-test
./cache_policy_sim fixtures/multi_phase_trace.txt 3 --phase-window 12 --top-keys 6 --markdown-out reports/cache-sweep.md --json-out reports/cache-sweep.json
```

That run should pass the self-test and show one LRU phase, one FIFO phase, and one tie while exporting the same conclusions to Markdown and JSON.

## Portfolio Demo Script

Use the same trace with at least two cache sizes when explaining the result:

1. Capacity 2 or 3 to show early eviction pressure.
2. Capacity 4 to show whether LRU actually converts locality into hit-rate lift.
3. Call out the winner summary only after reading the reuse profile so the claim stays tied to workload shape.

## Portfolio Positioning

- Project type: C++ command-line utility
- Verification path: zig c++ -std=c++17 -O2 -Wall -Wextra -pedantic cache_policy_sim.cpp -o cache_policy_sim && ./cache_policy_sim --self-test && ./cache_policy_sim sample_trace.txt 2-4 --phase-window 6

