#include <iostream>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <chrono>
#include <cmath>
#include <string>

// A thread pool: a fixed set of worker threads that pull tasks from a shared queue.
class ThreadPool {
public:
    ThreadPool(int numWorkers) {
        for (int i = 0; i < numWorkers; ++i)
            workers.emplace_back([this] { workerLoop(); });
    }

    // Add a task to the queue and wake one worker.
    void submit(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            tasks.push(std::move(task));
        }
        cv.notify_one();
    }

    // Finish all remaining tasks, then stop the workers.
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            stopping = true;
        }
        cv.notify_all();
        for (std::thread& w : workers) w.join();
    }

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queueMutex;
    std::condition_variable cv;
    bool stopping = false;

    void workerLoop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                // Sleep until there's a task, or we're shutting down.
                cv.wait(lock, [this] { return stopping || !tasks.empty(); });
                if (stopping && tasks.empty()) return;
                task = std::move(tasks.front());
                tasks.pop();
            }
            task();   // run the task with the lock released
        }
    }
};

// Some CPU work, so we can actually measure parallel speedup.
double heavyWork(int seed) {
    double sum = 0.0;
    for (int i = 1; i < 1000000; ++i)
        sum += std::sin(seed + i) * std::cos(i);
    return sum;
}

int main(int argc, char** argv) {
    int nWorkers = (argc > 1) ? std::stoi(argv[1]) : 1;
    const int numTasks = 64;

    std::vector<double> results(numTasks, 0.0);
    ThreadPool pool(nWorkers);

    auto start = std::chrono::high_resolution_clock::now();
    for (int t = 0; t < numTasks; ++t)
        pool.submit([t, &results] { results[t] = heavyWork(t); });
    pool.shutdown();                 // waits for all tasks to finish
    auto end = std::chrono::high_resolution_clock::now();

    double total = 0.0;
    for (double r : results) total += r;

    double secs = std::chrono::duration<double>(end - start).count();
    std::cout << "workers=" << nWorkers
              << "  tasks=" << numTasks
              << "  time=" << secs << " s"
              << "  (checksum " << total << ")\n";
    return 0;
}
