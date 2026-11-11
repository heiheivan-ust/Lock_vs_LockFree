// bench.cpp - lock-based vs lock-free synchronization micro-benchmark.
//
// Usage:  bench <benchmark> <variant> <threads> <millis> [work] [flags]
//         bench --layout | --tsc
//   counter : mutex | spinlock | atomic_faa | atomic_cas
//             | spinlock_backoff | atomic_cas_backoff
//   stack   : mutex | lockfree | lockfree_backoff  (lockfree = Treiber stack)
//   queue   : mutex | lockfree | lockfree_backoff  (lockfree = Michael-Scott queue)
//   *_backoff: the same algorithm with randomized exponential backoff after
//             every failed attempt (see BACKOFF)
//
//   work    : iterations of non-critical local work after each counter
//             increment / push+pop pair / enqueue+dequeue pair (default 0)
//
// Flags:
//   --latency  time every 64th iteration individually with the CPU timestamp
//              counter (see LATENCY SAMPLING) and fill the lat_* columns
//   --heap     stack/queue only: allocate each node with `new` inside the
//              timed loop (old behaviour) instead of the pre-touched pool
//   --pad64    stack/queue only: pool nodes on a 64-byte stride (one node per
//              cache line) instead of 16 bytes
//   --backoff-max N  *_backoff variants only: cap of the backoff limit in
//              cpu_pause() calls (default 256)
//   --layout  print the memory layout of all structures and exit
//   --tsc      print the invariant-TSC CPUID bit and TSC frequency and exit
//
// Output: exactly one CSV line, no header:
//   benchmark,variant,threads,millis,work,total_ops,ops_per_sec,
//   min_thread_ops,max_thread_ops,cv,check,retries_per_op,
//   lat_p50_ns,lat_p99_ns,lat_p999_ns,lat_max_ns,lat_samples
// The five lat_* fields are empty unless --latency is given.
// check: ok | MISMATCH (correctness check failed) | OVERFLOW (a thread used up
//        its pre-allocated budget and stopped early; should never happen).
// Exit code: 0 = ok, 1 = MISMATCH, 2 = usage error, 3 = OVERFLOW,
//            4 = not enough free RAM (needs 2x the run's pre-allocated budget;
//                checked before anything is allocated; no CSV line printed).
//
// retries_per_op = (failed attempts summed over all threads) / total_ops.
// A "failed attempt" is:
//   atomic_cas, Treiber : one failed CAS (= one extra loop iteration)
//   spinlock            : one failed lock exchange
//   Michael-Scott       : one extra loop iteration beyond the first in an
//                         enqueue/dequeue call, whatever caused it (failed
//                         link/head CAS, a re-read that found tail/head moved,
//                         or a helping iteration). A failed tail-swing CAS
//                         after a successful link ends the call and is NOT
//                         counted.
//   mutex, atomic_faa   : always 0.
// Each thread counts failures in a local variable passed by reference (no
// shared writes); the increment sits only on the retry path.
// compare_exchange_weak may also fail spuriously on LL/SC architectures (ARM);
// on x86 it does not.
//
// Single file, C++20. Builds with MSVC, GCC and Clang.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>  // GlobalMemoryStatusEx (free-RAM check)
#endif
#if defined(_MSC_VER)
#include <intrin.h>
#pragma warning(disable : 4324)  // "structure was padded due to alignment specifier" - intended
#endif
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
#include <cpuid.h>
#include <x86intrin.h>
#endif

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#define BENCH_X86 1
#else
#define BENCH_X86 0
#endif

// ---------------------------------------------------------------------------
// Platform helpers
// ---------------------------------------------------------------------------

constexpr std::size_t kCacheLine = 64;
constexpr std::size_t kPageSize = 4096;

// CPU hint used while spinning: tells the core we are in a spin-wait loop
// (saves power, frees resources for the SMT sibling, avoids a memory-order
// mis-speculation penalty when the lock is released).
static inline void cpu_pause() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(_MSC_VER) && (defined(_M_ARM64) || defined(_M_ARM))
    __yield();
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
    __builtin_ia32_pause();
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__aarch64__) || defined(__arm__))
    asm volatile("yield" ::: "memory");
#else
    std::this_thread::yield();
#endif
}

// BACKOFF (*_backoff variants): after every failed attempt the thread waits a
// random number of cpu_pause() calls, uniform in [1, limit]. `limit` starts at
// 1 and doubles after each failure up to g_backoff_max (--backoff-max,
// default 256); it starts again at 1 for every operation. The random numbers
// come from a per-thread xorshift32 generator, so threads that collided do not
// retry in lockstep. The wait only runs on the failure path; the first attempt
// of every operation is identical to the variant without backoff.
static int g_backoff_max = 256;  // set before any worker thread starts

struct NoBackoff {
    static constexpr bool kEnabled = false;
    void fail() {}
};

#if defined(_MSC_VER) && !defined(__clang__)
#define BENCH_NOINLINE __declspec(noinline)
#else
#define BENCH_NOINLINE __attribute__((noinline, cold))
#endif

// Out of line, so the wait code does not change how the compiler lays out the
// success path of the calling loop.
BENCH_NOINLINE static void backoff_wait(std::uint32_t& limit) {
    thread_local std::uint32_t rng = 0;
    if (rng == 0)  // seed once per thread; the TLS address differs per thread
        rng = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&rng) >> 4) | 1u;
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    const std::uint32_t n = 1 + rng % limit;
    for (std::uint32_t i = 0; i < n; ++i) cpu_pause();
    if (limit < static_cast<std::uint32_t>(g_backoff_max))
        limit = std::min(limit * 2, static_cast<std::uint32_t>(g_backoff_max));
}

struct ExpBackoff {
    static constexpr bool kEnabled = true;
    std::uint32_t limit = 1;
    void fail() { backoff_wait(limit); }
};

// Compiler-only barrier: emits no instruction, but stops the optimiser from
// deleting or merging the iterations of the "non-critical work" loop.
static inline void compiler_barrier() {
#if defined(_MSC_VER) && !defined(__clang__)
    _ReadWriteBarrier();
#else
    asm volatile("" ::: "memory");
#endif
}

// ----- Timestamp counter (latency sampling) ---------------------------------
// tsc_begin: lfence before RDTSC waits for earlier instructions to finish;
// lfence after it stops the timed operation from starting before the read.
// tsc_end: RDTSCP waits for the timed operation's instructions to execute;
// the trailing lfence stops later instructions from starting before it.
// On non-x86 targets steady_clock nanoseconds are used instead.

