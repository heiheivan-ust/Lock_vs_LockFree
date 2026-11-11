# Run log

Every full run of `run_all.py`, with the conditions that `sysinfo_*.txt` does not record.
Times are local. Hashes are the first 16 hex digits of SHA-256.
In this repository the result files of runs 4–5 and backoff1–2 are named by label (e.g.
`results_run4.csv`); the name `run_all.py` originally gave each file is in the `results file:`
line of its `sysinfo_*.txt`.

## Common to runs 1–3
- Machine: AMD Ryzen 9 6900HX (8 cores / 16 logical, SMT), 31.2 GB RAM.
- OS: Windows 11 Home 25H2, build 26200.9711.
- Power: plugged in (AC), battery 100%, power mode "Best performance".
- Compiler: MSVC 19.37.32824 x64, `/std:c++20 /O2 /DNDEBUG /EHsc /MD /W4`; Python 3.13.7.
- **Same binary**: `bench.exe` a667abd1a427454c, built 2026-10-19 03:18 from `bench.cpp`
  73ec185241df10c4. Not rebuilt for any later run.
- Matrix: 146 configurations × 7 reps × 500 ms = 1022 runs (see `sysinfo_*.txt` for sets,
  thread counts, TSC and memory layout).

## Runs

| Run | Files | Date and time | Order | Checks | Use |
|---|---|---|---|---|---|
| v1 | `results_v1.csv` (no sysinfo kept) | 2026-10-19, finished 01:33 | fixed | 686 runs | Earlier build and method; not comparable (see README) |
| 1 | `results_run1.csv`, `sysinfo_run1.txt` | 2026-10-19 03:35–03:47 | fixed | 1022/1022 ok | Main data |
| 2 | `results_run2.csv`, `sysinfo_run2.txt` | 2026-11-10 05:17–05:29 | fixed | 1022/1022 ok | Same-build replication |
| 3 | `results_run3_shuffled.csv`, `sysinfo_run3_shuffled.txt` | 2026-11-10 05:42–05:54 | shuffled every rep, seed 4036977441 | 1022/1022 ok | Run-order check |
| 4 | `results_run4.csv`, `sysinfo_run4.txt` | 2026-11-10 07:01–07:13 | fixed | 1022/1022 ok | Replication with measured environment |
| 5 | `results_run5.csv`, `sysinfo_run5.txt` | 2026-11-10 07:32–07:44 | fixed | 1022/1022 ok | Replication with measured environment (+ bench CPU) |
| backoff1 | `results_backoff1.csv`, `sysinfo_backoff1.txt` | 2026-11-10 08:02–08:20 | shuffled every rep, seed 2450872945 | 1456/1456 ok | Backoff vs no backoff (new binary c981e475b46ae5b5) |
| backoff2 | `results_backoff2.csv`, `sysinfo_backoff2.txt` | 2026-11-10 08:30–08:48 | shuffled every rep, seed 2490286898 | 1456/1456 ok | Replication of backoff1 |

Runs 1–3 were written as `results.csv` / `sysinfo.txt` and copied to the names above. From
2026-11-10 `run_all.py` writes new dated files itself (`results_<date>_<time>[_<tag>].csv`,
`sysinfo_<date>_<time>[_<tag>].txt`) and never overwrites anything, so no copying is needed.

Result file hashes: v1 5131100acdda1781, run 1 f482b18903bfac2e, run 2 24720e877f518de2,
run 3 2c2b9ce43e89bdf3.

## Conditions per run (not in sysinfo)
- **Run 1**: started by hand from `benchmark/` with other programs closed and the charger in.
  Background load was not measured.
- **Run 2**: started by hand in a standalone PowerShell window after a restart, following the
  full checklist (VS Code and other apps closed, startup apps disabled, AdobeCollabSync/Steam
  ended, OneDrive paused, RAM check passed). Before starting, Task Manager showed idle CPU of at
  most about 10% and memory in use about 20% (my observation; not logged during the run).
  Windows started at 05:07 (System event log), so the run began 10 minutes after the restart.
- **Run 3**: same procedure and conditions as run 2 (idle CPU at most about 10%, memory about
  20%), `python run_all.py --shuffle` (driver change only; the `--shuffle` option was added to
  `run_all.py` on 2026-11-10 before this run). The computer was restarted again between runs 2
  and 3: Windows started at 05:39, so this run began only 3 minutes after the restart.

