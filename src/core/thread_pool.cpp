#include "cyane/core/thread_pool.hpp"

#include <format>

#include "cyane/core/log.hpp"
#include "cyane/core/time.hpp"

namespace cyane {

ThreadPool::ThreadPool(std::string name, unsigned threads, std::size_t queue_capacity)
    : name_{std::move(name)}, queue_{queue_capacity} {
    const unsigned count = threads == 0 ? 1 : threads;
    workers_.reserve(count);
    for (unsigned index = 0; index < count; ++index) {
        workers_.emplace_back([this, index] {
            log::set_thread_name(std::format("{}{}", name_, index));
            worker_loop();
        });
    }
}

ThreadPool::~ThreadPool() { shutdown(); }

void ThreadPool::worker_loop() noexcept {
    Task task;
    while (running_.load(std::memory_order_acquire)) {
        active_.fetch_add(1, std::memory_order_acq_rel);
        if (queue_.try_pop(task)) {
            const auto start = now();
            try {
                task();
            } catch (const std::exception& error) {
                log::error("task failed: {}", error.what());
            } catch (...) {
                log::error("task failed with unknown exception");
            }
            busy_nanos_.fetch_add(elapsed_nanos(start), std::memory_order_relaxed);
            executed_.fetch_add(1, std::memory_order_relaxed);
            active_.fetch_sub(1, std::memory_order_acq_rel);
            continue;
        }
        active_.fetch_sub(1, std::memory_order_acq_rel);
        wake_.acquire();
    }
}

void ThreadPool::wait_idle() const noexcept {
    while (queue_.size_approx() != 0 || active_.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
    }
}

void ThreadPool::shutdown() noexcept {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    for (unsigned index = 0; index < threads(); ++index) {
        wake_.release();
    }
    workers_.clear();
}

}