static inline std::uint64_t tsc_now() {
#if BENCH_X86
    return __rdtsc();
#else
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}
static inline std::uint64_t tsc_begin() {
#if BENCH_X86
    _mm_lfence();
    std::uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
#else
    return tsc_now();
#endif
}
static inline std::uint64_t tsc_end() {
#if BENCH_X86
    unsigned aux;
    std::uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    return t;
#else
    return tsc_now();
#endif
}

// CPUID 0x80000007, EDX bit 8: "invariant TSC" (constant rate in all P/C-states).
// Returns 1 = yes, 0 = no, -1 = not x86 / leaf not available.
static int invariant_tsc() {
#if BENCH_X86 && defined(_MSC_VER)
    int r[4];
    __cpuid(r, static_cast<int>(0x80000000u));
    if (static_cast<unsigned>(r[0]) < 0x80000007u) return -1;
    __cpuid(r, static_cast<int>(0x80000007u));
    return (static_cast<unsigned>(r[3]) >> 8) & 1u;
#elif BENCH_X86
    unsigned a, b, c, d;
    if (!__get_cpuid(0x80000007u, &a, &b, &c, &d)) return -1;
    return (d >> 8) & 1u;
#else
    return -1;
#endif
}

// Writes one byte per 4 KB page so every page is faulted in before timing.
static void touch_pages(void* p, std::size_t bytes) {
    volatile char* c = static_cast<volatile char*>(p);
    for (std::size_t i = 0; i < bytes; i += kPageSize) c[i] = 0;
    if (bytes) c[bytes - 1] = 0;
}

// ---------------------------------------------------------------------------
// Counter implementations (one shared 64-bit counter, each struct alignas(64))
// ---------------------------------------------------------------------------

struct alignas(kCacheLine) MutexCounter {
    std::mutex m;
    std::uint64_t value = 0;
    void increment(std::uint64_t& /*fails: always 0 for a mutex*/) {
        std::lock_guard<std::mutex> g(m);
        ++value;
    }
    std::uint64_t get() const { return value; }
};

// Test-and-test-and-set spinlock: the expensive exchange (which takes the
// cache line in exclusive state) is only attempted after a cheap relaxed load
// has seen the lock free, so waiting threads spin in their own cache.
// With ExpBackoff, a failed exchange is followed by a backoff wait before the
// thread goes back to watching the lock.
template <class Backoff>
struct alignas(kCacheLine) SpinlockCounterT {
    std::atomic<bool> locked{false};
    std::uint64_t value = 0;  // protected by `locked`
    void lock(std::uint64_t& fails) {
        Backoff backoff;
        for (;;) {
            // acquire: the critical section must not move above lock().
            if (!locked.exchange(true, std::memory_order_acquire)) return;
            ++fails;  // exchange saw the lock taken: one failed attempt
            backoff.fail();
            // relaxed: only a hint; correctness comes from the exchange above.
            while (locked.load(std::memory_order_relaxed)) cpu_pause();
        }
    }
    void unlock() {
        // release: the write to `value` is visible to the next lock owner.
        locked.store(false, std::memory_order_release);
    }
    void increment(std::uint64_t& fails) {
        lock(fails);
        ++value;
        unlock();
    }
    std::uint64_t get() const { return value; }
};
using SpinlockCounter = SpinlockCounterT<NoBackoff>;

// relaxed is sufficient for a pure event counter: atomicity of the RMW
// guarantees no lost increments; no other memory is published through it.
struct alignas(kCacheLine) FaaCounter {
    std::atomic<std::uint64_t> value{0};
    void increment(std::uint64_t& /*fails: fetch_add cannot fail*/) {
        value.fetch_add(1, std::memory_order_relaxed);
    }
    std::uint64_t get() const { return value.load(std::memory_order_relaxed); }
};

