#!/usr/bin/env python3
"""Run the full lock vs lock-free benchmark matrix (Python standard library only).

This is the driver that produces the results. Every invocation writes two NEW files next
to this script, named after its start time (nothing is ever overwritten):
  sysinfo_<YYYY-MM-DD>_<HHMMSS>[_quick][_<tag>].txt
               - machine, power, compiler, binary, idle load, TSC check, memory layout,
                 run parameters, and an environment summary at the end
  results_<YYYY-MM-DD>_<HHMMSS>[_quick][_<tag>].csv
               - one line per run, header:
                 benchmark,variant,threads,millis,work,total_ops,ops_per_sec,
                 min_thread_ops,max_thread_ops,cv,check,retries_per_op,rep,
                 lat_p50_ns,lat_p99_ns,lat_p999_ns,lat_max_ns,lat_samples,
                 bg_cpu_pct,avail_ram_gb,bench_cpu_pct
                 (lat_* are empty except in the latency_* rows; bg_cpu_pct = CPU used
                 by everything other than bench during that run, in % of all logical
                 CPUs; avail_ram_gb = available RAM just before it; bench_cpu_pct = CPU
                 used by bench itself over its whole lifetime (setup, timed run,
                 verification), in % of all logical CPUs; all three Windows only)

Usage:
  python run_all.py              7 reps x 500 ms (default)
  python run_all.py --quick      2 reps x 200 ms (smoke test)
  python run_all.py --reps N --ms M
  python run_all.py --rebuild    force a fresh MSVC Release build first
  python run_all.py --shuffle    new random configuration order in every rep
                                 (seed recorded in sysinfo.txt; --seed S repeats an order)
  python run_all.py --backoff    also run the *_backoff variants in the main, contention
                                 and latency sets (next to the variants without backoff)
  python run_all.py --bench bench_runs1-3.exe
                                 run another binary (no rebuild; its hash goes into sysinfo)
  python run_all.py --tag run4   add a label to the output file names
"""

import argparse
import ctypes
import datetime
import hashlib
import os
import platform
import random
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
IS_WINDOWS = os.name == "nt"
BENCH = os.path.join(HERE, "bench.exe" if IS_WINDOWS else "bench")
BUILD_BAT = os.path.join(HERE, "build.bat")
RESULTS = None  # results_<start time>[...].csv, set in main()
SYSINFO = None  # sysinfo_<start time>[...].txt, set in main()

# v1 columns keep their positions; the latency columns are appended after rep,
# the environment columns (measured by this driver, not by bench) after those.
HEADER = ("benchmark,variant,threads,millis,work,total_ops,ops_per_sec,"
          "min_thread_ops,max_thread_ops,cv,check,retries_per_op,rep,"
          "lat_p50_ns,lat_p99_ns,lat_p999_ns,lat_max_ns,lat_samples,"
          "bg_cpu_pct,avail_ram_gb,bench_cpu_pct")
IDLE_SAMPLE_S = 10  # idle-load sample before the first run
BENCH_FIELDS = 17  # fields printed by bench (rep is inserted after the first 12)
BASE_FIELDS = 12

MAIN_SET = [
    ("counter", "mutex"), ("counter", "spinlock"),
    ("counter", "atomic_faa"), ("counter", "atomic_cas"),
    ("stack", "mutex"), ("stack", "lockfree"),
    ("queue", "mutex"), ("queue", "lockfree"),
]
# Contention sets, all at L threads: (CSV benchmark label, bench benchmark, variants)
CONTENTION_SETS = [
    ("contention", "counter", ["mutex", "spinlock", "atomic_faa"]),
    ("contention_stack", "stack", ["mutex", "lockfree"]),
    ("contention_queue", "queue", ["mutex", "lockfree"]),
]
CONTENTION_WORK = [0, 10, 100, 1000, 10000, 100000]

# --backoff: randomized exponential backoff variants, added to the main,
# contention and latency sets (not to the allocator/layout controls).
BACKOFF_MAIN = [
    ("counter", "spinlock_backoff"), ("counter", "atomic_cas_backoff"),
    ("stack", "lockfree_backoff"), ("queue", "lockfree_backoff"),
]
BACKOFF_CONTENTION = {
    "contention": ["spinlock_backoff"],
    "contention_stack": ["lockfree_backoff"],
    "contention_queue": ["lockfree_backoff"],
}