- **Run 4**: `python run_all.py --bench bench_runs1-3.exe --tag run4` (binary a667abd1a427454c,
  the same as runs 1–3), started by hand in a standalone PowerShell window after a restart
  (Windows started 06:56, run began 07:01), following the checklist. Measured by the driver:
  idle load before the start 8.3% of all logical CPUs (10 s), RAM 21.5 of 31.2 GB available
  (31% in use); background CPU during the 1022 runs median 9.8%, p90 13.4%, max 49.6% (upper
  bound, includes OS work for bench); available RAM before each run min 21.1 GB.
  Throughput vs mean of runs 1–3: median difference 0.5% (90th percentile 2.2%), 1 of 146
  configurations above 10%. I watched Task Manager during this run and saw total CPU of only
  about 7–17%; the matching throughput shows the threads ran normally, and a direct
  check afterwards showed 94–100% "% Processor Time" while a 16-thread run executes. The cause
  of the low Task Manager reading is unknown. From run 5 the driver also records bench's own
  CPU per run (`bench_cpu_pct`), which run 4 does not have.
- **Run 5**: `python run_all.py --bench bench_runs1-3.exe --tag run5` (same binary), after a
  restart (Windows started 07:27, run began 07:32), following the checklist with Task Manager
  closed. Measured: idle load 7.6% (10 s), RAM 21.5 of 31.2 GB available (31% in use);
  background CPU during runs median 9.8%, p90 12.8%, max 32.6%; available RAM min 21.0 GB.
  Bench's own CPU per run (whole process lifetime, % of all 16 logical CPUs), median by thread
  count: 1 → 7%, 2 → 12%, 4 → 20%, 8 → 44%, 16 → 78%, 32 → 84%, 64 → 81%.
  Background CPU was not higher in the runs with the largest node pools (1-thread stack/queue:
  median 9.1% vs 9.8% for all others), so OS work for bench's allocations does not noticeably
  inflate `bg_cpu_pct`.

- **backoff1**: `python run_all.py --backoff --shuffle --tag backoff1` with the new `bench.exe`
  (c981e475b46ae5b5), after a restart (Windows started 07:56, run began 08:02), following the
  checklist. Measured: idle load 7.8%, RAM 33% in use; background CPU median 9.7%, p90 11.9%;
  available RAM min 20.8 GB; bench CPU by threads as in run 5. The variants without backoff
  differed from the five-run medians by a median of 0.55%.
- **backoff2**: same command with `--tag backoff2`, after a restart (Windows started 08:25, run
  began 08:30). Measured: idle load 7.7%, RAM 31% in use; background CPU median 9.8%, p90
  12.0%; available RAM min 21.0 GB. Variants without backoff: median 0.64% from the five-run
  medians. All 208 configurations: median 0.62% from backoff1.

## Backoff results (backoff1 and backoff2)
- The two runs agree: backoff variants median difference 0.9% (90th percentile 7.1%), variants
  without backoff 0.5%. The only large differences are the spinlock with backoff at 32–64
  threads (oversubscribed; 13–37%).
- Throughput with/without backoff at 16 threads (both runs): spinlock 3.44×, Treiber 2.30×,
  M–S 1.46×, CAS counter 0.77× (slower; also at 32 and 64 threads). Against the mutex at 16
  threads: spinlock_backoff 1.65–1.69×, Treiber with backoff 2.30–2.34×, M–S with backoff
  1.44–1.45×, CAS with backoff 2.24×. At 64 threads the spinlock with backoff falls below the
  mutex (0.39–0.52×); the others keep their lead.
- Retries per op at 16 threads drop from 5.6 to 1.25 (spinlock), 9.6 to 1.6 (Treiber), 6.6 to
  4.3 (M–S), but only 2.85 to 2.45 (CAS). Fairness: Treiber improves (CV 0.37 → 0.11–0.15),
  spinlock and CAS get worse (0.20 → 0.50–0.54, 0.14–0.17 → 0.30–0.31).
- Contention sweep (16 threads): Treiber with backoff peaks at 19.2–19.8 M ops/s at work
  100–1000 (about 4× the mutex); M–S with backoff 10.0–10.7. The spinlock with backoff has a
  reproducible dip at work 100 (6.9–7.0 M ops/s vs 11.4 at work 10 and 16.7–16.8 at work 1000,
  tight across all 14 reps); the cause was not investigated.
