#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

namespace cyane::world {

enum class EnumBorderStatus {
    Stationary,
    Growing,
    Shrinking
};

class WorldBorder;

class IBorderListener {
public:
    virtual ~IBorderListener() = default;
    virtual void on_size_changed(const WorldBorder& border, double new_size) = 0;
    virtual void on_transition_started(const WorldBorder& border, double old_size, double new_size, std::int64_t time) = 0;
    virtual void on_center_changed(const WorldBorder& border, double x, double z) = 0;
    virtual void on_warning_time_changed(const WorldBorder& border, int new_time) = 0;
    virtual void on_warning_distance_changed(const WorldBorder& border, int new_dist) = 0;
};

// 1.12.2 原版世界边界系统（WorldBorder 逐行转写）
class WorldBorder {
public:
    WorldBorder()
        : center_x_(0.0),
          center_z_(0.0),
          start_diameter_(6.0e7),
          end_diameter_(6.0e7),
          start_time_(0),
          end_time_(0),
          damage_amount_(0.2),
          damage_buffer_(5.0),
          warning_time_(15),
          warning_distance_(5) {}

    [[nodiscard]] double get_center_x() const noexcept { return center_x_; }
    [[nodiscard]] double get_center_z() const noexcept { return center_z_; }

    void set_center(double x, double z) noexcept {
        center_x_ = x;
        center_z_ = z;
        for (auto* listener : listeners_) {
            if (listener) listener->on_center_changed(*this, x, z);
        }
    }

    [[nodiscard]] double get_diameter(std::int64_t now_ms = 0) const noexcept {
        if (get_status(now_ms) != EnumBorderStatus::Stationary) {
            const double progress = static_cast<double>(now_ms - start_time_) / static_cast<double>(end_time_ - start_time_);
            if (progress < 1.0) {
                return start_diameter_ + (end_diameter_ - start_diameter_) * progress;
            }
        }
        return end_diameter_;
    }

    void set_diameter(double size) noexcept {
        start_diameter_ = size;
        end_diameter_ = size;
        start_time_ = 0;
        end_time_ = 0;
        for (auto* listener : listeners_) {
            if (listener) listener->on_size_changed(*this, size);
        }
    }

    void set_transition(double new_size, std::int64_t time_ms, std::int64_t now_ms) noexcept {
        start_diameter_ = get_diameter(now_ms);
        end_diameter_ = new_size;
        start_time_ = now_ms;
        end_time_ = now_ms + time_ms;
        for (auto* listener : listeners_) {
            if (listener) listener->on_transition_started(*this, start_diameter_, new_size, time_ms);
        }
    }

    [[nodiscard]] EnumBorderStatus get_status(std::int64_t now_ms) const noexcept {
        if (end_time_ <= now_ms) return EnumBorderStatus::Stationary;
        return end_diameter_ > start_diameter_ ? EnumBorderStatus::Growing : EnumBorderStatus::Shrinking;
    }

    [[nodiscard]] double min_x(std::int64_t now_ms = 0) const noexcept {
        return std::clamp(center_x_ - get_diameter(now_ms) / 2.0, -29999984.0, 29999984.0);
    }

    [[nodiscard]] double max_x(std::int64_t now_ms = 0) const noexcept {
        return std::clamp(center_x_ + get_diameter(now_ms) / 2.0, -29999984.0, 29999984.0);
    }

    [[nodiscard]] double min_z(std::int64_t now_ms = 0) const noexcept {
        return std::clamp(center_z_ - get_diameter(now_ms) / 2.0, -29999984.0, 29999984.0);
    }

    [[nodiscard]] double max_z(std::int64_t now_ms = 0) const noexcept {
        return std::clamp(center_z_ + get_diameter(now_ms) / 2.0, -29999984.0, 29999984.0);
    }

    [[nodiscard]] bool contains(double x, double z, std::int64_t now_ms = 0) const noexcept {
        return x > min_x(now_ms) && x < max_x(now_ms) && z > min_z(now_ms) && z < max_z(now_ms);
    }

    [[nodiscard]] double get_closest_distance(double x, double z, std::int64_t now_ms = 0) const noexcept {
        const double d_north = z - min_z(now_ms);
        const double d_south = max_z(now_ms) - z;
        const double d_west = x - min_x(now_ms);
        const double d_east = max_x(now_ms) - x;
        return std::min({d_north, d_south, d_west, d_east});
    }

    [[nodiscard]] double get_damage_amount() const noexcept { return damage_amount_; }
    [[nodiscard]] double get_damage_buffer() const noexcept { return damage_buffer_; }
    [[nodiscard]] int get_warning_time() const noexcept { return warning_time_; }
    [[nodiscard]] int get_warning_distance() const noexcept { return warning_distance_; }

    void add_listener(IBorderListener* listener) {
        if (listener) listeners_.push_back(listener);
    }

private:
    double center_x_;
    double center_z_;
    double start_diameter_;
    double end_diameter_;
    std::int64_t start_time_;
    std::int64_t end_time_;
    double damage_amount_;
    double damage_buffer_;
    int warning_time_;
    int warning_distance_;
    std::vector<IBorderListener*> listeners_;
};

}  // namespace cyane::world