# Latency set: all 8 variants, work = 0, run with --latency (separate runs, so
# the main-set throughput runs carry no sampling overhead).
LATENCY_THREADS = [1, 8, 16, 64]
# Allocator/layout controls for stack and queue, work = 0.
NODE_SET = [("stack", "mutex"), ("stack", "lockfree"), ("queue", "mutex"), ("queue", "lockfree")]
HEAP_THREADS = [1, 4, 16]   # --heap  -> stack_heap / queue_heap
PAD64_THREADS = [16]        # --pad64 -> stack_pad64 / queue_pad64


# ---------------------------------------------------------------------------
# System information
# ---------------------------------------------------------------------------

def cpu_name():
    if IS_WINDOWS:
        try:
            import winreg
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                                r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as k:
                return winreg.QueryValueEx(k, "ProcessorNameString")[0].strip()
        except OSError:
            pass
    return platform.processor() or "unknown"


def physical_cores():
    if IS_WINDOWS:
        try:
            out = subprocess.run(
                ["powershell", "-NoProfile", "-Command",
                 "(Get-CimInstance Win32_Processor | Measure-Object NumberOfCores -Sum).Sum"],
                capture_output=True, text=True, timeout=60).stdout.strip()
            return int(out)
        except (OSError, ValueError, subprocess.SubprocessError):
            return None
    return None


def _mem_status():
    if IS_WINDOWS:
        class MEMORYSTATUSEX(ctypes.Structure):
            _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong),
                        ("ullTotalPhys", ctypes.c_ulonglong), ("ullAvailPhys", ctypes.c_ulonglong),
                        ("ullTotalPageFile", ctypes.c_ulonglong), ("ullAvailPageFile", ctypes.c_ulonglong),
                        ("ullTotalVirtual", ctypes.c_ulonglong), ("ullAvailVirtual", ctypes.c_ulonglong),
                        ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]
        m = MEMORYSTATUSEX()
        m.dwLength = ctypes.sizeof(m)
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m)):
            return m
    return None


def ram_gb():
    m = _mem_status()
    return m.ullTotalPhys / 2**30 if m else None


def avail_ram_gb():
    """Available physical RAM in GB (what Task Manager shows as "Available")."""
    m = _mem_status()
    return m.ullAvailPhys / 2**30 if m else None


# ---------------------------------------------------------------------------
# Environment monitoring (Windows): background CPU load during each run.
# Total busy CPU time of the machine (GetSystemTimes) over the lifetime of one
# bench process, minus that process's own CPU time (GetProcessTimes), divided
# by wall time x logical CPUs = the share of all logical CPUs used by anything
# other than bench while it ran. Read only before and after each run, so it
# adds nothing to bench's timed region.
# ---------------------------------------------------------------------------

def system_cpu_times():
    """(idle, busy) CPU time of the whole machine in 100 ns units, or None."""
    if not IS_WINDOWS:
        return None
    idle, kernel, user = ctypes.c_ulonglong(), ctypes.c_ulonglong(), ctypes.c_ulonglong()
    if not ctypes.windll.kernel32.GetSystemTimes(ctypes.byref(idle), ctypes.byref(kernel),
                                                  ctypes.byref(user)):
        return None
    # kernel time includes idle time
    return idle.value, kernel.value - idle.value + user.value


def process_cpu_time(handle):
    """CPU time (kernel + user) of a process in 100 ns units, or None."""
    if not IS_WINDOWS:
        return None
    c, e, k, u = (ctypes.c_ulonglong() for _ in range(4))
    f = ctypes.windll.kernel32.GetProcessTimes
    f.argtypes = [ctypes.c_void_p] + [ctypes.c_void_p] * 4
    if not f(int(handle), ctypes.byref(c), ctypes.byref(e), ctypes.byref(k), ctypes.byref(u)):
        return None
    return k.value + u.value


