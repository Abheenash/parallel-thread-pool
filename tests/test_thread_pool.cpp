// Assert-style tests for tp::ThreadPool. Run under ctest; CI also runs them with
// -fsanitize=thread, which is the real proof that the scheduler has no data races.
#include "tp/thread_pool.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond, __FILE__, __LINE__); \
            std::exit(1);                                                                    \
        }                                                                                    \
    } while (0)

static int tests_run = 0;
#define TEST(name)                       \
    static void name();                  \
    struct name##_reg {                  \
        name##_reg() { ++tests_run; name(); std::printf("ok  %s\n", #name); } \
    } name##_inst;                       \
    static void name()

TEST(submit_returns_result_through_future) {
    tp::ThreadPool pool(4);
    auto f = pool.submit([](int a, int b) { return a * b; }, 6, 7);
    CHECK(f.get() == 42);
    auto g = pool.submit([] { return std::string("hello"); });
    CHECK(g.get() == "hello");
}

TEST(exceptions_propagate_through_future) {
    tp::ThreadPool pool(2);
    auto f = pool.submit([]() -> int { throw std::runtime_error("boom"); });
    bool threw = false;
    try {
        (void)f.get();
    } catch (const std::runtime_error& e) {
        threw = std::string(e.what()) == "boom";
    }
    CHECK(threw);
    // The pool must still be healthy afterwards.
    CHECK(pool.submit([] { return 1; }).get() == 1);
}

TEST(post_counts_exceptions_instead_of_terminating) {
    tp::ThreadPool pool(2);
    pool.post([] { throw 42; });
    pool.post([] { throw std::runtime_error("x"); });
    pool.wait_idle();
    CHECK(pool.stats().exceptions == 2);
}

TEST(wait_idle_waits_for_everything) {
    tp::ThreadPool pool(8);
    std::atomic<int> n{0};
    for (int i = 0; i < 10000; ++i) pool.post([&] { n.fetch_add(1, std::memory_order_relaxed); });
    pool.wait_idle();
    CHECK(n.load() == 10000);
    CHECK(pool.stats().completed == 10000);
}

TEST(parallel_for_covers_every_index_exactly_once) {
    tp::ThreadPool pool(6);
    const std::size_t N = 1'000'003;
    std::vector<std::atomic<int>> hits(N);
    for (auto& h : hits) h.store(0);
    pool.parallel_for(std::size_t{0}, N, [&](std::size_t i) { hits[i].fetch_add(1); });
    for (std::size_t i = 0; i < N; ++i) CHECK(hits[i].load() == 1);
}

TEST(parallel_for_matches_serial_result) {
    tp::ThreadPool pool(8);
    const int N = 2'000'000;
    std::vector<double> a(N), b(N), out(N);
    for (int i = 0; i < N; ++i) { a[i] = i * 0.5; b[i] = 1.0 / (i + 1); }
    pool.parallel_for(0, N, [&](int i) { out[i] = 2.0 * a[i] + b[i]; }, 4096);
    double serial = 0, par = 0;
    for (int i = 0; i < N; ++i) { serial += 2.0 * a[i] + b[i]; par += out[i]; }
    CHECK(serial == par);  // bitwise: same operations, same order per element
}

TEST(parallel_for_propagates_exception) {
    tp::ThreadPool pool(4);
    bool threw = false;
    try {
        pool.parallel_for(0, 1000, [](int i) { if (i == 777) throw std::logic_error("777"); });
    } catch (const std::logic_error&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(pool.submit([] { return 5; }).get() == 5);
}

TEST(nested_parallel_for_from_inside_a_task_does_not_deadlock) {
    // Every worker blocks in a nested parallel_for; if waiting didn't "help", the
    // pool would deadlock with all workers parked and the inner chunks never run.
    tp::ThreadPool pool(2);
    std::atomic<long> total{0};
    pool.parallel_for(0, 64, [&](int) {
        pool.parallel_for(0, 1000, [&](int j) { total.fetch_add(j, std::memory_order_relaxed); }, 10);
    }, 1);
    CHECK(total.load() == 64L * (999L * 1000L / 2));
}

TEST(submit_from_inside_a_task_works) {
    tp::ThreadPool pool(3);
    auto outer = pool.submit([&] {
        auto inner = pool.submit([] { return 21; });
        return inner.get() * 2;
    });
    CHECK(outer.get() == 42);
}

TEST(shutdown_drains_pending_tasks) {
    std::atomic<int> n{0};
    {
        tp::ThreadPool pool(1);
        for (int i = 0; i < 500; ++i) {
            pool.post([&] {
                std::this_thread::sleep_for(std::chrono::microseconds(20));
                n.fetch_add(1);
            });
        }
        pool.shutdown();  // must run all 500 before returning
    }
    CHECK(n.load() == 500);
}

TEST(shutdown_now_discards_pending_tasks) {
    std::atomic<int> n{0};
    tp::ThreadPool pool(1);
    pool.post([&] { std::this_thread::sleep_for(std::chrono::milliseconds(50)); n.fetch_add(1); });
    for (int i = 0; i < 1000; ++i) pool.post([&] { n.fetch_add(1); });
    pool.shutdown_now();
    CHECK(n.load() < 1001);      // most were discarded
    CHECK(pool.stats().completed == static_cast<std::uint64_t>(n.load()));
}

TEST(submit_after_shutdown_throws) {
    tp::ThreadPool pool(1);
    pool.shutdown();
    bool threw = false;
    try { pool.submit([] {}); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

TEST(backpressure_bounds_pending_tasks) {
    tp::Options o;
    o.workers = 2;
    o.max_pending = 8;
    tp::ThreadPool pool(o);
    std::atomic<int> n{0};
    for (int i = 0; i < 200; ++i) {
        pool.post([&] { std::this_thread::sleep_for(std::chrono::microseconds(50)); n.fetch_add(1); });
    }
    pool.wait_idle();
    CHECK(n.load() == 200);
}

TEST(work_stealing_actually_happens_under_imbalance) {
    tp::ThreadPool pool(4);
    // One deque gets all the heavy work via round-robin position; the others steal.
    std::atomic<long> sink{0};
    for (int i = 0; i < 400; ++i) {
        pool.post([&, i] {
            long s = 0;
            const int iters = (i % 4 == 0) ? 200000 : 100;
            for (int k = 0; k < iters; ++k) s += k % 7;
            sink.fetch_add(s, std::memory_order_relaxed);
        });
    }
    pool.wait_idle();
    CHECK(pool.stats().steals > 0);
    CHECK(pool.stats().completed == 400);
}

TEST(single_queue_mode_works_too) {
    tp::Options o;
    o.workers = 4;
    o.work_stealing = false;
    tp::ThreadPool pool(o);
    std::atomic<int> n{0};
    for (int i = 0; i < 5000; ++i) pool.post([&] { n.fetch_add(1); });
    pool.wait_idle();
    CHECK(n.load() == 5000);
    CHECK(pool.stats().steals == 0);
}

TEST(stress_many_small_tasks_checksum) {
    tp::ThreadPool pool;
    const int N = 200000;
    std::vector<std::future<long>> futs;
    futs.reserve(N);
    for (int i = 0; i < N; ++i) futs.push_back(pool.submit([i] { return static_cast<long>(i) * 3; }));
    long sum = 0;
    for (auto& f : futs) sum += f.get();
    CHECK(sum == 3L * (static_cast<long>(N) - 1) * N / 2);
}

int main() {
    std::printf("%d tests passed\n", tests_run);
    return 0;
}
