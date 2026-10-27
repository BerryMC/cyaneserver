#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_set>

namespace cyane::world {

// 1.12.2 原版 Boss 血条状态容器（BossInfo / BossInfoServer 逐行转写）
// 颜色与分段叠加样式对应 1.12.2 SPacketUpdateBossInfo 的枚举序号
enum class BossColor : std::uint8_t {
    Pink = 0,
    Blue = 1,
    Red = 2,
    Green = 3,
    Yellow = 4,
    Purple = 5,
    White = 6
};

enum class BossOverlay : std::uint8_t {
    Progress = 0,
    Notched6 = 1,
    Notched10 = 2,
    Notched12 = 3,
    Notched20 = 4
};

class BossInfo {
public:
    BossInfo(std::uint32_t entity_id, std::string name, BossColor color, BossOverlay overlay)
        : entity_id_(entity_id), name_(std::move(name)), color_(color), overlay_(overlay) {}

    [[nodiscard]] std::uint32_t entity_id() const noexcept { return entity_id_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    void set_name(std::string name) { name_ = std::move(name); }

    [[nodiscard]] float percent() const noexcept { return percent_; }
    void set_percent(float percent) noexcept { percent_ = std::clamp(percent, 0.0f, 1.0f); }

    [[nodiscard]] BossColor color() const noexcept { return color_; }
    void set_color(BossColor color) noexcept { color_ = color; }

    [[nodiscard]] BossOverlay overlay() const noexcept { return overlay_; }
    void set_overlay(BossOverlay overlay) noexcept { overlay_ = overlay; }

    [[nodiscard]] bool darken_sky() const noexcept { return darken_sky_; }
    void set_darken_sky(bool value) noexcept { darken_sky_ = value; }

    [[nodiscard]] bool play_end_boss_music() const noexcept { return play_end_boss_music_; }
    void set_play_end_boss_music(bool value) noexcept { play_end_boss_music_ = value; }

    [[nodiscard]] bool create_fog() const noexcept { return create_fog_; }
    void set_create_fog(bool value) noexcept { create_fog_ = value; }

private:
    std::uint32_t entity_id_;
    std::string name_;
    float percent_{1.0f};
    BossColor color_;
    BossOverlay overlay_;
    bool darken_sky_{false};
    bool play_end_boss_music_{false};
    bool create_fog_{false};
};

// 服务端 Boss 血条：维护可见该血条的玩家集合（以玩家实体 id 表示）
class BossInfoServer : public BossInfo {
public:
    BossInfoServer(std::uint32_t entity_id, std::string name, BossColor color, BossOverlay overlay)
        : BossInfo(entity_id, std::move(name), color, overlay) {}

    void add_player(std::uint32_t player_entity_id) { players_.insert(player_entity_id); }

    void remove_player(std::uint32_t player_entity_id) { players_.erase(player_entity_id); }

    [[nodiscard]] bool is_visible() const noexcept { return visible_; }
    void set_visible(bool visible) noexcept { visible_ = visible; }

    [[nodiscard]] const std::unordered_set<std::uint32_t>& players() const noexcept { return players_; }

    [[nodiscard]] bool should_notify(std::uint32_t player_entity_id) const noexcept {
        return visible_ && players_.contains(player_entity_id);
    }

private:
    std::unordered_set<std::uint32_t> players_;
    bool visible_{true};
};

}  // namespace cyane::world
