#include <chrono>
#include <cstdint>
#include <thread>

#include "cyane/core/time.hpp"
#include "test_framework.hpp"

CYANE_TEST(ticker_counts_ticks_and_paces) {
    cyane::Ticker ticker{100};
    const auto start = cyane::now();
    for (int i = 0; i < 50; ++i) {
        ticker.wait_next();
    }
    const double elapsed_ms = cyane::nanos_to_ms(cyane::elapsed_nanos(start));

    CYANE_CHECK_EQ(ticker.tick(), std::uint64_t{50});
    CYANE_CHECK_EQ(ticker.rate_hz(), 100);
    CYANE_CHECK_NEAR(elapsed_ms, 500.0, 300.0);
}

CYANE_TEST(ticker_records_overruns_when_work_exceeds_interval) {
    cyane::Ticker ticker{1000};
    for (int i = 0; i < 5; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds{4});
        ticker.wait_next();
    }
    CYANE_CHECK(ticker.overruns() >= 4);
    CYANE_CHECK_EQ(ticker.tick(), std::uint64_t{5});
}

CYANE_TEST(ticker_does_not_accumulate_debt_after_overrun) {
    cyane::Ticker ticker{50};
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    for (int i = 0; i < 3; ++i) {
        ticker.wait_next();
    }
    const auto start = cyane::now();
    for (int i = 0; i < 3; ++i) {
        ticker.wait_next();
    }
    const double elapsed_ms = cyane::nanos_to_ms(cyane::elapsed_nanos(start));
    CYANE_CHECK_NEAR(elapsed_ms, 60.0, 45.0);
}

CYANE_TEST(tick_stats_reports_window_averages) {
    cyane::TickStats stats;
    stats.record(1'000'000);
    stats.record(2'000'000);
    stats.record(3'000'000);

    CYANE_CHECK_EQ(stats.total_ticks(), std::uint64_t{3});
    CYANE_CHECK_EQ(stats.tps(), 0.0);

    stats.complete_second();
    CYANE_CHECK_EQ(stats.tps(), 3.0);
    CYANE_CHECK_NEAR(cyane::nanos_to_ms(stats.avg_nanos()), 2.0, 1e-9);
    CYANE_CHECK_NEAR(cyane::nanos_to_ms(stats.max_nanos()), 3.0, 1e-9);
    CYANE_CHECK_EQ(stats.total_ticks(), std::uint64_t{3});
}

CYANE_TEST(tick_stats_resets_window_between_seconds) {
    cyane::TickStats stats;
    stats.record(1'000'000);
    stats.complete_second();
    stats.record(5'000'000);
    stats.record(5'000'000);
    stats.complete_second();

    CYANE_CHECK_EQ(stats.tps(), 2.0);
    CYANE_CHECK_NEAR(cyane::nanos_to_ms(stats.avg_nanos()), 5.0, 1e-9);
    CYANE_CHECK_EQ(stats.total_ticks(), std::uint64_t{3});
}
