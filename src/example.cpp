// The 30-second tour of tp::ThreadPool.
#include "tp/thread_pool.hpp"

#include <cstdio>
#include <numeric>
#include <vector>

int main() {
    tp::ThreadPool pool;  // hardware_concurrency() workers, work stealing on

    // 1. submit -> future
    auto f = pool.submit([](int a, int b) { return a + b; }, 40, 2);
    std::printf("submit:        %d\n", f.get());

    // 2. exceptions travel through the future
    auto g = pool.submit([]() -> int { throw std::runtime_error("nope"); });
    try { (void)g.get(); } catch (const std::exception& e) { std::printf("exception:     %s\n", e.what()); }

    // 3. parallel_for over a range, calling thread helps while it waits
    std::vector<double> v(10'000'000);
    pool.parallel_for(std::size_t{0}, v.size(), [&](std::size_t i) { v[i] = i * 0.5; });
    std::printf("parallel_for:  sum = %.1f\n", std::accumulate(v.begin(), v.end(), 0.0));

    // 4. fire-and-forget + wait_idle
    for (int i = 0; i < 1000; ++i) pool.post([] { /* work */ });
    pool.wait_idle();

    auto s = pool.stats();
    std::printf("stats:         submitted=%llu completed=%llu steals=%llu\n",
                (unsigned long long)s.submitted, (unsigned long long)s.completed, (unsigned long long)s.steals);
    return 0;  // destructor drains and joins
}