def idle_load(seconds):
    """Busy share (%) of all logical CPUs over `seconds` while nothing is run, or None."""
    a = system_cpu_times()
    time.sleep(seconds)
    b = system_cpu_times()
    if a is None or b is None:
        return None
    idle, busy = b[0] - a[0], b[1] - a[1]
    return 100.0 * busy / (idle + busy) if idle + busy > 0 else None


def os_version():
    s = f"{platform.system()} {platform.release()} (version {platform.version()})"
    if IS_WINDOWS:
        try:
            import winreg
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                                r"SOFTWARE\Microsoft\Windows NT\CurrentVersion") as k:
                def q(name):
                    try:
                        return winreg.QueryValueEx(k, name)[0]
                    except OSError:
                        return None
                build = q("CurrentBuild")
                # ProductName still says "Windows 10" on Windows 11; fix by build number.
                product = q("ProductName") or "Windows"
                if build and int(build) >= 22000:
                    product = product.replace("Windows 10", "Windows 11")
                s = f"{product} {q('DisplayVersion') or ''} (build {build}.{q('UBR')})"
        except (OSError, ValueError):
            pass
    return s


def power_state():
    """Returns (on_battery: bool|None, description)."""
    if not IS_WINDOWS:
        return None, "unknown"

    class SYSTEM_POWER_STATUS(ctypes.Structure):
        _fields_ = [("ACLineStatus", ctypes.c_ubyte), ("BatteryFlag", ctypes.c_ubyte),
                    ("BatteryLifePercent", ctypes.c_ubyte), ("SystemStatusFlag", ctypes.c_ubyte),
                    ("BatteryLifeTime", ctypes.c_ulong), ("BatteryFullLifeTime", ctypes.c_ulong)]
    s = SYSTEM_POWER_STATUS()
    if not ctypes.windll.kernel32.GetSystemPowerStatus(ctypes.byref(s)):
        return None, "unknown"
    pct = "" if s.BatteryLifePercent == 255 else f", battery {s.BatteryLifePercent}%"
    if s.ACLineStatus == 1:
        return False, f"plugged in (AC){pct}"
    if s.ACLineStatus == 0:
        return True, f"ON BATTERY{pct}"
    return None, f"unknown{pct}"


class GUID(ctypes.Structure):
    _fields_ = [("Data1", ctypes.c_ulong), ("Data2", ctypes.c_ushort),
                ("Data3", ctypes.c_ushort), ("Data4", ctypes.c_ubyte * 8)]

    def __str__(self):
        d4 = bytes(self.Data4)
        return (f"{self.Data1:08x}-{self.Data2:04x}-{self.Data3:04x}-"
                f"{d4[:2].hex()}-{d4[2:].hex()}")


POWER_MODES = {  # Windows 10/11 "power mode" slider overlays
    "00000000-0000-0000-0000-000000000000": "Balanced",
    "ded574b5-45a0-4f42-8737-46345c09c238": "Best performance",
    "961cc777-2547-4f9d-8174-7d86181b8a7a": "Better battery / best power efficiency",
    "3af9b8d9-7c97-431d-ad78-34a8bfea439f": "Better battery / best power efficiency",
}


def power_plan():
    if not IS_WINDOWS:
        return "unknown"
    parts = []
    try:
        pp = ctypes.windll.powrprof
        pguid = ctypes.POINTER(GUID)()
        if pp.PowerGetActiveScheme(None, ctypes.byref(pguid)) == 0:
            size = ctypes.c_ulong(0)
            pp.PowerReadFriendlyName(None, pguid, None, None, None, ctypes.byref(size))
            buf = ctypes.create_string_buffer(size.value)
            name = "?"
            if pp.PowerReadFriendlyName(None, pguid, None, None, buf, ctypes.byref(size)) == 0:
                name = buf.raw.decode("utf-16-le").rstrip("\x00")
            parts.append(f"plan '{name}' {{{pguid.contents}}}")
            ctypes.windll.kernel32.LocalFree(pguid)
        overlay = GUID()
        if pp.PowerGetEffectiveOverlayScheme(ctypes.byref(overlay)) == 0:
            g = str(overlay)
            parts.append(f"power mode '{POWER_MODES.get(g, g)}'")
    except (OSError, AttributeError) as e:
        parts.append(f"(could not query: {e})")
    return "; ".join(parts) or "unknown"


