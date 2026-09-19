// Benchmarks for tp::ThreadPool. Prints a table and writes results/bench.csv.
//
//   compute     64 compute-bound tasks (sin/cos) — the classic scaling curve
//   tiny        1,000,000 near-empty tasks — pure scheduler overhead, tasks/s
//   imbalanced  4,000 tasks whose cost follows a heavy tail — a global FIFO queue
//               balances this by construction, so this is where stealing does NOT win
//   spawn       a fork-join tree where every task spawns two more from inside a worker
//               (1M leaves) — worker-spawned work stays local; this is where it does
//   pfor        parallel_for: a memory-bound SAXPY (shows the bandwidth wall) and a
//               compute-bound transform (shows near-linear scaling)
//   async       the compute benchmark using std::async per task, for contrast
//
// Every parallel result is checked against the serial checksum before it is printed.
#include "tp/thread_pool.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <future>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using clk = std::chrono::steady_clock;
static double secs(clk::time_point a, clk::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

static double heavy(int seed, int iters = 1'000'000) {
    double s = 0.0;
    for (int i = 1; i < iters; ++i) s += std::sin(seed + i) * std::cos(i);
    return s;
}

struct Row {
    std::string bench, mode;
    int workers;
    double seconds, speedup, extra;
    std::string note;
};
static std::vector<Row> rows;

static void print(const Row& r) {
    std::printf("%-12s %-13s %3d  %9.4f s  %6.2fx  %s\n", r.bench.c_str(), r.mode.c_str(),
                r.workers, r.seconds, r.speedup, r.note.c_str());
    rows.push_back(r);
}

// Median of `reps` runs, so a single scheduler hiccup can't skew a number.
template <class F>
static double timed(F&& f, int reps = 3) {
    std::vector<double> t;
    for (int i = 0; i < reps; ++i) {
        auto a = clk::now();
        f();
        t.push_back(secs(a, clk::now()));
    }
    std::sort(t.begin(), t.end());
    return t[t.size() / 2];
}

static void bench_compute(const std::vector<int>& counts) {
    const int tasks = 64;
    std::vector<double> ref(tasks);
    double serial = timed([&] { for (int t = 0; t < tasks; ++t) ref[t] = heavy(t); });
    double refsum = std::accumulate(ref.begin(), ref.end(), 0.0);
    print({"compute", "serial", 1, serial, 1.0, 0, "64 tasks x 1M sin/cos"});

    for (int w : counts) {
        std::vector<double> out(tasks);
        double t = timed([&] {
            tp::ThreadPool pool(static_cast<std::size_t>(w));
            for (int i = 0; i < tasks; ++i) pool.post([&out, i] { out[i] = heavy(i); });
            pool.wait_idle();
        });
        double sum = std::accumulate(out.begin(), out.end(), 0.0);
        print({"compute", "pool", w, t, serial / t, 0,
               sum == refsum ? "checksum ok" : "CHECKSUM MISMATCH"});
    }
    // std::async, one OS thread per task — what people do instead of a pool.
    {
        std::vector<double> out(tasks);
        double t = timed([&] {
            std::vector<std::future<double>> f;
            for (int i = 0; i < tasks; ++i) f.push_back(std::async(std::launch::async, heavy, i, 1'000'000));
            for (int i = 0; i < tasks; ++i) out[i] = f[i].get();
        });
        double sum = std::accumulate(out.begin(), out.end(), 0.0);
        print({"compute", "std::async", tasks, t, serial / t, 0,
               sum == refsum ? "64 OS threads, checksum ok" : "CHECKSUM MISMATCH"});
    }
}

static void bench_tiny(int workers) {
    const int N = 1'000'000;
    for (bool stealing : {false, true}) {
        std::atomic<long> acc{0};
        double t = timed([&] {
            tp::Options o;
            o.workers = static_cast<std::size_t>(workers);
            o.work_stealing = stealing;
            tp::ThreadPool pool(o);
            acc.store(0);
            for (int i = 0; i < N; ++i) pool.post([&acc, i] { acc.fetch_add(i & 1, std::memory_order_relaxed); });
            pool.wait_idle();
        }, 3);
        char note[96];
        std::snprintf(note, sizeof note, "%.2f M tasks/s, sum=%ld", N / t / 1e6, acc.load());
        print({"tiny", stealing ? "work-stealing" : "single-queue", workers, t, 0, N / t, note});
    }
}

static void bench_imbalanced(int workers) {
    // Heavy-tailed cost: most tasks are cheap, a few are 100x. A single shared queue
    // hands the big ones out in order and lets workers idle; stealing rebalances.
    const int N = 4000;
    std::vector<int> cost(N);
    for (int i = 0; i < N; ++i) cost[i] = (i % 50 == 0) ? 400'000 : 4'000;
    std::vector<double> ref(N);
    double serial = timed([&] { for (int i = 0; i < N; ++i) ref[i] = heavy(i, cost[i]); }, 1);
    double refsum = std::accumulate(ref.begin(), ref.end(), 0.0);
    print({"imbalanced", "serial", 1, serial, 1.0, 0, "4,000 tasks, 2% are 100x"});

    for (bool stealing : {false, true}) {
        std::vector<double> out(N);
        std::uint64_t steals = 0;
        double t = timed([&] {
            tp::Options o;
            o.workers = static_cast<std::size_t>(workers);
            o.work_stealing = stealing;
            tp::ThreadPool pool(o);
            for (int i = 0; i < N; ++i) pool.post([&out, &cost, i] { out[i] = heavy(i, cost[i]); });
            pool.wait_idle();
            steals = pool.stats().steals;
        });
        double sum = std::accumulate(out.begin(), out.end(), 0.0);
        char note[96];
        std::snprintf(note, sizeof note, "%s, %llu steals", sum == refsum ? "checksum ok" : "CHECKSUM MISMATCH",
                      static_cast<unsigned long long>(steals));
        print({"imbalanced", stealing ? "work-stealing" : "single-queue", workers, t, serial / t, 0, note});
    }
}

// Recursive fork-join: each task either does a leaf's work or spawns two children.
// With work stealing, a worker's children land on its own deque (LIFO, cache-hot) and
// idle workers steal the oldest subtree. With a single queue every spawn crosses one lock.
static void spawn_tree(tp::ThreadPool& pool, int depth, std::atomic<long>& acc) {
    if (depth == 0) {
        long s = 0;
        for (int k = 0; k < 200; ++k) s += k % 7;
        acc.fetch_add(s, std::memory_order_relaxed);
        return;
    }
    pool.post([&pool, depth, &acc] { spawn_tree(pool, depth - 1, acc); });
    pool.post([&pool, depth, &acc] { spawn_tree(pool, depth - 1, acc); });
}

static void bench_spawn(int workers) {
    const int depth = 20;  // 2^20 = 1,048,576 leaves
    const long leaves = 1L << depth;
    const long expect = leaves * 594;  // sum_{k<200} k%7 = 28*21 + (0+1+2+3) = 594
    for (bool stealing : {false, true}) {
        std::atomic<long> acc{0};
        std::uint64_t steals = 0;
        double t = timed([&] {
            tp::Options o;
            o.workers = static_cast<std::size_t>(workers);
            o.work_stealing = stealing;
            tp::ThreadPool pool(o);
            acc.store(0);
            pool.post([&pool, &acc] { spawn_tree(pool, depth, acc); });
            pool.wait_idle();
            steals = pool.stats().steals;
        });
        char note[96];
        std::snprintf(note, sizeof note, "%s, %.2f M tasks/s, %llu steals",
                      acc.load() == expect ? "checksum ok" : "CHECKSUM MISMATCH",
                      (2.0 * leaves) / t / 1e6, static_cast<unsigned long long>(steals));
        print({"spawn", stealing ? "work-stealing" : "single-queue", workers, t, 0, 2.0 * leaves / t, note});
    }
}

static void bench_pfor(int workers) {
    const int N = 50'000'000;
    std::vector<float> x(N), y(N);
    for (int i = 0; i < N; ++i) x[i] = static_cast<float>(i % 1000);
    const float a = 2.5f;

    // Time only the SAXPY pass; the reset of y happens outside the window.
    auto median_pass = [&](auto&& pass) {
        std::vector<double> t;
        for (int rep = 0; rep < 5; ++rep) {
            std::fill(y.begin(), y.end(), 1.0f);
            auto s0 = clk::now();
            pass();
            t.push_back(secs(s0, clk::now()));
        }
        std::sort(t.begin(), t.end());
        return t[t.size() / 2];
    };

    double serial = median_pass([&] { for (int i = 0; i < N; ++i) y[i] = a * x[i] + y[i]; });
    double ref = std::accumulate(y.begin(), y.end(), 0.0);
    print({"pfor-saxpy", "serial", 1, serial, 1.0, 0, "50M-element SAXPY, one pass"});

    std::vector<int> ws = {2, 4, 8};
    if (workers > 8) ws.push_back(workers);
    for (int w : ws) {
        tp::ThreadPool pool(static_cast<std::size_t>(w));
        double t = median_pass([&] {
            pool.parallel_for(0, N, [&](int i) { y[i] = a * x[i] + y[i]; }, 1 << 16);
        });
        double got = std::accumulate(y.begin(), y.end(), 0.0);
        print({"pfor-saxpy", "parallel_for", w, t, serial / t, 0,
               got == ref ? "checksum ok (memory-bound)" : "CHECKSUM MISMATCH"});
    }

    // Same shape, compute-bound body: a few transcendental ops per element.
    const int M = 8'000'000;
    std::vector<double> in(M), out(M);
    for (int i = 0; i < M; ++i) in[i] = i * 1e-3;
    auto body = [&](int i) { out[i] = std::sin(in[i]) * std::cos(in[i]) + std::sqrt(in[i] + 1.0); };
    double serial2 = timed([&] { for (int i = 0; i < M; ++i) body(i); }, 3);
    double ref2 = std::accumulate(out.begin(), out.end(), 0.0);
    print({"pfor-compute", "serial", 1, serial2, 1.0, 0, "8M-element sin*cos+sqrt"});
    for (int w : ws) {
        std::fill(out.begin(), out.end(), 0.0);
        tp::ThreadPool pool(static_cast<std::size_t>(w));
        double t = timed([&] { pool.parallel_for(0, M, body, 1 << 14); }, 3);
        double got = std::accumulate(out.begin(), out.end(), 0.0);
        print({"pfor-compute", "parallel_for", w, t, serial2 / t, 0,
               got == ref2 ? "checksum ok (compute-bound)" : "CHECKSUM MISMATCH"});
    }
}

int main(int argc, char** argv) {
    const int hw = static_cast<int>(std::thread::hardware_concurrency());
    std::string only = argc > 1 ? argv[1] : "all";
    std::printf("hardware_concurrency = %d\n\n", hw);
    std::printf("%-12s %-13s %3s  %11s  %7s  %s\n", "bench", "mode", "thr", "time", "speedup", "note");

    std::vector<int> counts = {1, 2, 4, 8};
    if (hw > 8) counts.push_back(hw);

    if (only == "all" || only == "compute") bench_compute(counts);
    if (only == "all" || only == "tiny") bench_tiny(hw);
    if (only == "all" || only == "imbalanced") bench_imbalanced(hw);
    if (only == "all" || only == "spawn") bench_spawn(hw);
    if (only == "all" || only == "pfor") bench_pfor(hw);

    std::ofstream csv("bench.csv");
    csv << "bench,mode,workers,seconds,speedup,extra,note\n";
    for (const auto& r : rows) {
        csv << r.bench << ',' << r.mode << ',' << r.workers << ',' << r.seconds << ',' << r.speedup << ','
            << r.extra << ",\"" << r.note << "\"\n";
    }
    std::printf("\nwrote bench.csv\n");
    return 0;
}
