# Lock-based vs lock-free benchmark

| File | Purpose |
|---|---|
| `bench.cpp` | Single-file C++20 benchmark (MSVC, GCC, Clang); one run → one CSV line |
| `build.bat` | MSVC x64 **Release** build → `bench.exe` |
| `run_all.py` | **The driver**: runs the whole matrix and writes new dated `results_<date>_<time>.csv` and `sysinfo_<date>_<time>.txt` |
| `demo.py` | ~30 s live demo for the presentation |
| `results_v1.csv` | Earlier full run (before the pre-touched pools, M–S metric change and new sets) |
| `RUNS.md` | Log of every full run: files, dates, binary, conditions, what each run showed |
| `results_*.csv`, `sysinfo_*.txt` | Raw results and machine/environment details of every run (see `RUNS.md`) |

The binaries are not in the repository; build `bench.exe` with `build.bat`. The binary of runs 1–5
(`bench_runs1-3.exe` in `RUNS.md`, before the backoff variants were added) and the later one are
identified by their SHA-256 in the `sysinfo_*.txt` files.

## Requirements
- Windows with Visual Studio 2022 and the "Desktop development with C++" workload
  (`build.bat` locates it with `vswhere`; no Developer Prompt needed).
- Python 3.8+ (standard library only).
- Enough free RAM for the pre-touched node pools. At 500 ms the largest runs are the 1-thread
  stack/queue runs (~4.5 GB); `bench` refuses to start a run (exit code 4, clear message) unless
  free RAM is at least twice the run's budget, i.e. ~9 GB free for those runs.

## Build
```
build.bat
```
Flags: `/std:c++20 /O2 /DNDEBUG /EHsc /MD /W4` (optimised Release build).
`run_all.py` and `demo.py` build automatically if `bench.exe` is missing, and
`run_all.py` rebuilds if `bench.cpp` is newer than `bench.exe` (or with `--rebuild`).

## Full measurement (produces a dated results file)
Close other programs, plug in the charger, set Windows power mode to "Best performance", then:
```
python run_all.py            # 7 reps x 500 ms (default)
python run_all.py --quick    # 2 reps x 200 ms smoke test
python run_all.py --reps 10 --ms 1000 --rebuild
python run_all.py --shuffle  # new random configuration order in every rep
python run_all.py --backoff --shuffle   # adds the *_backoff variants (208 configs, ~17 min)
python run_all.py --bench other_build.exe --tag run4   # another binary, label in file names
```
Each invocation writes two new files named after its start time, e.g.
`results_2026-11-12_053000_run4.csv` and `sysinfo_2026-11-12_053000_run4.txt` (`_quick` is added
for `--quick`). Nothing is ever overwritten, so no copying is needed after a run. `--bench FILE`
runs that binary and never rebuilds; `sysinfo` records the binary's name, SHA-256 and file time.
`--shuffle` (driver only; `bench.exe` is unchanged) checks whether the fixed configuration order
affects the results. The seed is written to `sysinfo_*.txt`; `--shuffle --seed S` repeats the same
orders. Without `--shuffle` the order is fixed, as in `results_run1.csv` and `results_run2.csv`.
Configurations (all interleaved: for each rep → for each configuration, so slow drift affects
all variants equally):
- **Main set**: counter {mutex, spinlock, atomic_faa, atomic_cas}, stack {mutex, lockfree},
  queue {mutex, lockfree} at thread counts 1, 2, 4, … below L (L = logical cores), L, 2L, 4L
  (capped at 128); work = 0.
- **Contention sets** at L threads, work ∈ {0, 10, 100, 1000, 10000, 100000}:
  counter {mutex, spinlock, atomic_faa} → `contention`; stack {mutex, lockfree} →
  `contention_stack`; queue {mutex, lockfree} → `contention_queue`.
- **Latency set** (`--latency`): all 8 variants at 1, 8, 16 and 64 threads, work = 0 →
  `latency_counter`, `latency_stack`, `latency_queue`. These are separate runs; the main set
  carries no sampling overhead.
- **Backoff** (`--backoff` only): `spinlock_backoff`, `atomic_cas_backoff` (counter) and
  `lockfree_backoff` (stack, queue) are added to the main and latency sets, and
  `spinlock_backoff` / `lockfree_backoff` to the three contention sets, next to the same
  algorithms without backoff (146 → 208 configurations).
- **Allocator/layout controls**, stack and queue {mutex, lockfree}, work = 0:
  `--heap` at 1, 4, 16 threads → `stack_heap`, `queue_heap`;
  `--pad64` at 16 threads → `stack_pad64`, `queue_pad64`.

