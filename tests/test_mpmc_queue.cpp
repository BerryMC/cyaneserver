#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cyane/core/mpmc_queue.hpp"
#include "test_framework.hpp"

CYANE_TEST(mpmc_queue_preserves_fifo_order) {
    cyane::MpmcQueue<int> queue{16};
    for (int value = 0; value < 16; ++value) {
        CYANE_CHECK(queue.try_push(value));
    }
    for (int expected = 0; expected < 16; ++expected) {
        int value = -1;
        CYANE_CHECK(queue.try_pop(value));
        CYANE_CHECK_EQ(value, expected);
    }
    int extra = 0;
    CYANE_CHECK(!queue.try_pop(extra));
}

CYANE_TEST(mpmc_queue_rejects_when_full) {
    cyane::MpmcQueue<int> queue{8};
    CYANE_CHECK_EQ(queue.capacity(), std::size_t{8});
    for (int value = 0; value < 8; ++value) {
        CYANE_CHECK(queue.try_push(value));
    }
    CYANE_CHECK(!queue.try_push(99));
}

CYANE_TEST(mpmc_queue_wraps_around_capacity) {
    cyane::MpmcQueue<int> queue{4};
    int value = 0;
    for (int round = 0; round < 1000; ++round) {
        CYANE_CHECK(queue.try_push(round));
        CYANE_CHECK(queue.try_pop(value));
        CYANE_CHECK_EQ(value, round);
    }
    CYANE_CHECK_EQ(queue.size_approx(), std::size_t{0});
}

CYANE_TEST(mpmc_queue_rounds_capacity_up_to_power_of_two) {
    cyane::MpmcQueue<int> queue{5};
    CYANE_CHECK_EQ(queue.capacity(), std::size_t{8});
}

CYANE_TEST(mpmc_queue_moves_values) {
    cyane::MpmcQueue<std::string> queue{4};
    std::string payload{"cyane"};
    CYANE_CHECK(queue.try_push(std::move(payload)));
    std::string out;
    CYANE_CHECK(queue.try_pop(out));
    CYANE_CHECK_EQ(out, std::string{"cyane"});
}

CYANE_TEST(mpmc_queue_survives_concurrent_producers_and_consumers) {
    constexpr int kProducers = 4;
    constexpr int kConsumers = 4;
    constexpr int kPerProducer = 50'000;
    constexpr std::uint64_t kTotal = static_cast<std::uint64_t>(kProducers) * kPerProducer;
    constexpr std::uint64_t kStride = 1'000'000;

    cyane::MpmcQueue<std::uint64_t> queue{1024};
    std::atomic<std::uint64_t> produced{0};
    std::atomic<std::uint64_t> consumed{0};
    std::atomic<std::uint64_t> checksum{0};

    std::vector<std::jthread> threads;
    threads.reserve(kProducers + kConsumers);

    for (int producer = 0; producer < kProducers; ++producer) {
        threads.emplace_back([&queue, &produced, producer] {
            for (int sequence = 0; sequence < kPerProducer; ++sequence) {
                const auto value = static_cast<std::uint64_t>(producer) * kStride +
                                   static_cast<std::uint64_t>(sequence);
                while (!queue.try_push(value)) {
                    std::this_thread::yield();
                }
                produced.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (int consumer = 0; consumer < kConsumers; ++consumer) {
        threads.emplace_back([&queue, &consumed, &checksum] {
            std::uint64_t value = 0;
            for (;;) {
                if (queue.try_pop(value)) {
                    checksum.fetch_add(value, std::memory_order_relaxed);
                    const auto done = consumed.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (done >= kTotal) {
                        return;
                    }
                } else if (consumed.load(std::memory_order_relaxed) >= kTotal) {
                    return;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    threads.clear();

    CYANE_CHECK_EQ(produced.load(), kTotal);
    CYANE_CHECK_EQ(consumed.load(), kTotal);

    std::uint64_t expected = 0;
    for (int producer = 0; producer < kProducers; ++producer) {
        for (int sequence = 0; sequence < kPerProducer; ++sequence) {
            expected += static_cast<std::uint64_t>(producer) * kStride + static_cast<std::uint64_t>(sequence);
        }
    }
    CYANE_CHECK_EQ(checksum.load(), expected);
    CYANE_CHECK_EQ(queue.size_approx(), std::size_t{0});
}
