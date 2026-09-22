#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "cyane/core/error.hpp"
#include "cyane/world/chunk.hpp"

namespace cyane::entity {

enum class EntityType : std::uint8_t {
    Player = 1,
    Mob = 2,
    Item = 3,
    Arrow = 4,
    TNT = 5,
};

struct [[nodiscard]] Position {
    double x{0.0};
    double y{0.0};
    double z{0.0};
    float yaw{0.0f};
    float pitch{0.0f};

    [[nodiscard]] Position operator+(const Position& other) const noexcept {
        return {x + other.x, y + other.y, z + other.z, yaw, pitch};
    }
};

class Entity {
public:
    Entity() = default;
    Entity(std::uint32_t id, EntityType type, Position pos)
        : id_(id), type_(type), pos_(std::move(pos)) {}
    virtual ~Entity() = default;

    [[nodiscard]] std::uint32_t id() const noexcept { return id_; }
    [[nodiscard]] EntityType type() const noexcept { return type_; }
    [[nodiscard]] const Position& pos() const noexcept { return pos_; }

    void set_position(Position pos) noexcept { pos_ = std::move(pos); }

    [[nodiscard]] virtual bool is_player() const noexcept { return false; }

protected:
    std::uint32_t id_{0};
    EntityType type_{EntityType::Player};
    Position pos_{};
};

class Player : public Entity {
public:
    Player() = default;
    Player(std::uint32_t id, Position pos, std::string name)
        : Entity(id, EntityType::Player, std::move(pos)), name_(std::move(name)) {}

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] bool is_player() const noexcept override { return true; }

    void set_name(std::string name) noexcept { name_ = std::move(name); }

    [[nodiscard]] std::optional<world::ChunkPos> current_chunk() const noexcept {
        return world::ChunkPos::from_world(static_cast<std::int32_t>(pos_.x),
                                           static_cast<std::int32_t>(pos_.z));
    }

private:
    std::string name_;
};

}