def compiler_info():
    if not IS_WINDOWS:
        return "unknown"
    try:
        r = subprocess.run(["cmd", "/c", BUILD_BAT, "version"], capture_output=True,
                           text=True, errors="replace", timeout=180)
    except (OSError, subprocess.SubprocessError) as e:
        return f"unknown ({e})"
    lines = [ln.strip() for ln in r.stdout.splitlines() if ln.strip()]
    lines = [ln[2:] if ln.startswith("1:") else ln for ln in lines]
    return "; ".join(lines) if lines else f"unknown ({r.stderr.strip()})"


def bench_info(flag):
    """Output lines of `bench --tsc` / `bench --layout` (measured by bench itself)."""
    try:
        r = subprocess.run([BENCH, flag], capture_output=True, text=True, errors="replace",
                           timeout=120)
    except (OSError, subprocess.SubprocessError) as e:
        return [f"unavailable ({e})"]
    if r.returncode != 0:
        return [f"unavailable (exit {r.returncode}: {r.stderr.strip()})"]
    return [ln.rstrip() for ln in r.stdout.splitlines() if ln.strip()]


# ---------------------------------------------------------------------------
# Build and run
# ---------------------------------------------------------------------------

def build():
    print("Building bench.exe (MSVC Release) ...", flush=True)
    r = subprocess.run(["cmd", "/c", BUILD_BAT], capture_output=True, text=True, errors="replace")
    if r.returncode != 0 or not os.path.exists(BENCH):
        print(r.stdout, r.stderr, sep="\n", file=sys.stderr)
        sys.exit("ERROR: build failed (output above).")


def thread_counts(L):
    ts = []
    p = 1
    while p < L:
        ts.append(p)
        p *= 2
    ts += [L, 2 * L, 4 * L]
    return sorted({min(t, 128) for t in ts})


def fmt_dur(s):
    s = int(round(s))
    return f"{s // 3600}h{s % 3600 // 60:02d}m{s % 60:02d}s" if s >= 3600 else f"{s // 60}m{s % 60:02d}s"


def run_one(benchmark, variant, threads, ms, work, flags=()):
    """Runs bench once. Returns (fields list or None, error text or None, env).
    env = (background CPU % of all logical CPUs during the run, available RAM GB
    just before it, bench's own CPU % of all logical CPUs over its lifetime);
    each value is None if it could not be measured."""
    cmd = [BENCH, benchmark, variant, str(threads), str(ms), str(work), *flags]
    ram = avail_ram_gb()
    s0 = system_cpu_times()
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                         errors="replace")
    try:
        out, err = p.communicate(timeout=ms / 1000 + 300)
    except subprocess.TimeoutExpired:
        p.kill()
        p.communicate()
        return None, f"timeout: {' '.join(cmd)}", (None, ram, None)
    s1 = system_cpu_times()
    own = process_cpu_time(p._handle) if IS_WINDOWS else None
    bg = own_pct = None
    if s0 and s1 and own is not None:
        idle, busy = s1[0] - s0[0], s1[1] - s0[1]
        if idle + busy > 0:
            bg = max(0.0, 100.0 * (busy - own) / (idle + busy))
            own_pct = 100.0 * own / (idle + busy)
    line = out.strip()
    fields = line.split(",")
    if len(fields) != BENCH_FIELDS:
        return None, (f"bad output (exit {p.returncode}) from {' '.join(cmd[1:])}:\n"
                      f"  stdout: {line!r}\n  stderr: {err.strip()!r}"), (bg, ram, own_pct)
    return fields, None, (bg, ram, own_pct)


