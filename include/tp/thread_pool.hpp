// tp::ThreadPool — a header-only C++17 thread pool with work stealing.
//
//   * submit(f, args...)   -> std::future<R>; exceptions propagate through the future
//   * post(f)              -> fire-and-forget (exceptions are counted, never terminate)
//   * parallel_for(a, b, f)-> splits [a, b) into chunks and runs them on the pool;
//                             the calling thread helps run tasks while it waits
//   * wait_idle()          -> blocks until every submitted task has finished
//   * shutdown()           -> drain the queues, then join; shutdown_now() discards pending
//
// Scheduling: each worker owns a deque protected by its own mutex. A worker pops from
// the FRONT of its own deque (recently pushed work is hot in cache) and, when empty,
// steals from the BACK of a random victim's deque (oldest work — least likely to be
// contended). External submitters round-robin new tasks across worker deques so no
// single queue becomes a serialisation point. Set Options::work_stealing = false to get
// the classic single-queue design instead; the benchmarks compare the two.
//
// Sleeping: a worker that finds nothing to run after scanning every deque parks on a
// condition variable. Submitters only take the wake-up mutex when `sleepers_` says a
// worker is actually parked — the common case (all workers busy) is lock-free on the
// notify side. The Dekker-style pairing (submit: pending++ then read sleepers; worker:
// sleepers++ then read pending, both seq_cst) guarantees no wake-up is ever lost.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace tp {

// A minimal move-only callable so packaged_task (move-only) can be queued
// without wrapping it in a shared_ptr.
class Task {
public:
    Task() = default;
    template <class F, class = std::enable_if_t<!std::is_same_v<std::decay_t<F>, Task>>>
    Task(F&& f) : impl_(std::make_unique<Model<std::decay_t<F>>>(std::forward<F>(f))) {}
    Task(Task&&) noexcept = default;
    Task& operator=(Task&&) noexcept = default;
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    explicit operator bool() const noexcept { return static_cast<bool>(impl_); }
    void operator()() { impl_->call(); }

private:
    struct Concept {
        virtual ~Concept() = default;
        virtual void call() = 0;
    };
    template <class F>
    struct Model final : Concept {
        // Parameter named fn, not f: GCC's -Wshadow flags a constructor
        // parameter shadowing a member even in the safe `f(std::move(f))`
        // idiom, and clang does not. Renaming satisfies both and reads better
        // than suppressing the warning.
        explicit Model(F&& fn) : f(std::move(fn)) {}
        explicit Model(const F& fn) : f(fn) {}
        void call() override { f(); }
        F f;
    };
    std::unique_ptr<Concept> impl_;
};

struct Options {
    std::size_t workers = 0;          // 0 -> std::thread::hardware_concurrency()
    bool work_stealing = true;        // false -> single shared queue
    std::size_t max_pending = 0;      // 0 -> unbounded; otherwise submit() blocks (backpressure)
    unsigned spin_before_park = 64;   // empty-queue scans before a worker sleeps on the condvar
};

struct Stats {
    std::uint64_t submitted = 0;
    std::uint64_t completed = 0;
    std::uint64_t steals = 0;         // tasks a worker took from another worker's deque
    std::uint64_t exceptions = 0;     // exceptions thrown by post()-ed tasks
};

class ThreadPool {
public:
    explicit ThreadPool(Options opts = {}) : opts_(opts) {
        if (opts_.workers == 0) {
            opts_.workers = std::max(1u, std::thread::hardware_concurrency());
        }
        queues_.reserve(opts_.workers);
        for (std::size_t i = 0; i < opts_.workers; ++i) {
            queues_.emplace_back(std::make_unique<Queue>());
        }
        workers_.reserve(opts_.workers);
        for (std::size_t i = 0; i < opts_.workers; ++i) {
            workers_.emplace_back([this, i] { worker_loop(i); });
        }
    }
    explicit ThreadPool(std::size_t workers) : ThreadPool(Options{workers, true, 0, 64}) {}

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    ~ThreadPool() { shutdown(); }

    std::size_t size() const noexcept { return opts_.workers; }