template <class Backoff>
struct alignas(kCacheLine) CasCounterT {
    std::atomic<std::uint64_t> value{0};
    void increment(std::uint64_t& fails) {
        Backoff backoff;
        std::uint64_t expected = value.load(std::memory_order_relaxed);
        // On failure compare_exchange_weak reloads `expected`; just retry.
        while (!value.compare_exchange_weak(expected, expected + 1,
                                            std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {
            ++fails;
            backoff.fail();
            // After a wait, the value reloaded by the failed CAS is likely stale.
            if constexpr (Backoff::kEnabled) expected = value.load(std::memory_order_relaxed);
        }
    }
    std::uint64_t get() const { return value.load(std::memory_order_relaxed); }
};
using CasCounter = CasCounterT<NoBackoff>;

// ---------------------------------------------------------------------------
// Node shared by ALL stack and queue variants (same layout, same allocation
// policy), so that only the synchronization differs.
// `next` is atomic in every variant; the mutex variants access it with
// relaxed operations under the lock, which costs the same as a plain access.
// ---------------------------------------------------------------------------

struct Node {
    std::atomic<Node*> next{nullptr};
    std::uint64_t value = 0;  // unique payload, written once before publication
};
static_assert(std::is_trivially_destructible_v<Node>, "pools skip node destructors");

// NODE ALLOCATION (default): nothing is allocated and no page is faulted in
// inside the timed region. Before the start barrier each thread allocates a
// NodePool with a budget of ceil(K * seconds / threads) + 1M nodes (K = 384M
// nodes/s for 1 thread, 128M for 2, 32M for 4+; see node_budget_per_thread)
// and writes one byte to every 4 KB page of it. Before anything is allocated,
// main() checks that free RAM is at least twice the run's total budget and
// aborts otherwise. During the run a node is handed out by
// bumping a thread-local pointer (no shared state, no synchronization). The
// stride is sizeof(Node) = 16 bytes, or 64 bytes with --pad64 (one node per
// cache line). If a thread exhausts its budget it stops and the run reports
// OVERFLOW instead of allocating.
// With --heap each node is instead allocated with `new` inside the timed loop
// (the original behaviour, kept as a control).
//
// MEMORY RECLAMATION (deliberate simplification):
// Popped/dequeued nodes are NOT freed or reused during the timed run. Each
// thread stores them by index in its own pre-sized, pre-touched "retired"
// array (used only for the correctness check); pools and heap nodes are freed
// only after timing and verification have ended. Because no node address is
// ever handed out twice during a run, a CAS can never see "the same pointer"
// referring to a different node, so the ABA problem cannot occur. A
// production lock-free structure would need hazard pointers, epoch-based
// reclamation or tagged pointers instead.

constexpr std::size_t kMi = std::size_t{1} << 20;
constexpr std::size_t kExtraNodesPerThread = 1 * kMi;

// Node budget rate K (nodes per second of run, shared out between threads),
// chosen above the highest node rate observed for each thread count: one
// thread is uncontended and very fast; with 4+ threads contention caps the
// combined rate far lower.
static std::size_t node_rate_per_second(int threads) {
    if (threads == 1) return 384 * kMi;
    if (threads == 2) return 128 * kMi;
    return 32 * kMi;
}

// Per-thread budget = ceil(K * seconds / threads) + 1M nodes.
static std::size_t node_budget_per_thread(int threads, int millis) {
    const std::size_t num = node_rate_per_second(threads) * static_cast<std::size_t>(millis);
    const std::size_t den = std::size_t{1000} * static_cast<std::size_t>(threads);
    return (num + den - 1) / den + kExtraNodesPerThread;
}

class NodePool {
public:
    NodePool() = default;
    NodePool(const NodePool&) = delete;
    NodePool& operator=(const NodePool&) = delete;
    ~NodePool() { release(); }

    // Allocates `count` slots of `stride` bytes (64-byte aligned) and faults
    // every page in. Called before the start barrier.
    void init(std::size_t count, std::size_t stride) {
        release();
        bytes_ = count * stride;
        base_ = static_cast<char*>(::operator new(bytes_, std::align_val_t{kCacheLine}));
        touch_pages(base_, bytes_);
        cur_ = base_;
        end_ = base_ + bytes_;
        stride_ = stride;
    }
    // Constructs a fresh node in the next slot, or returns nullptr when the
    // budget is used up (the caller then stops: OVERFLOW).
    Node* alloc() {
        if (cur_ == end_) return nullptr;
        Node* n = ::new (static_cast<void*>(cur_)) Node();
        cur_ += stride_;
        return n;
    }
    // Node is trivially destructible, so no destructor calls are needed.
    void release() {
        if (base_) ::operator delete(base_, std::align_val_t{kCacheLine});
        base_ = cur_ = end_ = nullptr;
        bytes_ = 0;
    }

private:
    char* base_ = nullptr;
    char* cur_ = nullptr;
    char* end_ = nullptr;
    std::size_t stride_ = sizeof(Node);
    std::size_t bytes_ = 0;
};

// ----- Stack: one mutex ----------------------------------------------------

struct MutexStack {
    alignas(kCacheLine) std::mutex m;
    Node* head = nullptr;

    // `fails` is unused: a mutex has no failed attempts (always 0).
    void push(Node* n, std::uint64_t& /*fails*/) {
        std::lock_guard<std::mutex> g(m);
        n->next.store(head, std::memory_order_relaxed);
        head = n;
    }
    Node* pop(std::uint64_t& /*fails*/) {
        std::lock_guard<std::mutex> g(m);
        Node* h = head;
        if (h) head = h->next.load(std::memory_order_relaxed);
        return h;
    }
};

// ----- Stack: Treiber (lock-free, CAS on head for push and pop) -------------

// With ExpBackoff, every failed CAS is followed by a backoff wait and a fresh
// read of head (the value returned by the failed CAS is likely stale by then).
template <class Backoff>
struct TreiberStackT {
    alignas(kCacheLine) std::atomic<Node*> head{nullptr};

    void push(Node* n, std::uint64_t& fails) {
        Backoff backoff;
        Node* h = head.load(std::memory_order_relaxed);
        for (;;) {
            n->next.store(h, std::memory_order_relaxed);
            // success = release: n->value and n->next are visible to any
            // thread that later reads `head` with acquire and sees n.
            // failure = relaxed: we only use the new h to relink n.
            if (head.compare_exchange_weak(h, n, std::memory_order_release,
                                           std::memory_order_relaxed))
                return;
            ++fails;
            backoff.fail();
            if constexpr (Backoff::kEnabled) h = head.load(std::memory_order_relaxed);
        }
    }
    Node* pop(std::uint64_t& fails) {
        Backoff backoff;
        // acquire (load and failure): we dereference h (h->next), so we must
        // see the pusher's writes to that node.
        Node* h = head.load(std::memory_order_acquire);
        // h->next is safe to read even if h was popped concurrently, because
        // nodes are never freed during the run (see MEMORY RECLAMATION).
        while (h && !head.compare_exchange_weak(h, h->next.load(std::memory_order_relaxed),
                                                std::memory_order_acquire,
                                                std::memory_order_acquire)) {
            ++fails;
            backoff.fail();
            if constexpr (Backoff::kEnabled) h = head.load(std::memory_order_acquire);
        }
        return h;
    }
};
using TreiberStack = TreiberStackT<NoBackoff>;

// ----- Queue: linked list with dummy node, one mutex ------------------------
// Dequeue follows the same convention as Michael-Scott: the value is taken
// from head->next, which becomes the new dummy; the OLD dummy is returned so
// the caller retires it. Both queue variants therefore retire the same nodes.

struct MutexQueue {
    alignas(kCacheLine) std::mutex m;
    Node* head;
    Node* tail;

    explicit MutexQueue(Node* dummy) : head(dummy), tail(dummy) {}

    // `fails` is unused: a mutex has no failed attempts (always 0).
    void enqueue(Node* n, std::uint64_t& /*fails*/) {
        n->next.store(nullptr, std::memory_order_relaxed);
        std::lock_guard<std::mutex> g(m);
        tail->next.store(n, std::memory_order_relaxed);
        tail = n;
    }
    // Returns the old dummy (to retire) or nullptr if empty; value via `out`.
    Node* dequeue(std::uint64_t& out, std::uint64_t& /*fails*/) {
        std::lock_guard<std::mutex> g(m);
        Node* h = head;
        Node* next = h->next.load(std::memory_order_relaxed);
        if (!next) return nullptr;
        out = next->value;
        head = next;
        return h;
    }
    Node* dummy() const { return head; }
};

// ----- Queue: Michael & Scott (PODC 1996), lock-free ------------------------
// Head and tail are on separate cache lines. Enqueue uses two CAS (link the
// node at tail->next, then swing tail); any thread that finds tail lagging
// behind (tail->next != nullptr) helps by swinging tail forward. The original
// paper uses counted pointers against ABA; they are unnecessary here because
// addresses are never reused during the run (see MEMORY RECLAMATION).
// With ExpBackoff, a failed link CAS (enqueue) or head CAS (dequeue) is
// followed by a backoff wait; the loop then re-reads head/tail as usual.
// Re-reads that find head/tail moved and helping iterations do not back off.

template <class Backoff>
struct MSQueueT {
    alignas(kCacheLine) std::atomic<Node*> head;
    alignas(kCacheLine) std::atomic<Node*> tail;  // struct size rounds up to 2 lines

    explicit MSQueueT(Node* dummy) : head(dummy), tail(dummy) {}

    // `fails` counts one failure per loop iteration beyond the first in each
    // call (`extra`), whatever caused the retry: a failed link/head CAS, a
    // re-read that found tail/head moved (`continue`), or a helping iteration.
    // The tail-swing CAS after a successful link is not counted: the call
    // ends there whether or not it succeeds.
    void enqueue(Node* n, std::uint64_t& fails) {
        Backoff backoff;
        n->next.store(nullptr, std::memory_order_relaxed);
        for (std::uint64_t extra = 0;; ++extra) {
            // acquire: we dereference t (t->next).
            Node* t = tail.load(std::memory_order_acquire);
            Node* next = t->next.load(std::memory_order_acquire);
            if (t != tail.load(std::memory_order_acquire)) continue;  // tail moved, retry
            if (next == nullptr) {
                // CAS #1: link n after the last node.
                // release: publishes n->value / n->next to dequeuers, which
                // read t->next with acquire.
                if (t->next.compare_exchange_weak(next, n, std::memory_order_release,
                                                  std::memory_order_relaxed)) {
                    // CAS #2: swing tail to n. May fail if another thread
                    // already helped; that is fine.
                    tail.compare_exchange_strong(t, n, std::memory_order_release,
                                                 std::memory_order_relaxed);
                    fails += extra;
                    return;
                }
                backoff.fail();  // link CAS lost to another enqueuer
            } else {
                // Tail is lagging: help by advancing it, then retry.
                tail.compare_exchange_weak(t, next, std::memory_order_release,
                                           std::memory_order_relaxed);
            }
        }
    }

    // Returns the old dummy (to retire) or nullptr if empty; value via `out`.
    Node* dequeue(std::uint64_t& out, std::uint64_t& fails) {
        Backoff backoff;
        for (std::uint64_t extra = 0;; ++extra) {
            Node* h = head.load(std::memory_order_acquire);
            Node* t = tail.load(std::memory_order_acquire);
            Node* next = h->next.load(std::memory_order_acquire);
            if (h != head.load(std::memory_order_acquire)) continue;  // head moved, retry
            if (h == t) {
                if (next == nullptr) {  // empty
                    fails += extra;
                    return nullptr;
                }
                // Tail is lagging behind a linked node: help advance it.
                tail.compare_exchange_weak(t, next, std::memory_order_release,
                                           std::memory_order_relaxed);
            } else {
                // Read the value BEFORE the CAS, as in the paper (after the
                // CAS another dequeuer could already treat `next` as dummy).
                // `next` was obtained by an acquire load, so the enqueuer's
                // write of next->value happens-before this read.
                std::uint64_t v = next->value;
                // relaxed is enough on success: nothing new is published
                // through head, and `next` was already acquired above.
                if (head.compare_exchange_weak(h, next, std::memory_order_relaxed,
                                               std::memory_order_relaxed)) {
                    out = v;
                    fails += extra;
                    return h;
                }
                backoff.fail();  // head CAS lost to another dequeuer
            }
        }
    }
    Node* dummy() const { return head.load(std::memory_order_relaxed); }
};
using MSQueue = MSQueueT<NoBackoff>;

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------

// LATENCY SAMPLING (--latency): every 64th loop iteration of each thread is
// timed individually with the TSC. Counter: the increment is one sample.
// Stack/queue: the push and the pop (enqueue and dequeue) of that iteration
// are two separate samples. Either way 1 in 64 operations is timed. Samples
// are raw TSC ticks in a pre-sized, pre-touched per-thread buffer; they are
// converted to ns after the run with a per-run calibration (TSC ticks vs
// steady_clock over the timed interval). Percentiles are taken over all
// samples of all threads combined (push and pop samples together).
constexpr std::uint64_t kSampleMask = 63;  // sample when (iteration & 63) == 0
constexpr std::size_t kTotalSampleBudget = 16 * kMi;

static std::size_t sample_budget_per_thread(int threads) {
    const std::size_t t = static_cast<std::size_t>(threads);
    return (kTotalSampleBudget + t - 1) / t + 1 * kMi;
}

struct Options {
    int threads = 0;
    int millis = 0;
    int work = 0;
    bool latency = false;
    bool heap = false;
    bool pad64 = false;
};

// Per-thread results, each on its own cache line(s) to avoid false sharing.
struct alignas(kCacheLine) ThreadResult {
    std::uint64_t ops = 0;
    std::uint64_t fails = 0;       // failed attempts (retries_per_op)
    std::uint64_t pushed = 0;      // stack/queue: nodes inserted
    std::uint64_t pushed_sum = 0;  // stack/queue: sum of inserted payloads
    std::uint64_t popped_sum = 0;  // stack/queue: sum of removed payloads
    bool overflow = false;         // budget exhausted -> thread stopped early
    std::vector<Node*> retired;    // removed nodes, pre-sized (verification only)
    std::size_t nretired = 0;
    NodePool pool;                 // this thread's node pool (unused with --heap)
    std::vector<std::uint64_t> samples;  // latency samples in TSC ticks, pre-sized
    std::size_t nsamples = 0;
};

struct alignas(kCacheLine) SharedFlags {
    alignas(kCacheLine) std::atomic<int> ready{0};
    alignas(kCacheLine) std::atomic<bool> start{false};
    alignas(kCacheLine) std::atomic<bool> stop{false};
};

struct RunTiming {
    double seconds = 0.0;
    std::uint64_t ticks = 0;  // TSC ticks over the same interval (calibration)
};

// Runs `setup(i, result)` and then `body(i, result)` on `threads` threads.
// setup (allocation and page pre-touching) runs before a thread checks in at
// the barrier. The main thread waits until all threads have checked in, then
// releases them with the start flag, sleeps `millis`, sets the stop flag and
// joins. Elapsed time (and TSC ticks) are measured from setting the start
// flag to the end of the joins, before any verification work.
template <class Setup, class Body>
static RunTiming run_threads(int threads, int millis, SharedFlags& flags,
                             std::vector<ThreadResult>& results, Setup setup, Body body) {
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (int i = 0; i < threads; ++i) {
        pool.emplace_back([&, i] {
            setup(i, results[i]);
            flags.ready.fetch_add(1, std::memory_order_acq_rel);
            // Yield while waiting so oversubscribed runs (threads > cores)
            // can get all threads to the barrier quickly.
            while (!flags.start.load(std::memory_order_acquire)) std::this_thread::yield();
            body(i, results[i]);
        });
    }
    while (flags.ready.load(std::memory_order_acquire) < threads) std::this_thread::yield();

    auto t0 = std::chrono::steady_clock::now();
    std::uint64_t k0 = tsc_now();
    flags.start.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(millis));
    flags.stop.store(true, std::memory_order_relaxed);
    for (auto& t : pool) t.join();
    std::uint64_t k1 = tsc_now();
    auto t1 = std::chrono::steady_clock::now();

    return RunTiming{std::chrono::duration<double>(t1 - t0).count(), k1 - k0};
}

static inline bool should_stop(const SharedFlags& f) {
    return f.stop.load(std::memory_order_relaxed);
}

// Unique payload for node number `seq` created by thread `tid`.
static inline std::uint64_t make_payload(int tid, std::uint64_t seq) {
    return (static_cast<std::uint64_t>(tid) << 48) | seq;
}

// Pre-sizes and pre-touches the latency sample buffer (vector value-init
// writes every element, which faults every page in).
static void setup_samples(const Options& o, ThreadResult& r) {
    if (o.latency) r.samples.assign(sample_budget_per_thread(o.threads), 0);
}

// ----- counter ---------------------------------------------------------------

template <bool kLat, class Counter>
static void counter_worker(Counter* counter, const SharedFlags& flags, int work,
                           ThreadResult& r) {
    std::uint64_t ops = 0, fails = 0;
    std::uint64_t* smp = r.samples.data();
    const std::size_t scap = r.samples.size();
    std::size_t ns = 0;
    bool overflow = false;
    while (!should_stop(flags)) {
        if constexpr (kLat) {
            if (((ops + 1) & kSampleMask) == 0) {
                if (ns == scap) { overflow = true; break; }
                std::uint64_t a = tsc_begin();
                counter->increment(fails);
                std::uint64_t b = tsc_end();
                smp[ns++] = b - a;
            } else {
                counter->increment(fails);
            }
        } else {
            counter->increment(fails);
        }
        for (int k = 0; k < work; ++k) compiler_barrier();  // non-critical local work
        ++ops;
    }
    r.ops = ops;
    r.fails = fails;
    r.nsamples = ns;
    r.overflow = overflow;
}

template <class Counter>
static bool bench_counter(const Options& o, std::vector<ThreadResult>& results, RunTiming& t) {
    auto* counter = new Counter();  // heap: guarantees the alignas(64) placement
    SharedFlags flags;
    auto setup = [&](int, ThreadResult& r) { setup_samples(o, r); };
    if (o.latency)
        t = run_threads(o.threads, o.millis, flags, results, setup, [&](int, ThreadResult& r) {
            counter_worker<true>(counter, flags, o.work, r);
        });
    else
        t = run_threads(o.threads, o.millis, flags, results, setup, [&](int, ThreadResult& r) {
            counter_worker<false>(counter, flags, o.work, r);
        });

    // Check: final counter value == sum of per-thread op counts.
    std::uint64_t sum = 0;
    for (auto& r : results) sum += r.ops;
    bool ok = counter->get() == sum;
    delete counter;
    return ok;
}

// ----- stack / queue ---------------------------------------------------------

// Pre-sizes and pre-touches the retired array and (unless --heap) the node
// pool. Runs on the worker thread before it checks in at the barrier.
static void setup_nodes(const Options& o, ThreadResult& r) {
    const std::size_t budget = node_budget_per_thread(o.threads, o.millis);
    r.retired.assign(budget, nullptr);  // value-init writes every page
    if (!o.heap) r.pool.init(budget, o.pad64 ? kCacheLine : sizeof(Node));
    setup_samples(o, r);
}

template <bool kHeap>
static inline Node* new_node(ThreadResult& r) {
    if constexpr (kHeap) return new Node();
    else return r.pool.alloc();
}

// One worker loop for both stack and queue. `insert`/`remove` wrap
// push/pop or enqueue/dequeue; remove returns the node to retire (popped
// node, or old dummy for the queue) and adds the removed payload to `qsum`.
template <bool kLat, bool kHeap, class Insert, class Remove>
static void node_worker(int tid, const SharedFlags& flags, int work, ThreadResult& r,
                        Insert insert, Remove remove) {
    std::uint64_t ops = 0, fails = 0, seq = 0, psum = 0, qsum = 0;
    Node** ret = r.retired.data();
    const std::size_t rcap = r.retired.size();
    std::size_t nret = 0;
    std::uint64_t* smp = r.samples.data();
    const std::size_t scap = r.samples.size();
    std::size_t ns = 0;
    bool overflow = false;
    while (!should_stop(flags)) {
        const bool sample = kLat && ((seq + 1) & kSampleMask) == 0;
        if (nret == rcap || (sample && ns + 2 > scap)) { overflow = true; break; }
        Node* n = new_node<kHeap>(r);
        if (!n) { overflow = true; break; }  // pool budget exhausted
        n->value = make_payload(tid, ++seq);
        psum += n->value;
        Node* p;
        if (sample) {
            std::uint64_t a = tsc_begin();
            insert(n, fails);
            std::uint64_t b = tsc_end();
            smp[ns++] = b - a;
            a = tsc_begin();
            p = remove(qsum, fails);
            b = tsc_end();
            smp[ns++] = b - a;
        } else {
            insert(n, fails);
            p = remove(qsum, fails);
        }
        if (p) ret[nret++] = p;
        for (int k = 0; k < work; ++k) compiler_barrier();  // non-critical local work
        ops += 2;
    }
    r.ops = ops;
    r.fails = fails;
    r.pushed = seq;
    r.pushed_sum = psum;
    r.popped_sum = qsum;
    r.nretired = nret;
    r.nsamples = ns;
    r.overflow = overflow;
}

// Dispatches the four <latency, heap> instantiations of node_worker.
template <class Insert, class Remove>
static RunTiming run_node_bench(const Options& o, std::vector<ThreadResult>& results,
                                SharedFlags& flags, Insert insert, Remove remove) {
    auto setup = [&](int, ThreadResult& r) { setup_nodes(o, r); };
    auto go = [&](auto lat, auto heap) {
        return run_threads(o.threads, o.millis, flags, results, setup,
                           [&](int tid, ThreadResult& r) {
                               node_worker<decltype(lat)::value, decltype(heap)::value>(
                                   tid, flags, o.work, r, insert, remove);
                           });
    };
    using T = std::true_type;
    using F = std::false_type;
    if (o.latency) return o.heap ? go(T{}, T{}) : go(T{}, F{});
    return o.heap ? go(F{}, T{}) : go(F{}, F{});
}

// Common verification for stack/queue: every pushed node must appear exactly
// once across all retired + drained nodes (count first, then sort the
// pointers and look for duplicates). `expected_extra` counts nodes that are
// not "pushed" nodes but are part of the structure (the queue's initial
// dummy). On return `all` holds every distinct node seen, sorted.
static bool verify_nodes(std::vector<ThreadResult>& results, const std::vector<Node*>& drained,
                         std::uint64_t drained_sum, std::size_t expected_extra,
                         std::vector<Node*>& all) {
    std::uint64_t pushed = 0, pushed_sum = 0, popped_sum = drained_sum;
    std::size_t total = drained.size();
    for (auto& r : results) {
        pushed += r.pushed;
        pushed_sum += r.pushed_sum;
        popped_sum += r.popped_sum;
        total += r.nretired;
    }
    bool ok = (total == pushed + expected_extra);

    all.clear();
    all.reserve(total);
    all.insert(all.end(), drained.begin(), drained.end());
    for (auto& r : results) {
        all.insert(all.end(), r.retired.begin(), r.retired.begin() + r.nretired);
        std::vector<Node*>().swap(r.retired);
    }
    std::sort(all.begin(), all.end());
    if (std::adjacent_find(all.begin(), all.end()) != all.end()) ok = false;

    // Extra check: every payload that went in came out (wrap-around sum).
    if (pushed_sum != popped_sum) ok = false;

    all.erase(std::unique(all.begin(), all.end()), all.end());
    return ok;
}

// Frees all nodes after verification. Pool mode: release every pool; only the
// queue's initial dummy came from `new`. Heap mode: every node came from
// `new` (including the queue's initial dummy, which is in `all`).
static void free_nodes(const Options& o, std::vector<ThreadResult>& results,
                       std::vector<Node*>& all, Node* initial_dummy) {
    if (o.heap) {
        for (Node* n : all) delete n;
    } else {
        for (auto& r : results) r.pool.release();
        delete initial_dummy;
    }
}

template <class Stack>
static bool bench_stack(const Options& o, std::vector<ThreadResult>& results, RunTiming& t) {
    auto* s = new Stack();
    SharedFlags flags;
    t = run_node_bench(
        o, results, flags, [s](Node* n, std::uint64_t& fails) { s->push(n, fails); },
        [s](std::uint64_t& qsum, std::uint64_t& fails) {
            Node* p = s->pop(fails);
            if (p) qsum += p->value;  // safe: acquired, and never freed during the run
            return p;
        });

    std::vector<Node*> drained;
    std::uint64_t dsum = 0, drain_fails = 0;  // drain is single-threaded and untimed
    while (Node* p = s->pop(drain_fails)) {
        dsum += p->value;
        drained.push_back(p);
    }
    std::vector<Node*> all;
    bool ok = verify_nodes(results, drained, dsum, 0, all);
    free_nodes(o, results, all, nullptr);
    delete s;
    return ok;
}

template <class Queue>
static bool bench_queue(const Options& o, std::vector<ThreadResult>& results, RunTiming& t) {
    Node* initial_dummy = new Node();  // from `new` in both modes; see free_nodes
    auto* q = new Queue(initial_dummy);
    SharedFlags flags;
    t = run_node_bench(
        o, results, flags, [q](Node* n, std::uint64_t& fails) { q->enqueue(n, fails); },
        [q](std::uint64_t& qsum, std::uint64_t& fails) {
            std::uint64_t v = 0;
            Node* old = q->dequeue(v, fails);
            if (old) qsum += v;
            return old;
        });

    // Drain. Retired/drained nodes are old dummies; the node left at head at
    // the end is the final dummy. Together they are {initial dummy} + every
    // enqueued node, so we expect exactly pushed + 1 distinct nodes.
    std::vector<Node*> drained;
    std::uint64_t dsum = 0, v = 0, drain_fails = 0;  // drain is single-threaded and untimed
    while (Node* old = q->dequeue(v, drain_fails)) {
        dsum += v;
        drained.push_back(old);
    }
    drained.push_back(q->dummy());
    std::vector<Node*> all;
    bool ok = verify_nodes(results, drained, dsum, 1, all);
    free_nodes(o, results, all, initial_dummy);
    delete q;
    return ok;
}

// ----- --layout and --tsc ----------------------------------------------------

template <class S, class F>
static long field_offset(const S& s, const F& f) {
    return static_cast<long>(reinterpret_cast<const char*>(&f) - reinterpret_cast<const char*>(&s));
}

template <class S, class F>
static void print_field(const char* name, const S& s, const char* field, const F& f) {
    std::printf("%-16s sizeof=%zu alignof=%zu %s: offset=%ld size=%zu\n", name, sizeof(S),
                alignof(S), field, field_offset(s, f), sizeof(F));
}

static void print_layout() {
    std::printf("%-16s sizeof=%zu alignof=%zu\n", "std::mutex", sizeof(std::mutex), alignof(std::mutex));
    auto* mc = new MutexCounter();
    auto* sc = new SpinlockCounter();
    auto* fc = new FaaCounter();
    auto* cc = new CasCounter();
    auto* ms = new MutexStack();
    auto* ts = new TreiberStack();
    Node dummy;
    auto* mq = new MutexQueue(&dummy);
    auto* sq = new MSQueue(&dummy);
    print_field("MutexCounter", *mc, "value", mc->value);
    print_field("SpinlockCounter", *sc, "value", sc->value);
    std::printf("%-16s   (lock flag `locked`: offset=%ld size=%zu)\n", "", field_offset(*sc, sc->locked),
                sizeof(sc->locked));
    print_field("FaaCounter", *fc, "value", fc->value);
    print_field("CasCounter", *cc, "value", cc->value);
    print_field("MutexStack", *ms, "head", ms->head);
    print_field("TreiberStack", *ts, "head", ts->head);
    print_field("MutexQueue", *mq, "head", mq->head);
    print_field("MutexQueue", *mq, "tail", mq->tail);
    print_field("MSQueue", *sq, "head", sq->head);
    print_field("MSQueue", *sq, "tail", sq->tail);
    std::printf("%-16s sizeof=%zu alignof=%zu; pool stride %zu bytes (default) or %zu (--pad64)\n",
                "Node", sizeof(Node), alignof(Node), sizeof(Node), kCacheLine);
    std::printf("%-16s sizeof=%zu alignof=%zu\n", "ThreadResult", sizeof(ThreadResult),
                alignof(ThreadResult));
    std::printf("%-16s ceil(K * seconds / threads) + %zuM nodes per thread (pool and retired "
                "array); K = %zuM/s (1 thread), %zuM/s (2), %zuM/s (4+); e.g. 500 ms, 1 thread: "
                "%zu nodes\n",
                "node budget", kExtraNodesPerThread / kMi, node_rate_per_second(1) / kMi,
                node_rate_per_second(2) / kMi, node_rate_per_second(4) / kMi,
                node_budget_per_thread(1, 500));
    delete mc; delete sc; delete fc; delete cc; delete ms; delete ts; delete mq; delete sq;
}

static void print_tsc() {
    int inv = invariant_tsc();
    std::printf("invariant_tsc: %s (CPUID 0x80000007 EDX bit 8)\n",
                inv == 1 ? "yes" : inv == 0 ? "NO" : "unknown (not x86 or leaf unavailable)");
    auto c0 = std::chrono::steady_clock::now();
    std::uint64_t k0 = tsc_now();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::uint64_t k1 = tsc_now();
    auto c1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(c1 - c0).count();
    const double ticks_per_ns = static_cast<double>(k1 - k0) / secs / 1e9;
    std::printf("tsc_frequency: %.1f MHz (TSC ticks vs steady_clock over %.0f ms)\n",
                ticks_per_ns * 1e3, secs * 1e3);

    // Granularity: smallest non-zero difference between back-to-back reads.
    std::uint64_t gran = UINT64_MAX;
    for (int i = 0; i < 1000000; ++i) {
        std::uint64_t a = tsc_now(), b = tsc_now();
        if (b > a) gran = std::min(gran, b - a);
    }
    // Timer overhead: an empty timed region (tsc_begin + tsc_end), as used
    // around every latency sample. Reported, not subtracted from samples.
    std::vector<std::uint64_t> empty(1000000);
    for (auto& e : empty) {
        std::uint64_t a = tsc_begin();
        std::uint64_t b = tsc_end();
        e = b - a;
    }
    std::sort(empty.begin(), empty.end());
    std::printf("tsc_granularity: %llu ticks = %.1f ns (smallest non-zero delta of back-to-back reads)\n",
                static_cast<unsigned long long>(gran), static_cast<double>(gran) / ticks_per_ns);
    std::printf("timer_overhead: empty timed region min %.1f ns, median %.1f ns "
                "(included in every latency sample, not subtracted)\n",
                static_cast<double>(empty.front()) / ticks_per_ns,
                static_cast<double>(empty[empty.size() / 2]) / ticks_per_ns);
    std::printf("latency_sampling: every %lluth iteration per thread (1 in %llu operations timed); "
                "buffer ceil(%zuM / threads) + 1M samples per thread\n",
                static_cast<unsigned long long>(kSampleMask + 1),
                static_cast<unsigned long long>(kSampleMask + 1), kTotalSampleBudget / kMi);

    // Cost of one cpu_pause(), the unit of the backoff wait (best of 5 x 1M).
    double pause_ns = 1e300;
    for (int rep = 0; rep < 5; ++rep) {
        std::uint64_t a = tsc_begin();
        for (int i = 0; i < 1000000; ++i) cpu_pause();
        std::uint64_t b = tsc_end();
        pause_ns = std::min(pause_ns, static_cast<double>(b - a) / ticks_per_ns / 1e6);
    }
    std::printf("cpu_pause: %.1f ns per call (best of 5 x 1M); backoff (*_backoff variants): wait "
                "uniform in [1, limit] pauses, limit 1 -> doubling per failure -> cap %d "
                "(--backoff-max), i.e. at most %.0f ns per wait\n",
                pause_ns, g_backoff_max, pause_ns * g_backoff_max);
}

// ----- main --------------------------------------------------------------------

static int usage() {
    std::fprintf(stderr,
                 "usage: bench <benchmark> <variant> <threads> <millis> [work]"
                 " [--latency] [--heap | --pad64] [--backoff-max N]\n"
                 "       bench --layout | --tsc\n"
                 "  counter: mutex | spinlock | atomic_faa | atomic_cas\n"
                 "           | spinlock_backoff | atomic_cas_backoff\n"
                 "  stack:   mutex | lockfree | lockfree_backoff\n"
                 "  queue:   mutex | lockfree | lockfree_backoff\n"
                 "  --heap and --pad64 apply to stack/queue only\n"
                 "  --backoff-max N (default 256) applies to *_backoff variants only\n");
    return 2;
}

static bool parse_int(const char* s, int lo, int& out) {
    char* end = nullptr;
    long v = std::strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < lo || v > 1000000000L) return false;
    out = static_cast<int>(v);
    return true;
}