def main():
    if IS_WINDOWS:
        sys.stdout.reconfigure(encoding="utf-8")
        os.system("")  # enables ANSI escape processing in the Windows console

    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--quick", action="store_true", help="2 reps x 200 ms")
    ap.add_argument("--reps", type=int, help="repetitions (default 7)")
    ap.add_argument("--ms", type=int, help="milliseconds per run (default 500)")
    ap.add_argument("--rebuild", action="store_true", help="force a fresh build first")
    ap.add_argument("--shuffle", action="store_true",
                    help="run the configurations in a new random order in every rep")
    ap.add_argument("--seed", type=int, help="seed for --shuffle (default: random, recorded in sysinfo.txt)")
    ap.add_argument("--backoff", action="store_true",
                    help="also run the *_backoff variants (main, contention and latency sets)")
    ap.add_argument("--bench", help="binary to run instead of bench.exe (never rebuilt), "
                                    "e.g. bench_runs1-3.exe")
    ap.add_argument("--tag", help="label appended to the output file names, e.g. run4")
    args = ap.parse_args()
    global BENCH, RESULTS, SYSINFO

    reps = args.reps if args.reps is not None else (2 if args.quick else 7)
    ms = args.ms if args.ms is not None else (200 if args.quick else 500)
    if reps < 1 or ms < 1:
        sys.exit("--reps and --ms must be >= 1")
    if args.seed is not None and not args.shuffle:
        sys.exit("--seed only applies with --shuffle")
    seed = None
    if args.shuffle:
        seed = args.seed if args.seed is not None else random.SystemRandom().randrange(2**32)

    if args.tag is not None and not all(c.isalnum() or c in "-_" for c in args.tag):
        sys.exit("--tag may only contain letters, digits, '-' and '_'")

    if args.bench:
        if args.rebuild:
            sys.exit("--rebuild cannot be combined with --bench (that binary is never rebuilt)")
        BENCH = args.bench if os.path.isabs(args.bench) else os.path.join(HERE, args.bench)
        if not os.path.isfile(BENCH):
            sys.exit(f"--bench: {BENCH} not found")
        if args.backoff:
            probe = subprocess.run([BENCH, "counter", "spinlock_backoff", "1", "1"],
                                   capture_output=True, text=True, errors="replace")
            if probe.returncode != 0:
                sys.exit(f"--backoff: {os.path.basename(BENCH)} has no *_backoff variants")
    elif args.rebuild or not os.path.exists(BENCH):
        build()
    elif os.path.getmtime(os.path.join(HERE, "bench.cpp")) > os.path.getmtime(BENCH):
        print("bench.cpp is newer than bench.exe -> rebuilding.")
        build()
    with open(BENCH, "rb") as f:
        bench_sha = hashlib.sha256(f.read()).hexdigest()
    bench_built = datetime.datetime.fromtimestamp(os.path.getmtime(BENCH)).isoformat(timespec="seconds")

    # New output files named after the start time; never overwrite anything.
    started = datetime.datetime.now()
    stem = started.strftime("%Y-%m-%d_%H%M%S") + ("_quick" if args.quick else "") \
        + (f"_{args.tag}" if args.tag else "")
    RESULTS = os.path.join(HERE, f"results_{stem}.csv")
    SYSINFO = os.path.join(HERE, f"sysinfo_{stem}.txt")
    if os.path.exists(RESULTS) or os.path.exists(SYSINFO):
        sys.exit(f"{RESULTS} or {SYSINFO} already exists - not overwriting")

    L = os.cpu_count() or 1
    P = physical_cores()
    threads = thread_counts(L)

    # (benchmark name in CSV, bench benchmark, variant, threads, work, extra flags)
    main_set = MAIN_SET + (BACKOFF_MAIN if args.backoff else [])
    configs = []
    for t in threads:
        for b, v in main_set:
            configs.append((b, b, v, t, 0, ()))
    for label, b, variants in CONTENTION_SETS:
        if args.backoff:
            variants = variants + BACKOFF_CONTENTION[label]
        for w in CONTENTION_WORK:
            for v in variants:
                configs.append((label, b, v, L, w, ()))
    for t in LATENCY_THREADS:
        for b, v in main_set:
            configs.append((f"latency_{b}", b, v, t, 0, ("--latency",)))
    for t in HEAP_THREADS:
        for b, v in NODE_SET:
            configs.append((f"{b}_heap", b, v, t, 0, ("--heap",)))
    for t in PAD64_THREADS:
        for b, v in NODE_SET:
            configs.append((f"{b}_pad64", b, v, t, 0, ("--pad64",)))

    # ---- sysinfo.txt ----
    on_batt, pstate = power_state()
    ram = ram_gb()
    print(f"Measuring idle load for {IDLE_SAMPLE_S} s - please don't use the computer ...", flush=True)
    idle_pct = idle_load(IDLE_SAMPLE_S)
    idle_ram = avail_ram_gb()
    if P and L > P:
        smt = (f"yes - {P} physical cores with SMT ({L} logical, {L // P} hardware threads per core). "
               f"Runs with more than {P} threads place two threads on the same physical core "
               f"(sharing its execution units and L1/L2 caches); runs with more than {L} threads "
               f"oversubscribe the CPU (more threads than hardware threads).")
    elif P:
        smt = f"no ({P} cores, {L} logical)"
    else:
        smt = "unknown"
    info = [
        f"date:            {datetime.datetime.now().isoformat(timespec='seconds')}",
        f"cpu:             {cpu_name()}",
        f"physical cores:  {P if P else 'unknown'}",
        f"logical cores:   {L}",
        f"smt:             {smt}",
        f"ram:             {ram:.1f} GB" if ram else "ram:             unknown",
        f"os:              {os_version()}",
        f"power state:     {pstate}",
        f"power plan:      {power_plan()}",
        (f"idle before run: CPU busy {idle_pct:.1f}% of all logical CPUs over {IDLE_SAMPLE_S} s; "
         f"RAM available {idle_ram:.1f} of {ram:.1f} GB ({100 * (1 - idle_ram / ram):.0f}% in use)"
         if idle_pct is not None and idle_ram is not None and ram
         else "idle before run: not measured (Windows only)"),
        f"compiler:        {compiler_info()}"
        + (" (compiler installed now; see binary line for when the binary was built)" if args.bench else ""),
        f"binary:          {os.path.basename(BENCH)}, sha256 {bench_sha}, file time {bench_built}",
        f"results file:    {os.path.basename(RESULTS)}",
        f"python:          {platform.python_version()}",
        f"thread counts:   {', '.join(map(str, threads))}",
        f"contention sets: at {L} threads, work in {{{', '.join(map(str, CONTENTION_WORK))}}}: "
        + "; ".join(f"{lbl} = {b} {{{', '.join(vs)}}}" for lbl, b, vs in CONTENTION_SETS),
        f"latency set:     latency_counter/_stack/_queue = all 8 variants with --latency at "
        f"{', '.join(map(str, LATENCY_THREADS))} threads, work 0",
        f"allocator sets:  stack_heap/queue_heap (--heap, `new` per node in the timed loop) at "
        f"{', '.join(map(str, HEAP_THREADS))} threads; stack_pad64/queue_pad64 (--pad64, "
        f"64-byte node stride) at {', '.join(map(str, PAD64_THREADS))} threads; work 0",
        ("backoff:         --backoff: " + ", ".join(f"{b} {v}" for b, v in BACKOFF_MAIN)
         + " added to the main and latency sets; spinlock_backoff / lockfree_backoff added to the "
         "contention sets (randomized exponential backoff, cap 256 pauses; see cpu_pause below)"
         if args.backoff else "backoff:         not run (no --backoff)"),
        "node allocation: default = per-thread pool, pre-allocated and pre-touched before the "
        "start barrier (no allocation or page faults while timing); budget and stride below",
        "tsc / latency (measured by bench --tsc):",
        *(f"    {ln}" for ln in bench_info("--tsc")),
        "memory layout (bench --layout):",
        *(f"    {ln}" for ln in bench_info("--layout")),
        f"reps:            {reps}",
        f"ms per run:      {ms}",
        f"runs total:      {reps * len(configs)}",
        ("loop order:      for each rep -> for each configuration (interleaved)" if seed is None else
         f"loop order:      for each rep -> for each configuration (interleaved), configurations "
         f"shuffled in every rep with random.Random({seed}) (repeat with --shuffle --seed {seed})"),
    ]
    with open(SYSINFO, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(info) + "\n")
    print("\n".join(info))
    print(f"\nwrote {SYSINFO}")
    if on_batt:
        print("\n\x1b[1;31mWARNING: the laptop is running ON BATTERY. "
              "Results will be throttled - plug in the charger.\x1b[0m")

    # ---- runs ----
    total = reps * len(configs)
    done = 0
    failed = []
    env_bg, env_ram = [], []  # per run: background CPU %, available RAM GB
    env_own = []              # per run: (threads, bench's own CPU %)
    rng = random.Random(seed) if seed is not None else None
    t_start = time.monotonic()
    print(f"\nRunning {total} runs ({reps} reps x {len(configs)} configs x {ms} ms) -> {RESULTS}\n")
    with open(RESULTS, "w", encoding="utf-8", newline="\n") as out:
        out.write(HEADER + "\n")
        out.flush()
        for rep in range(1, reps + 1):
            order = configs
            if rng is not None:
                order = configs[:]
                rng.shuffle(order)
            for name, b, v, t, w, flags in order:
                fields, err, (bg, avail, own_pct) = run_one(b, v, t, ms, w, flags)
                if bg is not None:
                    env_bg.append(bg)
                if avail is not None:
                    env_ram.append(avail)
                if own_pct is not None:
                    env_own.append((t, own_pct))
                done += 1
                elapsed = time.monotonic() - t_start
                eta = elapsed / done * (total - done)
                label = f"{name:<16} {v:<10} t={t:<3} w={w:<6} rep {rep}/{reps}"
                if fields is None:
                    failed.append(label)
                    print(f"[{done:>4}/{total}] {label}  \x1b[31mERROR\x1b[0m\n{err}", flush=True)
                    continue
                fields[0] = name  # CSV label, e.g. "contention_stack", "latency_queue"
                env = ["" if bg is None else f"{bg:.2f}", "" if avail is None else f"{avail:.2f}",
                       "" if own_pct is None else f"{own_pct:.2f}"]
                row = fields[:BASE_FIELDS] + [str(rep)] + fields[BASE_FIELDS:] + env
                out.write(",".join(row) + "\n")
                out.flush()
                check = fields[10]
                if check != "ok":
                    failed.append(f"{label}: {check}")
                mops = float(fields[6]) / 1e6
                chk = "ok" if check == "ok" else f"\x1b[31m{check}\x1b[0m"
                lat = f"  p50 {fields[12]} ns p99 {fields[13]} ns" if fields[12] else ""
                bgs = (f"  bench cpu {own_pct:.0f}%" if own_pct is not None else "") \
                    + (f"  bg cpu {bg:.1f}%" if bg is not None else "")
                print(f"[{done:>4}/{total}] {label} {mops:10.2f} M ops/s  cv {float(fields[9]):.3f}  "
                      f"retries/op {float(fields[11]):.3f}{lat}  {chk}{bgs}   "
                      f"elapsed {fmt_dur(elapsed)}, remaining ~{fmt_dur(eta)}", flush=True)

    print(f"\nFinished in {fmt_dur(time.monotonic() - t_start)}. Results: {RESULTS}")
    if env_bg or env_ram:
        def pct(xs, q):
            xs = sorted(xs)
            return xs[min(len(xs) - 1, int(q * len(xs)))]
        lines = []
        if env_bg:
            lines.append(f"background CPU during runs (% of all logical CPUs, per run): "
                         f"median {pct(env_bg, 0.5):.1f}, p90 {pct(env_bg, 0.9):.1f}, "
                         f"max {max(env_bg):.1f}; runs above 5%: {sum(x > 5 for x in env_bg)} "
                         f"of {len(env_bg)}")
        if env_ram:
            lines.append(f"available RAM before runs: min {min(env_ram):.1f} GB, "
                         f"median {pct(env_ram, 0.5):.1f} GB")
        if env_own:
            by_t = {}
            for t, x in env_own:
                by_t.setdefault(t, []).append(x)
            lines.append("bench's own CPU per run (% of all logical CPUs, whole process lifetime), "
                         "median by thread count: "
                         + ", ".join(f"{t} thr {pct(xs, 0.5):.0f}%" for t, xs in sorted(by_t.items())))
        with open(SYSINFO, "a", encoding="utf-8", newline="\n") as f:
            f.write("".join(f"env:             {ln}\n" for ln in lines))
        print("\n".join(lines))
    print(f"Failed checks: {len(failed)}")
    for f in failed:
        print(f"  - {f}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
