#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

class ThreadPoolFullException : public std::runtime_error {
public:
    ThreadPoolFullException() : std::runtime_error("ThreadPool queue is full") {}
};

class ThreadPool {
public:
    ThreadPool(size_t threads, size_t max_queue_size = 256);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Enqueue a task into the thread pool and returns a future that will hold the result of the task.
    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>;

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;

    std::mutex queue_mtx;
    std::condition_variable condition;
    bool stop;
    size_t max_queue_size_;
};

inline ThreadPool::ThreadPool(size_t threads, size_t max_queue_size)
    : stop(false), max_queue_size_(max_queue_size) {
    
    for (size_t i = 0; i < threads; ++i) {
        workers.emplace_back([this] {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(this->queue_mtx);

                    this->condition.wait(lock, [this] {
                        return this->stop || !this->tasks.empty();
                    });

                    if (this->stop && this->tasks.empty()) return;

                    task = std::move(this->tasks.front());
                    this->tasks.pop();
                }
                task();
            }
        });
    }
}

template<class F, class... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args)
    -> std::future<std::invoke_result_t<F, Args...>> {

    using rslt_type = std::invoke_result_t<F, Args...>;

    auto task = std::make_shared<std::packaged_task<rslt_type()>>(
        std::bind(
            std::forward<F>(f),
            std::forward<Args>(args)...
        )
    );

    std::future<rslt_type> rslt = task->get_future();

    {
        std::unique_lock<std::mutex> lock(queue_mtx);

        if (stop) {
            throw std::runtime_error("enqueue on stopped ThreadPool");
        }
        if (max_queue_size_ > 0 && tasks.size() >= max_queue_size_) {
            throw ThreadPoolFullException();
        }

        tasks.emplace([task]() {
            (*task)();
        });
    }

    condition.notify_one();
    return rslt;
}

inline ThreadPool::~ThreadPool() {
    {
        std::unique_lock<std::mutex> lock(queue_mtx);
        stop = true;
    }

    condition.notify_all();

    for (std::thread& worker : workers) {
        worker.join();
    }
}