// Free physical memory in bytes, or 0 if it cannot be determined.
static std::uint64_t available_ram_bytes() {
#if defined(_WIN32)
    MEMORYSTATUSEX m;
    m.dwLength = sizeof m;
    if (GlobalMemoryStatusEx(&m)) return m.ullAvailPhys;
    return 0;
#else
    // Linux: MemAvailable from /proc/meminfo (kB).
    if (std::FILE* f = std::fopen("/proc/meminfo", "r")) {
        char line[256];
        unsigned long long kb = 0;
        while (std::fgets(line, sizeof line, f))
            if (std::sscanf(line, "MemAvailable: %llu kB", &kb) == 1) break;
        std::fclose(f);
        return static_cast<std::uint64_t>(kb) * 1024;
    }
    return 0;
#endif
}

// Total memory this run pre-allocates (all threads): node pool (or, with
// --heap, the nodes `new` may allocate up to the same budget), the retired
// array and the latency sample buffer.
static std::uint64_t run_budget_bytes(const Options& o, bool node_bench) {
    std::uint64_t per_thread = 0;
    if (node_bench) {
        const std::uint64_t nodes = node_budget_per_thread(o.threads, o.millis);
        const std::uint64_t node_bytes = o.pad64 ? kCacheLine : sizeof(Node);
        per_thread += nodes * (node_bytes + sizeof(Node*));
    }
    if (o.latency) per_thread += sample_budget_per_thread(o.threads) * sizeof(std::uint64_t);
    return per_thread * static_cast<std::uint64_t>(o.threads);
}