    // Run f(args...) on the pool. The returned future yields the result or rethrows.
    template <class F, class... Args>
    auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
        using R = std::invoke_result_t<F, Args...>;
        std::packaged_task<R()> pt(
            [fn = std::forward<F>(f), tup = std::make_tuple(std::forward<Args>(args)...)]() mutable {
                return std::apply(std::move(fn), std::move(tup));
            });
        std::future<R> fut = pt.get_future();
        enqueue(Task(std::move(pt)));
        return fut;
    }

    // Fire-and-forget. An exception escaping f is counted in stats().exceptions.
    template <class F>
    void post(F&& f) {
        enqueue(Task([this, fn = std::forward<F>(f)]() mutable {
            try {
                fn();
            } catch (...) {
                exceptions_.fetch_add(1, std::memory_order_relaxed);
            }
        }));
    }

    // Run body(i) for every i in [begin, end), chunked across the pool. Blocks until
    // done. If called from inside a worker, that worker keeps executing tasks while
    // it waits, so nested parallel_for cannot deadlock the pool.
    template <class Index, class F>
    void parallel_for(Index begin, Index end, F&& body, std::size_t grain = 0) {
        if (end <= begin) return;
        const std::size_t n = static_cast<std::size_t>(end - begin);
        if (grain == 0) grain = std::max<std::size_t>(1, n / (opts_.workers * 4));
        const std::size_t chunks = (n + grain - 1) / grain;

        auto state = std::make_shared<ForState>();
        state->remaining.store(chunks, std::memory_order_relaxed);
        auto fn = std::forward<F>(body);

        for (std::size_t c = 0; c < chunks; ++c) {
            const Index lo = begin + static_cast<Index>(c * grain);
            const Index hi = std::min(end, begin + static_cast<Index>((c + 1) * grain));
            enqueue(Task([state, lo, hi, &fn] {
                try {
                    for (Index i = lo; i < hi; ++i) fn(i);
                } catch (...) {
                    std::lock_guard<std::mutex> lk(state->m);
                    if (!state->error) state->error = std::current_exception();
                }
                if (state->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    std::lock_guard<std::mutex> lk(state->m);
                    state->cv.notify_all();
                }
            }));
        }
        help_until([&] { return state->remaining.load(std::memory_order_acquire) == 0; },
                   state->m, state->cv);
        if (state->error) std::rethrow_exception(state->error);
    }

    // Block until every task submitted so far has completed.
    void wait_idle() {
        help_until([&] { return pending_.load(std::memory_order_acquire) == 0; }, idle_m_, idle_cv_);
    }

    // Finish everything that was submitted, then stop the workers. Idempotent.
    void shutdown() { stop(false); }
    // Stop as soon as running tasks finish; pending tasks are discarded. Idempotent.
    void shutdown_now() { stop(true); }

    Stats stats() const {
        Stats s;
        s.submitted = submitted_.load(std::memory_order_relaxed);
        s.completed = completed_.load(std::memory_order_acquire);
        s.steals = steals_.load(std::memory_order_relaxed);
        s.exceptions = exceptions_.load(std::memory_order_relaxed);
        return s;
    }

    // True when the calling thread is one of this pool's workers.
    bool on_worker_thread() const noexcept { return tls_pool() == this; }

