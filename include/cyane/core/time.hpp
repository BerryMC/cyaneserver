#pragma once

#include <chrono>
#include <cstdint>
#include <thread>

namespace cyane {

using Clock = std::chrono::steady_clock;

[[nodiscard]] inline Clock::time_point now() noexcept { return Clock::now(); }

[[nodiscard]] inline std::uint64_t elapsed_nanos(Clock::time_point since) noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - since).count());
}

// 单调毫秒时间戳（steady_clock 纪元）。跨线程比较"何时到期"统一用它。
[[nodiscard]] inline std::uint64_t now_ms() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
            .count());
}

[[nodiscard]] constexpr double nanos_to_ms(std::uint64_t nanos) noexcept {
    return static_cast<double>(nanos) * 1e-6;
}

// 固定频率节拍器：过载时重同步而不是累加欠账，避免死亡螺旋
class Ticker {
public:
    explicit Ticker(int rate_hz) noexcept
        : interval_{std::chrono::nanoseconds{1'000'000'000} / rate_hz},
          next_{Clock::now() + interval_},
          rate_hz_{rate_hz} {}

    void wait_next() noexcept {
        ++tick_;
        const auto current = Clock::now();
        if (next_ > current) {
            std::this_thread::sleep_until(next_);
            next_ += interval_;
        } else {
            ++overruns_;
            next_ = current + interval_;
        }
    }

    [[nodiscard]] std::uint64_t tick() const noexcept { return tick_; }
    [[nodiscard]] std::uint64_t overruns() const noexcept { return overruns_; }
    [[nodiscard]] int rate_hz() const noexcept { return rate_hz_; }

private:
    std::chrono::nanoseconds interval_;
    Clock::time_point next_;
    std::uint64_t tick_{0};
    std::uint64_t overruns_{0};
    int rate_hz_;
};

// 1 秒窗口的 TPS 与耗时统计，complete_second() 结算并滚窗
class TickStats {
public:
    void record(std::uint64_t tick_nanos) noexcept {
        window_nanos_ += tick_nanos;
        window_count_ += 1;
        window_max_ = tick_nanos > window_max_ ? tick_nanos : window_max_;
    }

    void complete_second() noexcept {
        last_window_count_ = window_count_;
        last_window_nanos_ = window_nanos_;
        last_window_max_ = window_max_;
        window_count_ = 0;
        window_nanos_ = 0;
        window_max_ = 0;
        total_ += last_window_count_;
    }

    [[nodiscard]] double tps() const noexcept { return static_cast<double>(last_window_count_); }
    [[nodiscard]] std::uint64_t avg_nanos() const noexcept {
        return last_window_count_ == 0 ? 0 : last_window_nanos_ / last_window_count_;
    }
    [[nodiscard]] std::uint64_t max_nanos() const noexcept { return last_window_max_; }
    [[nodiscard]] std::uint64_t total_ticks() const noexcept { return total_ + window_count_; }

private:
    std::uint64_t window_nanos_{0};
    std::uint64_t window_max_{0};
    std::uint64_t window_count_{0};
    std::uint64_t total_{0};
    std::uint64_t last_window_count_{0};
    std::uint64_t last_window_nanos_{0};
    std::uint64_t last_window_max_{0};
};

}
