# Parallel Thread Pool (C++)

A thread pool in C++ that runs many tasks concurrently across a fixed set of worker threads. Includes a benchmark that distributes compute-bound tasks across 1–8 threads and measures the speedup.

## How It Works

A thread pool creates a fixed number of worker threads once, up front. The workers stay alive and repeatedly pull tasks off a shared queue, so the program never pays the cost of creating a new thread for every task.

- Tasks are added to a queue with `submit()`.
- A **mutex** protects the queue so two workers never take the same task or corrupt it.
- A **condition variable** lets idle workers sleep until a task is available, then wakes one — no wasted CPU spinning.
- `shutdown()` finishes all remaining tasks, then joins the workers cleanly.

## Build and Run

```bash
clang++ -O2 -std=c++17 threadpool.cpp -o threadpool
./threadpool 4      # the number is how many worker threads to use
```

## Results

Benchmark on a MacBook Air (Apple M4, 2025, 16 GB), 64 compute-bound tasks:

| Workers | Time (s) | Speedup |
|--------:|---------:|--------:|
| 1       | 0.397    | 1.0×    |
| 2       | 0.190    | 2.1×    |
| 4       | 0.101    | 3.9×    |
| 8       | 0.076    | 5.2×    |

The output checksum is identical across all runs, confirming the result is the same regardless of thread count (no data races).

## Scaling Analysis

Speedup is near-linear up to 4 threads and reaches 5.2× at 8 threads, for two reasons:

1. **The tasks are compute-bound.** Each task is heavy arithmetic (sin/cos) with almost no memory traffic, so each core works independently without competing for memory bandwidth. This is why it scales well — in contrast to a memory-bound workload like a stencil simulation, which saturates memory bandwidth and plateaus much earlier on the same hardware.

2. **Performance vs. efficiency cores.** The M4 has four fast performance cores and four slower efficiency cores. Scaling is near-linear across the four performance cores (~3.9× at 4 threads); the efficiency cores add the rest but at a lower rate (5.2× at 8 rather than 8×).

## Future Improvements
- Return task results to the caller using futures (`std::future` / `std::packaged_task`)
- Per-worker queues with work stealing to reduce contention on the shared queue
- Dynamic pool sizing based on load
