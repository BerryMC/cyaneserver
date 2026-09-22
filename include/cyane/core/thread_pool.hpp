#pragma once

#include <atomic>
#include <cstdint>
#include <semaphore>
#include <string>
#include <thread>
#include <vector>

#include "cyane/core/inline_function.hpp"
#include "cyane/core/mpmc_queue.hpp"

namespace cyane {

class ThreadPool {
public:
    using Task = InlineFunction<void()>;

    struct Stats {
        std::uint64_t executed{0};
        std::uint64_t rejected{0};
        std::uint64_t busy_nanos{0};
    };

    ThreadPool(std::string name, unsigned threads, std::size_t queue_capacity = 4096);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    [[nodiscard]] bool try_submit(Task task) noexcept {
        if (!queue_.try_push(std::move(task))) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        wake_.release();
        return true;
    }

    void wait_idle() const noexcept;
    void shutdown() noexcept;

    [[nodiscard]] Stats stats() const noexcept {
        return Stats{executed_.load(std::memory_order_relaxed),
                     rejected_.load(std::memory_order_relaxed),
                     busy_nanos_.load(std::memory_order_relaxed)};
    }

    [[nodiscard]] unsigned threads() const noexcept { return static_cast<unsigned>(workers_.size()); }

private:
    void worker_loop() noexcept;

    std::string name_;
    MpmcQueue<Task> queue_;
    std::counting_semaphore<> wake_{0};
    std::vector<std::jthread> workers_;
    std::atomic<std::uint64_t> executed_{0};
    std::atomic<std::uint64_t> rejected_{0};
    std::atomic<std::uint64_t> busy_nanos_{0};
    std::atomic<std::uint32_t> active_{0};
    std::atomic<bool> running_{true};
};

}
