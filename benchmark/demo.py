#!/usr/bin/env python3
"""Short live demo of lock-based vs lock-free synchronization (~30 s).

  python demo.py            1000 ms per run (default)
  python demo.py --ms 500

Every number shown is measured live by bench.exe; nothing is precomputed.
"""

import argparse
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
IS_WINDOWS = os.name == "nt"
BENCH = os.path.join(HERE, "bench.exe" if IS_WINDOWS else "bench")
BUILD_BAT = os.path.join(HERE, "build.bat")

RESET, BOLD, DIM = "\x1b[0m", "\x1b[1m", "\x1b[2m"
RED, GREEN, YELLOW, BLUE, MAGENTA, CYAN = (f"\x1b[{c}m" for c in (31, 32, 33, 34, 35, 36))
VARIANT_COLOUR = {"mutex": RED, "spinlock": YELLOW, "atomic_faa": GREEN,
                  "atomic_cas": CYAN, "lockfree": GREEN}
SPINNER = "⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏"
PARTIAL = " ▏▎▍▌▋▊▉"


def enable_windows_console():
    if not IS_WINDOWS:
        return
    sys.stdout.reconfigure(encoding="utf-8")
    try:
        import ctypes
        k32 = ctypes.windll.kernel32
        h = k32.GetStdHandle(-11)  # STD_OUTPUT_HANDLE
        mode = ctypes.c_ulong()
        if k32.GetConsoleMode(h, ctypes.byref(mode)):
            k32.SetConsoleMode(h, mode.value | 0x0004)  # ENABLE_VIRTUAL_TERMINAL_PROCESSING
        k32.SetConsoleOutputCP(65001)
    except (OSError, AttributeError):
        os.system("")


def w(s):
    sys.stdout.write(s)
    sys.stdout.flush()


def fairness(cv):
    if cv < 0.15:
        return GREEN + "fair" + RESET
    if cv < 0.4:
        return YELLOW + "uneven" + RESET
    return RED + "starving" + RESET


def bar(frac, width):
    frac = max(0.0, min(1.0, frac))
    eighths = int(round(frac * width * 8))
    full, part = divmod(eighths, 8)
    s = "█" * full + (PARTIAL[part] if part else "")
    return s + " " * (width - len(s))


class Block:
    """A group of bars that is redrawn in place (rescaled when a larger value arrives)."""

    def __init__(self):
        self.rows = []   # dicts: label, colour, value, cv, check
        self.drawn = 0   # lines currently on screen for this block
        self.width = max(20, min(50, shutil.get_terminal_size((100, 30)).columns - 58))

    def _lines(self, scale, grow=1.0):
        lines = []
        for i, r in enumerate(self.rows):
            v = r["value"] * (grow if i == len(self.rows) - 1 else 1.0)
            b = bar(v / scale if scale else 0, self.width)
            chk = "" if r["check"] == "ok" else f"  {RED}{BOLD}{r['check']}{RESET}"
            lines.append(f"  {r['label']:<22} {r['colour']}{b}{RESET} "
                         f"{BOLD}{v / 1e6:8.2f}{RESET} M ops/s  {fairness(r['cv'])} "
                         f"{DIM}(cv {r['cv']:.2f}){RESET}{chk}")
        return lines

    def _paint(self, lines):
        w("\r\x1b[2K")  # clear the spinner line
        if self.drawn:
            w(f"\x1b[{self.drawn}F")
        for ln in lines:
            w("\x1b[2K" + ln + "\n")
        self.drawn = len(lines)

    def add(self, row, frames=18, dt=0.022):
        old_scale = max((r["value"] for r in self.rows), default=0.0)
        self.rows.append(row)
        new_scale = max(old_scale, row["value"])
        for f in range(1, frames + 1):
            t = f / frames
            t = 1 - (1 - t) ** 3  # ease-out
            scale = old_scale + (new_scale - old_scale) * t if old_scale else new_scale
            self._paint(self._lines(scale, grow=t))
            time.sleep(dt)

    def error(self, text):
        w("\r\x1b[2K")
        w(f"  {RED}{text}{RESET}\n")
        self.drawn = 0  # don't redraw over the error message
        self.rows = []


def run_bench(benchmark, variant, threads, ms, label):
    cmd = [BENCH, benchmark, variant, str(threads), str(ms)]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         text=True, errors="replace")
    i = 0
    t0 = time.monotonic()
    while p.poll() is None:
        w(f"\r\x1b[2K  {CYAN}{SPINNER[i % len(SPINNER)]}{RESET} running {label} "
          f"{DIM}({threads} threads, {time.monotonic() - t0:.1f}s){RESET}")
        i += 1
        time.sleep(0.08)
    out, err = p.communicate()
    fields = out.strip().split(",")
    if len(fields) != 17:
        return None, f"bench failed (exit {p.returncode}): {out.strip()!r} {err.strip()!r}"
    return {"value": float(fields[6]), "cv": float(fields[9]), "check": fields[10]}, None