// Nearest-rank percentile of sorted samples.
static std::uint64_t percentile(const std::vector<std::uint64_t>& sorted, double q) {
    std::size_t n = sorted.size();
    std::size_t rank = static_cast<std::size_t>(std::ceil(q * static_cast<double>(n)));
    if (rank < 1) rank = 1;
    if (rank > n) rank = n;
    return sorted[rank - 1];
}

int main(int argc, char** argv) {
    std::vector<std::string> pos;
    bool layout = false, tsc = false, backoff_max_set = false;
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--latency") o.latency = true;
        else if (a == "--heap") o.heap = true;
        else if (a == "--pad64") o.pad64 = true;
        else if (a == "--backoff-max") {
            if (i + 1 >= argc || !parse_int(argv[++i], 1, g_backoff_max) || g_backoff_max > 1000000)
                return usage();
            backoff_max_set = true;
        }
        else if (a == "--layout") layout = true;
        else if (a == "--tsc") tsc = true;
        else if (a.rfind("--", 0) == 0) return usage();
        else pos.push_back(a);
    }
    if (layout || tsc) {
        if (!pos.empty()) return usage();
        if (layout) print_layout();
        if (tsc) print_tsc();
        return 0;
    }
    if (pos.size() < 4 || pos.size() > 5) return usage();
    const std::string& benchmark = pos[0];
    const std::string& variant = pos[1];
    if (!parse_int(pos[2].c_str(), 1, o.threads) || !parse_int(pos[3].c_str(), 1, o.millis))
        return usage();
    if (pos.size() == 5 && !parse_int(pos[4].c_str(), 0, o.work)) return usage();
    if (o.heap && o.pad64) return usage();
    if ((o.heap || o.pad64) && benchmark == "counter") return usage();
    const bool backoff_variant =
        variant.size() > 8 && variant.compare(variant.size() - 8, 8, "_backoff") == 0;
    if (backoff_max_set && !backoff_variant) return usage();

    // Free-RAM guard: refuse to start (before anything is allocated or
    // touched) unless free RAM is at least twice this run's budget.
    const bool node_bench = (benchmark == "stack" || benchmark == "queue");
    const std::uint64_t need = run_budget_bytes(o, node_bench);
    if (need > 0) {
        const std::uint64_t avail = available_ram_bytes();
        const double gib = 1024.0 * 1024.0 * 1024.0;
        if (avail == 0) {
            std::fprintf(stderr, "warning: could not determine free RAM; run budget is %.2f GB\n",
                         static_cast<double>(need) / gib);
        } else if (avail < 2 * need) {
            std::fprintf(stderr,
                         "ERROR: not enough free RAM for %s %s %d threads %d ms: the run "
                         "pre-allocates %.2f GB and needs at least twice that (%.2f GB) free, "
                         "but only %.2f GB is free. Close other programs and retry. Nothing was "
                         "run.\n",
                         benchmark.c_str(), variant.c_str(), o.threads, o.millis,
                         static_cast<double>(need) / gib, 2.0 * static_cast<double>(need) / gib,
                         static_cast<double>(avail) / gib);
            return 4;
        }
    }

    std::vector<ThreadResult> results(o.threads);
    RunTiming t;
    bool ok = false;

    if (benchmark == "counter") {
        if (variant == "mutex") ok = bench_counter<MutexCounter>(o, results, t);
        else if (variant == "spinlock") ok = bench_counter<SpinlockCounter>(o, results, t);
        else if (variant == "atomic_faa") ok = bench_counter<FaaCounter>(o, results, t);
        else if (variant == "atomic_cas") ok = bench_counter<CasCounter>(o, results, t);
        else if (variant == "spinlock_backoff")
            ok = bench_counter<SpinlockCounterT<ExpBackoff>>(o, results, t);
        else if (variant == "atomic_cas_backoff")
            ok = bench_counter<CasCounterT<ExpBackoff>>(o, results, t);
        else return usage();
    } else if (benchmark == "stack") {
        if (variant == "mutex") ok = bench_stack<MutexStack>(o, results, t);
        else if (variant == "lockfree") ok = bench_stack<TreiberStack>(o, results, t);
        else if (variant == "lockfree_backoff")
            ok = bench_stack<TreiberStackT<ExpBackoff>>(o, results, t);
        else return usage();
    } else if (benchmark == "queue") {
        if (variant == "mutex") ok = bench_queue<MutexQueue>(o, results, t);
        else if (variant == "lockfree") ok = bench_queue<MSQueue>(o, results, t);
        else if (variant == "lockfree_backoff")
            ok = bench_queue<MSQueueT<ExpBackoff>>(o, results, t);
        else return usage();
    } else {
        return usage();
    }

    // Statistics over per-thread op counts.
    std::uint64_t total = 0, fails = 0, mn = UINT64_MAX, mx = 0;
    bool overflow = false;
    for (auto& r : results) {
        total += r.ops;
        fails += r.fails;
        mn = std::min(mn, r.ops);
        mx = std::max(mx, r.ops);
        overflow = overflow || r.overflow;
    }
    const int threads = o.threads;
    double mean = static_cast<double>(total) / threads;
    double cv = 0.0;
    if (threads > 1 && mean > 0.0) {
        double var = 0.0;
        for (auto& r : results) {
            double d = static_cast<double>(r.ops) - mean;
            var += d * d;
        }
        cv = std::sqrt(var / threads) / mean;  // population std-dev / mean
    }
    double ops_per_sec = t.seconds > 0.0 ? static_cast<double>(total) / t.seconds : 0.0;
    double retries_per_op = total > 0 ? static_cast<double>(fails) / static_cast<double>(total) : 0.0;
    const char* check = !ok ? "MISMATCH" : overflow ? "OVERFLOW" : "ok";

    // Latency columns (empty unless --latency).
    char lat[160] = ",,,,";
    if (o.latency) {
        std::vector<std::uint64_t> all;
        std::size_t n = 0;
        for (auto& r : results) n += r.nsamples;
        all.reserve(n);
        for (auto& r : results) all.insert(all.end(), r.samples.begin(), r.samples.begin() + r.nsamples);
        std::sort(all.begin(), all.end());
        if (!all.empty() && t.ticks > 0) {
            const double ns_per_tick = t.seconds * 1e9 / static_cast<double>(t.ticks);
            auto ns = [&](std::uint64_t ticks) { return static_cast<double>(ticks) * ns_per_tick; };
            std::snprintf(lat, sizeof lat, "%.1f,%.1f,%.1f,%.1f,%zu", ns(percentile(all, 0.50)),
                          ns(percentile(all, 0.99)), ns(percentile(all, 0.999)), ns(all.back()),
                          all.size());
        } else {
            std::snprintf(lat, sizeof lat, ",,,,%zu", all.size());
        }
    }

    std::printf("%s,%s,%d,%d,%d,%llu,%.0f,%llu,%llu,%.4f,%s,%.4f,%s\n", benchmark.c_str(),
                variant.c_str(), threads, o.millis, o.work, static_cast<unsigned long long>(total),
                ops_per_sec, static_cast<unsigned long long>(mn),
                static_cast<unsigned long long>(mx), cv, check, retries_per_op, lat);
    return !ok ? 1 : overflow ? 3 : 0;
}
