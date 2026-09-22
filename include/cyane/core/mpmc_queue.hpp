#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace cyane {

inline constexpr std::size_t kCacheLine = 64;

// Vyukov 有界 MPMC 队列：每槽位一个序号，push/pop 各自 CAS 游标，无锁无 ABA
template <typename T>
class MpmcQueue {
    static_assert(std::is_nothrow_move_assignable_v<T>, "T must be nothrow move assignable");

public:
    explicit MpmcQueue(std::size_t min_capacity)
        : mask_{std::bit_ceil(min_capacity) - 1},
          buffer_{std::make_unique<Cell[]>(mask_ + 1)} {
        for (std::size_t i = 0; i <= mask_; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    MpmcQueue(const MpmcQueue&) = delete;
    MpmcQueue& operator=(const MpmcQueue&) = delete;

    [[nodiscard]] bool try_push(T value) noexcept {
        std::size_t pos = enqueue_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& cell = buffer_[pos & mask_];
            const std::size_t seq = cell.sequence.load(std::memory_order_acquire);
            const auto diff = static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos);
            if (diff == 0) {
                if (enqueue_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    cell.value = std::move(value);
                    cell.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;
            } else {
                pos = enqueue_.load(std::memory_order_relaxed);
            }
        }
    }

    [[nodiscard]] bool try_pop(T& out) noexcept {
        std::size_t pos = dequeue_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& cell = buffer_[pos & mask_];
            const std::size_t seq = cell.sequence.load(std::memory_order_acquire);
            const auto diff = static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos + 1);
            if (diff == 0) {
                if (dequeue_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    out = std::move(cell.value);
                    cell.sequence.store(pos + mask_ + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;
            } else {
                pos = dequeue_.load(std::memory_order_relaxed);
            }
        }
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return mask_ + 1; }

    [[nodiscard]] std::size_t size_approx() const noexcept {
        const std::size_t head = enqueue_.load(std::memory_order_relaxed);
        const std::size_t tail = dequeue_.load(std::memory_order_relaxed);
        return head - tail;
    }

private:
    struct Cell {
        std::atomic<std::size_t> sequence{0};
        T value{};
    };

    alignas(kCacheLine) std::atomic<std::size_t> enqueue_{0};
    alignas(kCacheLine) std::atomic<std::size_t> dequeue_{0};
    const std::size_t mask_;
    std::unique_ptr<Cell[]> buffer_;
};

}
