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

Inspect locality shifts and boundary effects across a long trace with phase windows:

```bash
./cache_policy_sim sample_trace.txt 2-6 --phase-window 6 --markdown-out reports/cache-sweep.md
```

Separate each live phase's transition warm-up from its steady state:

```bash
./cache_policy_sim sample_trace.txt 4 --phase-window 12 --phase-warmup 4
```

Weight misses by the bytes fetched for each key:

```bash
./cache_policy_sim fixtures/multi_phase_trace.txt 3 \
  --key-bytes fixtures/multi_phase_key_bytes.csv \
  --phase-window 12 \
  --phase-warmup 3
```

The key-byte file accepts `key,bytes` or whitespace-separated rows, permits `#` comments, rejects duplicates, and must map every key in the trace. Capacity remains an entry count; the byte data estimates refill traffic rather than changing eviction capacity.

Run the deterministic three-regime fixture to verify that policy advice changes with the workload:

```bash
./cache_policy_sim fixtures/multi_phase_trace.txt 3 --phase-window 12
```

The fixture contains an LRU-favoring locality phase, a FIFO-favoring cyclic scan, and a hot set that fits entirely. For every window, the phase table shows both an isolated run that starts empty and a continuous run that carries live cache state forward. In phase two, the isolated result favors FIFO by one hit while the production-like continuous run favors LRU by one; retained state contributes two FIFO hits and four LRU hits. With the bundled byte mapping, LRU wins the full trace by two hits but FIFO transfers about 56 MiB less data because it avoids a second miss on the 64 MiB object. OPT stays visible as a hit-optimal offline ceiling; it is not presented as a byte-cost-optimal policy.

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
- `--phase-window N`: Optional phase analysis window size in accesses. Uses the largest requested capacity and reports isolated-reset and continuous-state results together.
- `--phase-warmup N`: Optional number of leading accesses in every continuous phase to classify as transition warm-up. Must be smaller than `--phase-window`.
- `--key-bytes path`: Optional key-to-byte mapping used to calculate total and per-key refill volume. Every trace key must be mapped.
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
- Optional phase table that compares isolated-reset results with continuous cache state
- Per-phase carry-over deltas showing warm-start gains or transition penalties for FIFO, LRU, and OPT
- Optional warm-up / steady-state decomposition for every continuous phase
- Optional byte-weighted miss volume, per-key transfer hotspots, and FIFO-vs-LRU byte recommendation
- FIFO-vs-LRU online choice and signed LRU hit delta for every capacity and phase in console, Markdown, and JSON output
- Isolated and continuous conclusion counts showing how often LRU leads, FIFO leads, or both tie
- Optional CSV export with one row per policy/capacity pair
- Optional Markdown brief with sweep, phase-local, and per-capacity policy details
- Optional JSON export with sweep metrics, final-cache state, and phase-local results

## Example workload

`sample_trace.txt` includes mixed locality to show where LRU usually outperforms FIFO.

## Example verification

```bash
zig c++ -std=c++17 -O2 -Wall -Wextra -pedantic cache_policy_sim.cpp -o cache_policy_sim
./cache_policy_sim --self-test
./cache_policy_sim fixtures/multi_phase_trace.txt 3 --phase-window 12 --phase-warmup 3 --key-bytes fixtures/multi_phase_key_bytes.csv --top-keys 6 --markdown-out reports/cache-sweep.md --json-out reports/cache-sweep.json
```

That run should pass the self-test and show the isolated LRU/FIFO/tie split. The continuous hit-count view changes phase two from FIFO to LRU, yielding two LRU phases and one tie, with aggregate carry-over gains of two FIFO hits and four LRU hits. The byte view recommends FIFO for phase two's steady state and for the trace overall, freezing the difference between request-count and transfer-cost optimization.

## Portfolio Demo Script

Use the same trace with at least two cache sizes when explaining the result:

1. Capacity 2 or 3 to show early eviction pressure.
2. Capacity 4 to show whether LRU actually converts locality into hit-rate lift.
3. Call out the winner summary only after reading the reuse profile so the claim stays tied to workload shape.

## Portfolio Positioning

- Project type: C++ command-line utility
- Verification path: zig c++ -std=c++17 -O2 -Wall -Wextra -pedantic cache_policy_sim.cpp -o cache_policy_sim && ./cache_policy_sim --self-test && ./cache_policy_sim fixtures/multi_phase_trace.txt 3 --phase-window 12 --phase-warmup 3 --key-bytes fixtures/multi_phase_key_bytes.csv

