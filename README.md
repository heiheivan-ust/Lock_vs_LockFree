# Lock-based vs lock-free synchronization in C++

A micro-benchmark comparing lock-based and lock-free synchronization on a shared counter, a stack
and a queue, written for the course CSIT6000U Enterprise Software Engineering.

| Structure | Lock-based | Lock-free |
|---|---|---|
| Counter | `std::mutex`, test-and-test-and-set spinlock | atomic fetch-and-add, CAS loop |
| Stack | `std::mutex` | Treiber stack |
| Queue | `std::mutex` | Michael–Scott queue |

The spinlock, the CAS counter and both lock-free structures also have a variant with randomized
exponential backoff.

## Contents
- [`benchmark/README.md`](benchmark/README.md): method, build and run instructions, output format.
- [`benchmark/RUNS.md`](benchmark/RUNS.md): log of every full run (files, binary, conditions,
  measured environment) and a summary of what the runs showed.
- `benchmark/bench.cpp`: the benchmark (single file, C++20; MSVC, GCC, Clang).
- `benchmark/run_all.py`: the driver that runs the full matrix and writes the results.
- `benchmark/results_*.csv`, `benchmark/sysinfo_*.txt`: raw results and machine/environment
  details of every run (runs 1–5, backoff runs 1–2, and the earlier `results_v1.csv`).

## Reproducing
Windows with Visual Studio 2022 (C++ workload) and Python 3.8+:
```
cd benchmark
build.bat
python run_all.py
```
See [`benchmark/README.md`](benchmark/README.md) for options (`--quick`, `--shuffle`, `--backoff`,
`--bench`, `--tag`).

The exact binaries used for the measurements are not in the repository. Runs 1–5 used a build
with SHA-256 `a667abd1a427454c…`, the backoff runs a later build with `c981e475b46ae5b5…` (full
hashes in the `sysinfo_*.txt` files).

## Test machine
AMD Ryzen 9 6900HX (8 cores / 16 logical), 31.2 GB RAM, Windows 11 Home 25H2, MSVC 19.37
(`/O2`), on AC power in "Best performance" mode.
