# parallel-thread-pool

> **Sep 2026 (v2):** rebuilt as a header-only work-stealing pool — futures, exception propagation, helping `parallel_for`, backpressure; 16 tests under TSAN/ASAN; benchmarks that show where stealing wins and where it honestly ties.

A header-only C++17 thread pool with **work stealing**, **futures**, **exception propagation**,
**`parallel_for`**, **backpressure**, and **graceful shutdown** — with a benchmark suite that
shows where each design decision pays off *and where it doesn't*, and a test suite that runs
clean under ThreadSanitizer.

[![ci](https://github.com/Abheenash/parallel-thread-pool/actions/workflows/ci.yml/badge.svg)](https://github.com/Abheenash/parallel-thread-pool/actions/workflows/ci.yml)

```cpp
#include "tp/thread_pool.hpp"

tp::ThreadPool pool;                                   // hardware_concurrency() workers

auto f = pool.submit([](int a, int b) { return a + b; }, 40, 2);
f.get();                                               // 42 — exceptions rethrow here too

pool.parallel_for(0, n, [&](int i) { y[i] = a * x[i] + y[i]; });   // blocks until done

pool.post([] { /* fire and forget */ });
pool.wait_idle();                                      // destructor also drains + joins
```

## What's in it

| Feature | How |
| --- | --- |
| `submit(f, args...) → std::future<R>` | `packaged_task` wrapped in a small move-only `Task` (no `shared_ptr` per task) |
| Exceptions | propagate through the future; `post()` counts them in `stats().exceptions` instead of terminating |
| Work stealing | one deque per worker; pop own **front** (cache-hot), steal a random victim's **back** (oldest work) |
| `parallel_for(begin, end, body, grain)` | chunks the range; the **calling thread helps** run tasks while it waits, so nested `parallel_for` from inside a task cannot deadlock |
| `wait_idle()` | blocks until every submitted task has finished |
| Backpressure | `Options::max_pending` makes `submit()` block instead of growing the queue without bound |
| Shutdown | `shutdown()` drains then joins; `shutdown_now()` discards pending; both idempotent; submit-after-shutdown throws |
| Lost-wakeup-free sleeping | submitters only take the wake-up mutex when `sleepers_ > 0`; the Dekker-style `pending++ / read sleepers` vs `sleepers++ / read pending` pairing (both `seq_cst`) guarantees no worker sleeps through a submission |
| Bounded spin before park | a worker scans the queues `spin_before_park` times (default 64) before sleeping on the condvar — parking/waking is a syscall each way, which is far more than a small task costs |
| Single-queue mode | `Options::work_stealing = false` gives the classic one-queue-one-mutex design, so the two can be compared on identical workloads |

## Results — Apple M4 (10 cores: 4 performance + 6 efficiency), clang, `-O3`

Every parallel result is checked against the serial checksum before it's printed. Full table in
[`docs/bench-m4.txt`](docs/bench-m4.txt), CSV in [`results/`](results/).

**Compute-bound scaling** — 64 tasks × 1M `sin·cos`:

| workers | time | speedup |
| --- | --- | --- |
| 1 (serial) | 0.377 s | 1.00× |
| 2 | 0.192 s | 1.96× |
| 4 | 0.104 s | 3.62× |
| 8 | 0.079 s | 4.79× |
| 10 | 0.070 s | **5.37×** |
| `std::async`, 64 OS threads | 0.068 s | 5.57× |

Near-linear across the four performance cores, then diminishing as work lands on efficiency cores.
`std::async` with a thread per task is a hair faster *here* because 64 threads is small — it's the
wrong tool at 1M tasks, which is what the next row measures.

**Scheduler overhead** — 1,000,000 near-empty tasks from one producer:

| mode | throughput |
| --- | --- |
| single queue | 1.51 M tasks/s |
| work stealing | **1.98 M tasks/s** |

The `spin_before_park` change alone took this from 0.60 → 1.98 M tasks/s: without it, workers parked
and were woken by syscall on almost every task. What remains is dominated by the single producer's
per-task heap allocation.

**Fork-join spawn tree** — every task spawns two more *from inside a worker*, 2²⁰ leaves:

| mode | throughput | steals |
| --- | --- | --- |
| single queue | 3.38 M tasks/s | 0 |
| work stealing | **4.24 M tasks/s** | 414,277 |

This is the case work stealing is *for*: a worker's children land on its own deque and stay cache-hot,
and idle workers steal whole subtrees from the back. Every spawn in single-queue mode crosses one lock.

**Heavy-tailed task costs** — 4,000 tasks, 2% of them 100× larger:

| mode | speedup (10 workers) | steals |
| --- | --- | --- |
| single queue | 5.87× | 0 |
| work stealing | 5.80× | 873 |

**No difference — and that's the honest finding.** A global FIFO queue balances a heavy-tailed
*pre-submitted* workload by construction: whichever worker frees up takes the next task. Stealing
earns its keep on contention (tiny tasks) and on worker-spawned work (the tree above), not here.

**`parallel_for`** — same API, two bodies:

| body | 1 thread | 2 | 4 | 8 | 10 |
| --- | --- | --- | --- | --- | --- |
| SAXPY over 50M floats (memory-bound) | 6.5 ms | 1.05× | 1.05× | 1.12× | 1.10× |
| `sin·cos + sqrt` over 8M doubles (compute-bound) | 24.7 ms | 1.94× | 2.58× | 4.40× | **4.98×** |

The SAXPY pass moves ~600 MB in 6.5 ms ≈ 92 GB/s from a *single core* — already near the M4's memory
bandwidth, so more threads can't help. The compute-bound body scales like the thread pool itself.
Same machine, same API, opposite behaviour: whether parallelism pays depends on the body, not the pool.

## Correctness

`tests/test_thread_pool.cpp` — 16 tests covering futures, exception propagation (through `submit`,
`post`, and `parallel_for`), every-index-exactly-once coverage, bitwise-identical `parallel_for`
results, **nested `parallel_for` from inside a task** (deadlocks if waiting doesn't help), submit
from inside a task, drain-on-shutdown, discard-on-`shutdown_now`, submit-after-shutdown, backpressure,
that stealing actually happens under imbalance, single-queue mode, and a 200k-future stress checksum.

CI runs them on Linux and macOS, and again under **ThreadSanitizer** and **AddressSanitizer**
(`-DTP_SANITIZE=thread|address`). TSAN is the real proof that the deque / sleeper / pending
choreography has no data races.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/example
./build/bench            # or: ./build/bench compute|tiny|imbalanced|spawn|pfor
```

Header-only: copy `include/tp/thread_pool.hpp`, or `add_subdirectory` and link `tp::tp`.

## Design notes

- **Why a `Task` type instead of `std::function`** — `std::function` must be copyable, so queuing a
  `packaged_task` through it forces a `shared_ptr` per task. `tp::Task` is a 1-pointer move-only
  wrapper; one allocation per task, not two.
- **Why LIFO-own / FIFO-steal** — the task a worker just pushed is the one whose data is still in
  its cache; the *oldest* task on someone else's deque is the least likely to be touched by its owner
  soon and, in fork-join, the biggest subtree.
- **Why the calling thread helps** — a thread that waits on `parallel_for` inside a worker would
  otherwise hold a worker hostage; with every worker doing that, nothing is left to run the chunks.
  `help_until()` runs tasks until the wait predicate is satisfied.
- **What I'd do next** — a lock-free Chase–Lev deque per worker (the per-deque mutex is the remaining
  contention point), task affinity hints, and priority lanes.