Results file header (the v1 columns keep their positions; latency columns are appended after
`rep` and are empty except in the `latency_*` rows):
```
benchmark,variant,threads,millis,work,total_ops,ops_per_sec,min_thread_ops,max_thread_ops,cv,check,retries_per_op,rep,lat_p50_ns,lat_p99_ns,lat_p999_ns,lat_max_ns,lat_samples,bg_cpu_pct,avail_ram_gb,bench_cpu_pct
```
The last two columns (from 2026-11-10; runs 1–3 do not have them) are measured by `run_all.py`,
not by `bench`, and only on Windows:
- `bg_cpu_pct`: CPU time used by everything except that `bench` process while it ran, in % of
  all logical CPUs: (total busy time from `GetSystemTimes` − the process's own time from
  `GetProcessTimes`) / (wall time × logical CPUs). It includes other programs **and** work the OS
  does on bench's behalf outside its process (e.g. interrupts), so it is an upper bound on other
  programs' load. In run 5 it was about 2 points above the idle sample and not higher in the
  runs with the largest node pools.
- `avail_ram_gb`: available physical RAM just before the run.
- `bench_cpu_pct` (from run 5): CPU time of the bench process itself over its whole lifetime
  (setup, timed run, verification) in % of all logical CPUs. About 6% per busy thread up to 16
  threads; below 100% at 16+ threads because setup and verification are mostly single-threaded.