def scenario(title, subtitle, runs, ms):
    """runs: list of (benchmark, variant, threads, label). Returns {label: result}."""
    w(f"\n{BOLD}{MAGENTA}{title}{RESET}\n{DIM}{subtitle}{RESET}\n")
    time.sleep(0.6)  # give the audience a moment to read the title
    block = Block()
    results = {}
    for b, v, t, label in runs:
        res, err = run_bench(b, v, t, ms, label)
        if res is None:
            block.error(err)
            continue
        results[label] = res
        block.add({"label": label, "colour": VARIANT_COLOUR.get(v, BLUE), **res})
    time.sleep(1.8)  # pause on the finished chart
    return results


def speedup_text(fast, slow):
    if not fast or not slow or slow["value"] == 0:
        return f"{DIM}n/a{RESET}"
    r = fast["value"] / slow["value"]
    if 0.9 <= r <= 1.1:
        return f"{YELLOW}about the same{RESET} ({r:.2f}x)"
    colour = GREEN if r > 1 else RED
    return f"{colour}{BOLD}{r:.2f}x{RESET}" + (" faster" if r > 1 else " (slower)")


def main():
    enable_windows_console()
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ms", type=int, default=1000, help="milliseconds per run (default 1000)")
    args = ap.parse_args()

    if not os.path.exists(BENCH):
        print("bench.exe not found - building (MSVC Release) ...")
        if subprocess.run(["cmd", "/c", BUILD_BAT]).returncode != 0:
            sys.exit("build failed")

    L = os.cpu_count() or 1
    O = min(4 * L, 128)
    ms = args.ms

    w(f"\n{BOLD}Lock-based vs lock-free synchronization - live demo{RESET}\n")
    w(f"{DIM}{L} logical cores, {ms} ms per run, higher bar = more operations per second{RESET}\n")

    s1 = scenario("1) Shared counter",
                  f"{L} threads all increment one 64-bit counter",
                  [("counter", "mutex", L, "mutex"),
                   ("counter", "spinlock", L, "spinlock"),
                   ("counter", "atomic_faa", L, "atomic fetch_add"),
                   ("counter", "atomic_cas", L, "atomic CAS loop")], ms)
    s2 = scenario("2) Stack (push + pop)",
                  f"{L} threads; one mutex vs Treiber lock-free stack",
                  [("stack", "mutex", L, "mutex stack"),
                   ("stack", "lockfree", L, "Treiber (lock-free)")], ms)
    s3 = scenario("3) Queue (enqueue + dequeue)",
                  f"{L} threads; one mutex vs Michael-Scott lock-free queue",
                  [("queue", "mutex", L, "mutex queue"),
                   ("queue", "lockfree", L, "Michael-Scott (l-free)")], ms)
    s4 = scenario("4) Oversubscription",
                  f"{O} threads on {L} logical cores (4x more threads than cores)",
                  [("counter", "mutex", O, "mutex"),
                   ("counter", "spinlock", O, "spinlock"),
                   ("counter", "atomic_faa", O, "atomic fetch_add")], ms)

    w(f"\n{BOLD}Summary: lock-free / mutex throughput at {L} threads{RESET}\n")
    w(f"  counter, atomic fetch_add : {speedup_text(s1.get('atomic fetch_add'), s1.get('mutex'))}\n")
    w(f"  counter, atomic CAS loop  : {speedup_text(s1.get('atomic CAS loop'), s1.get('mutex'))}\n")
    w(f"  stack,   Treiber          : {speedup_text(s2.get('Treiber (lock-free)'), s2.get('mutex stack'))}\n")
    w(f"  queue,   Michael-Scott    : {speedup_text(s3.get('Michael-Scott (l-free)'), s3.get('mutex queue'))}\n")

    a, b = s1.get("spinlock"), s4.get("spinlock")
    if a and b and a["value"] > 0:
        change = (b["value"] / a["value"] - 1) * 100
        colour = RED if change < -10 else (GREEN if change > 10 else YELLOW)
        w(f"\n{BOLD}Spinlock under oversubscription{RESET}: {L} -> {O} threads: "
          f"{a['value'] / 1e6:.2f} -> {b['value'] / 1e6:.2f} M ops/s "
          f"({colour}{BOLD}{change:+.0f}%{RESET})\n")

    bad = [k for s in (s1, s2, s3, s4) for k, r in s.items() if r["check"] != "ok"]
    if bad:
        w(f"\n{RED}{BOLD}Correctness check FAILED for: {', '.join(bad)}{RESET}\n")
    else:
        w(f"\n{DIM}All runs passed their correctness checks.{RESET}\n")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        w(RESET + "\n")
