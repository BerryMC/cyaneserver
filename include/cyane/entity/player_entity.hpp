#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "cyane/core/error.hpp"
#include "cyane/core/bytes.hpp"
#include "cyane/world/chunk.hpp"

namespace cyane::entity {

struct Position {
    double x{0.0};
    double y{0.0};
    double z{0.0};
    float yaw{0.0f};
    float pitch{0.0f};

    Position operator+(const Position& other) const noexcept {
        return {x + other.x, y + other.y, z + other.z, yaw + other.yaw, pitch + other.pitch};
    }
};

class Player {
public:
    Player(std::uint32_t id, std::string name, Position pos)
        : id_(id), name_(std::move(name)), pos_(std::move(pos)) {}

    [[nodiscard]] std::uint32_t id() const noexcept { return id_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const Position& pos() const noexcept { return pos_; }

    void set_position(Position pos) noexcept { pos_ = std::move(pos); }
    void set_name(std::string name) noexcept { name_ = std::move(name); }

    [[nodiscard]] std::optional<world::ChunkPos> current_chunk() const noexcept {
        return world::ChunkPos::from_world(static_cast<std::int32_t>(pos_.x),
                                           static_cast<std::int32_t>(pos_.z));
    }

private:
    std::uint32_t id_{0};
    std::string name_;
    Position pos_{};
};

}