Before the first run, `run_all.py` also measures the idle load for 10 s (same `GetSystemTimes`
method; it matches Windows' "% Processor Time" counter, while Task Manager shows "% Processor
Utility", which scales with clock speed and reads a few points higher) and writes it to
`sysinfo_*.txt` (`idle before run:`). At the end it appends the median/p90/max of `bg_cpu_pct` and
the minimum/median of `avail_ram_gb` (`env:` lines).
The results file is written line by line (a partial file survives an interruption). Every run
whose `check` is not `ok` is listed at the end.
`sysinfo_*.txt` records the machine, power state, compiler, the binary (name, SHA-256, file
time), the idle load, the invariant-TSC check, TSC
frequency/granularity/timer overhead, the latency sampling interval, the node budget and the
memory layout of every structure.

## Run a single benchmark
```
bench <benchmark> <variant> <threads> <millis> [work] [--latency] [--heap | --pad64]
bench --layout          # sizes/offsets of all structures
bench --tsc             # invariant-TSC bit, TSC frequency, granularity, timer overhead

bench counter mutex 16 500             # counter: mutex | spinlock | atomic_faa | atomic_cas
bench counter spinlock 16 500 100      # work = 100 iterations of non-critical work per op
bench stack lockfree 8 500             # stack: mutex | lockfree (Treiber)
bench queue lockfree 8 500 0 --latency # queue: mutex | lockfree (Michael-Scott)
bench stack mutex 16 500 0 --heap      # nodes from `new` inside the timed loop
bench queue lockfree 16 500 0 --pad64  # one node per 64-byte cache line
bench stack lockfree_backoff 16 500    # Treiber with randomized exponential backoff
bench counter spinlock_backoff 16 500 0 --backoff-max 1024   # other backoff cap
```
Backoff variants: counter `spinlock_backoff`, `atomic_cas_backoff`; stack and queue
`lockfree_backoff`. `--backoff-max N` (default 256) is accepted only with these.
`work` (default 0) is an empty loop with a compiler barrier, run after each counter increment,
each push+pop pair (stack) or each enqueue+dequeue pair (queue).

Output: one CSV line, no header, 17 fields:
`benchmark,variant,threads,millis,work,total_ops,ops_per_sec,min_thread_ops,max_thread_ops,cv,check,retries_per_op,lat_p50_ns,lat_p99_ns,lat_p999_ns,lat_max_ns,lat_samples`
- `cv`: population std-dev / mean of per-thread op counts (fairness; 0 for 1 thread).
- `check`: `ok`, `MISMATCH` (correctness check failed, exit 1) or `OVERFLOW` (a thread used up
  its pre-allocated budget and stopped early, exit 3; the throughput of such a run is invalid).
  Exit code 4 (no CSV line) means there was not enough free RAM to start the run.
- `retries_per_op` = failed attempts / total ops (a stack/queue pair counts as 2 ops):
  - `atomic_cas`, Treiber: one per failed CAS (= one extra loop iteration);
  - spinlock: one per failed lock exchange;
  - Michael–Scott: one per loop iteration beyond the first in each enqueue/dequeue call,
    whatever caused it (failed link/head CAS, a re-read that found tail/head moved, or a helping
    iteration). The tail-swing CAS after a successful link ends the call and is not counted.
    (In `results_v1.csv` the M–S metric instead counted failed CAS only, including failed
    tail-swing and helping CAS, so v1 and current M–S values are not directly comparable.)
  - `mutex`, `atomic_faa`: always 0.
- `lat_*`: only with `--latency` (see below); empty otherwise.

## Method notes (relevant for the report)
- **No allocation or page faults while timing (stack/queue).** Before the start barrier each
  thread allocates a node pool of ceil(K × seconds / threads) + 1M nodes (M = 2^20), with
  K = 384M nodes/s for 1 thread, 128M for 2 threads and 32M for 4+ threads (above the highest
  node rates measured), and writes one byte to every 4 KB page, so all pages are faulted in.
  Before allocating anything, `bench` checks that free RAM is at least twice the run's total
  budget (pool + retired array + latency buffer) and aborts otherwise. Its `retired` array has the same size, is
  pre-touched, and is written by index. During the run a node is a thread-local pointer bump;
  locked and lock-free variants use the same `Node` and the same pool. If a thread exhausts its
  budget it stops and the run reports `OVERFLOW` instead of allocating.
- **Controls:** `--heap` restores `new` per node inside the timed loop (the very first version,
  before any arena; `results_v1.csv` used an in-loop bump allocator without pre-touching) and
  `--pad64` places one node per 64-byte cache line (pool stride 64 instead of 16).
- **Backoff (`*_backoff` variants).** After every failed attempt the thread waits a random
  number of `pause` instructions, uniform in [1, limit]; `limit` starts at 1, doubles after each
  failure up to 256 (`--backoff-max`) and restarts at 1 for every operation (randomized truncated
  exponential backoff, per-thread xorshift32). One `pause` takes about 14 ns on the Ryzen 9 6900HX
  (14.0–14.1 ns in the backoff runs' `sysinfo`), so a single wait is at most ~3.6 µs. Where it applies: spinlock after a failed
  lock exchange (then it watches the lock again); CAS counter and Treiber after every failed CAS,
  followed by a fresh read of the value/head; Michael–Scott after a failed link CAS (enqueue) or
  head CAS (dequeue) only, not after re-reads or helping iterations. Mutex and fetch-and-add have
  no failed attempts and no backoff variant. The wait is an out-of-line function, so the first
  attempt of every operation runs the same code as without backoff; the remaining 1-thread
  overhead (no failures at all) was measured at 0–3%. `retries_per_op` is counted exactly as
  without backoff. The parameters were chosen once, not tuned; other caps can be tried with
  `--backoff-max`.
- **No reclamation during the run.** Removed nodes are only recorded; pools (or heap nodes) are
  freed after timing and verification. No address is handed out twice during a run, so ABA
  cannot occur. This is a deliberate simplification: real lock-free code needs hazard pointers,
  epochs or tagged pointers.
- **Timing** starts when all threads have reached the barrier (after their setup) and the start
  flag is set, and ends after all threads are joined, before any verification.
- **Latency (`--latency`)**: every 64th loop iteration of each thread is timed with the CPU
  timestamp counter (`lfence; rdtsc; lfence` … `rdtscp; lfence`). Counter: the increment is one
  sample; stack/queue: the push and the pop (enqueue and dequeue) are two separate samples, so
  1 in 64 operations is timed everywhere. Samples go to a pre-sized, pre-touched per-thread
  buffer (ceil(16M / threads) + 1M samples). Ticks are converted to ns per run by calibrating TSC
  ticks against `steady_clock` over the timed interval. Percentiles (nearest rank) are over all
  samples of all threads combined, with push and pop samples together. The CPU reports an
  invariant TSC (CPUID 0x80000007 EDX bit 8); see `sysinfo_*.txt`. Each sample includes the timer's
  own overhead (median ~20 ns on this machine, see `bench --tsc`); it is not subtracted.
- **SMT:** on the Ryzen 9 6900HX (8 cores / 16 logical), runs with 9–16 threads place two
  threads on one physical core; runs with more than 16 threads are oversubscribed.
- **Correctness:** the counter must equal the sum of per-thread ops; for stack/queue every pushed
  node must appear exactly once across retired + drained nodes (count, then sorted pointers
  without duplicates), and the sum of inserted payloads must equal the sum of removed payloads.
- **Retry metric caveats:** `retries_per_op` counts retries, not time spent; a spinlock thread
  that waits in the relaxed-load loop and then wins its exchange records 0 failures.
  `compare_exchange_weak` can fail spuriously on LL/SC CPUs (ARM), not on x86.
- **ThreadSanitizer** (not available for MSVC) is run from WSL Ubuntu on the lock-free variants,
  the spinlock and the mutex stack/queue, with and without `--latency`, `--heap`, `--pad64`:
  ```
  g++ -std=c++20 -O1 -g -fsanitize=thread -pthread bench.cpp -o bench_tsan
  setarch $(uname -m) -R ./bench_tsan queue lockfree 4 300 0 --latency
  ```
  `setarch -R` disables ASLR for that process; without it, TSan in GCC 13 aborts with
  "unexpected memory mapping" on the WSL 6.6 kernel.

## Demo
```
python demo.py               # 1000 ms per run, ~30 s total
```