private:
    struct Queue {
        std::mutex m;
        std::deque<Task> d;
    };
    struct ForState {
        std::atomic<std::size_t> remaining{0};
        std::mutex m;
        std::condition_variable cv;
        std::exception_ptr error;
    };

    static ThreadPool*& tls_pool() noexcept {
        thread_local ThreadPool* p = nullptr;
        return p;
    }
    static std::size_t& tls_index() noexcept {
        thread_local std::size_t i = 0;
        return i;
    }

    void enqueue(Task t) {
        if (opts_.max_pending) {
            std::unique_lock<std::mutex> lk(cv_m_);
            backpressure_cv_.wait(lk, [&] {
                return stopping_.load(std::memory_order_acquire) ||
                       pending_.load(std::memory_order_acquire) < opts_.max_pending;
            });
        }
        if (stopping_.load(std::memory_order_acquire)) {
            throw std::runtime_error("tp::ThreadPool: submit after shutdown");
        }
        submitted_.fetch_add(1, std::memory_order_relaxed);
        pending_.fetch_add(1, std::memory_order_seq_cst);

        std::size_t target = 0;
        if (opts_.work_stealing) {
            // A worker submitting keeps the task local (hot cache, no steal needed);
            // external threads round-robin so no deque becomes a hot spot.
            target = on_worker_thread() ? tls_index()
                                        : next_.fetch_add(1, std::memory_order_relaxed) % opts_.workers;
        }
        {
            std::lock_guard<std::mutex> lk(queues_[target]->m);
            queues_[target]->d.push_back(std::move(t));
        }
        if (sleepers_.load(std::memory_order_seq_cst) > 0) {
            std::lock_guard<std::mutex> lk(cv_m_);
            cv_.notify_one();
        }
    }

    bool pop_own(std::size_t idx, Task& out) {
        Queue& q = *queues_[idx];
        std::lock_guard<std::mutex> lk(q.m);
        if (q.d.empty()) return false;
        out = std::move(q.d.front());
        q.d.pop_front();
        return true;
    }

    bool steal(std::size_t self, Task& out) {
        if (!opts_.work_stealing) return false;
        const std::size_t n = opts_.workers;
        // Start at a random victim so all thieves don't hammer the same deque.
        std::size_t start = rng_index(self);
        for (std::size_t k = 0; k < n; ++k) {
            const std::size_t v = (start + k) % n;
            if (v == self) continue;
            Queue& q = *queues_[v];
            std::lock_guard<std::mutex> lk(q.m);
            if (q.d.empty()) continue;
            out = std::move(q.d.back());
            q.d.pop_back();
            steals_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    // Take any task from any queue (used by threads that "help" while waiting).
    bool take_any(Task& out) {
        if (!opts_.work_stealing) return pop_own(0, out);
        const std::size_t n = opts_.workers;
        std::size_t start = on_worker_thread() ? tls_index() : 0;
        for (std::size_t k = 0; k < n; ++k) {
            if (pop_own((start + k) % n, out)) return true;
        }
        return false;
    }

    std::size_t rng_index(std::size_t seed) {
        thread_local std::minstd_rand rng(static_cast<unsigned>(seed * 2654435761u + 1));
        return static_cast<std::size_t>(rng()) % opts_.workers;
    }

    void run(Task& t) {
        t();
        // Count the completion BEFORE releasing the pending count: wait_idle() returns
        // the moment pending_ hits zero, and stats().completed must already be final
        // then (CI caught the other order as an off-by-one on a fast Linux runner).
        completed_.fetch_add(1, std::memory_order_release);
        if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::lock_guard<std::mutex> lk(idle_m_);
            idle_cv_.notify_all();
        }
        if (opts_.max_pending) {
            std::lock_guard<std::mutex> lk(cv_m_);
            backpressure_cv_.notify_one();
        }
    }

    void worker_loop(std::size_t idx) {
        tls_pool() = this;
        tls_index() = idx;
        const std::size_t own = opts_.work_stealing ? idx : 0;
        Task t;
        unsigned spins = 0;
        while (true) {
            if (pop_own(own, t) || steal(idx, t)) {
                run(t);
                spins = 0;
                continue;
            }
            // Parking and waking a thread costs a syscall each way — far more than a
            // tiny task. Spin a bounded number of times first so bursts of small tasks
            // never pay it; then sleep for real so an idle pool burns no CPU.
            if (spins++ < opts_.spin_before_park) {
                std::this_thread::yield();
                continue;
            }
            spins = 0;
            std::unique_lock<std::mutex> lk(cv_m_);
            sleepers_.fetch_add(1, std::memory_order_seq_cst);
            cv_.wait(lk, [&] {
                return stopping_.load(std::memory_order_acquire) ||
                       pending_.load(std::memory_order_seq_cst) > 0;
            });
            sleepers_.fetch_sub(1, std::memory_order_seq_cst);
            if (stopping_.load(std::memory_order_acquire)) {
                if (discard_.load(std::memory_order_acquire) ||
                    pending_.load(std::memory_order_acquire) == 0) {
                    return;
                }
            }
        }
    }

    // Wait for `done()`; a worker thread keeps running tasks in the meantime.
    template <class Pred>
    void help_until(Pred done, std::mutex& m, std::condition_variable& cv) {
        if (on_worker_thread()) {
            Task t;
            while (!done()) {
                if (take_any(t)) run(t);
                else std::this_thread::yield();
            }
            return;
        }
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, done);
    }

    void stop(bool discard) {
        bool expected = false;
        if (!stopping_.compare_exchange_strong(expected, true)) return;  // already stopping
        discard_.store(discard, std::memory_order_release);
        if (discard) {
            std::size_t dropped = 0;
            for (auto& q : queues_) {
                std::lock_guard<std::mutex> lk(q->m);
                dropped += q->d.size();
                q->d.clear();
            }
            if (dropped) pending_.fetch_sub(dropped, std::memory_order_acq_rel);
        }
        {
            std::lock_guard<std::mutex> lk(cv_m_);
            cv_.notify_all();
            backpressure_cv_.notify_all();
        }
        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
        {
            std::lock_guard<std::mutex> lk(idle_m_);
            idle_cv_.notify_all();
        }
    }

    Options opts_;
    std::vector<std::unique_ptr<Queue>> queues_;
    std::vector<std::thread> workers_;

    std::mutex cv_m_;
    std::condition_variable cv_;
    std::condition_variable backpressure_cv_;
    std::mutex idle_m_;
    std::condition_variable idle_cv_;

    std::atomic<std::size_t> pending_{0};
    std::atomic<std::size_t> sleepers_{0};
    std::atomic<std::size_t> next_{0};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> discard_{false};
    std::atomic<std::uint64_t> submitted_{0};
    std::atomic<std::uint64_t> completed_{0};
    std::atomic<std::uint64_t> steals_{0};
    std::atomic<std::uint64_t> exceptions_{0};
};

}  // namespace tp