- Latency at 64 threads: backoff lowers p50 (Treiber 1.45 → 0.25 µs, CAS 0.68 → 0.15 µs) but
  raises p99/p99.9 (Treiber p99.9 39–40 → 71–72 µs, M–S 33–34 → 52 µs, CAS 6 → 29–30 µs). Still
  far below the mutex's p99.9 (0.5–1.5 ms in these runs).
- Side effect, not caused by backoff: at 1 thread (no failures, 0 retries) the spinlock_backoff
  build runs 16–17% faster than the spinlock (374–376 vs 320–322 M ops/s), a code-generation
  difference between the two compiled variants.

## Binary change on 2026-11-10 (after run 3)
The backoff variants were added to `bench.cpp` and `bench.exe` was rebuilt (now c981e475b46ae5b5).
The binary of runs 1–3 is kept as `bench_runs1-3.exe` (a667abd1a427454c). Checks before any full
run with the new binary: all 12 variants `ok` at 1, 2 and 16 threads with and without
`--latency`; backoff stack/queue `ok` (no OVERFLOW) at 1–64 threads at 500 ms; TSan (GCC 13, WSL)
clean for all backoff and lock-free variants incl. `--latency`, `--heap`, `--pad64`,
`--backoff-max 4`; GCC 13 / Clang 18 `-Wall -Wextra -pedantic` and MSVC `/W4` without warnings;
old vs new binary on the variants without backoff within ~1% (interleaved, 5 × 300 ms);
`run_all.py --backoff --quick` 416/416 ok. (Test outputs were deleted.)

## Environment recording (from 2026-11-10, driver only)
`run_all.py` now records a 10 s idle-load sample before the run (`sysinfo_*.txt`, `idle before
run:`), background CPU and available RAM for every run (results columns `bg_cpu_pct`,
`avail_ram_gb`) and a summary at the end (`env:` lines), plus the binary's file name, SHA-256 and
file time (`binary:`). `--bench FILE` runs another binary without rebuilding. Checked against
Windows' "% Processor Time" counter (15.3% vs 15.2% over the same 10 s) and against a known load
of 2 busy cores; tested with `--bench bench_runs1-3.exe` (146/146 ok, hash recorded correctly).
Runs 1–3 have no such data; for them only my Task Manager observations above exist.

## Planned
- Nothing. Main study (runs 1–5) and backoff extension (backoff1–2) are complete.

## What the replications showed
All five complete runs (1–5, same binary, same matrix):
- Median throughput difference between any two runs 0.6–1.1% (90th percentile 2.7–4.0%), at
  most 3 of 146 configurations above 10% (mostly the oversubscribed spinlock). Per
  configuration, max/min across the five runs: median 2.2%, 90th percentile 6.5%.
- Ratios across runs 1–5: FAA/mutex at 16 threads 12.4–12.6×; CAS/mutex 2.85–2.95×;
  lock-free/mutex stack 1.01–1.04 (work 0), 1.31–1.37 (100), 1.62–1.71 (1000); queue
  0.98–1.00 (0), 1.56–1.59 (100), 1.79–1.84 (1000).
- FAA median CV at 16 threads: 0.54 / 0.55 / 0.44 / 0.58 / 0.48.
- 64-thread p99.9: mutex counter 0.48–1.86 ms, stack 1.01–2.35 ms, queue 1.56–2.98 ms
  (falling over runs 1–3, then stable); lock-free stack 42–45 µs, queue 32–35 µs; p99 within
  ~10% everywhere. Lock-free p99.9 is 23–86× lower than the mutex's.

Earlier notes (runs 1–3):
- Run 2 vs run 1: median throughput difference 0.9% (90th percentile 4.0%) over 146
  configurations; 2 above 10% (oversubscribed spinlock with latency sampling, CAS at 4 threads).
- Run 3 (shuffled) vs mean of runs 1–2: median 0.7% (90th percentile 2.5%); lock-free/mutex
  ratios within 0.03 in 38 of 41 configurations; no effect of the preceding run's type.
- fetch-and-add fairness at 16 threads, median CV: 0.54 / 0.55 / 0.44 (v1: 0.09). Individual
  reps range 0.06–0.74.
- Mutex p99.9 at 64 threads fell from run to run (counter 1.86 / 0.92 / 0.57 ms, queue
  2.98 / 2.15 / 1.56 ms, stack 2.35 / 1.49 / 1.01 ms) while p99 stayed within 10% and lock-free
  p99.9 stayed at 32–45 µs.
