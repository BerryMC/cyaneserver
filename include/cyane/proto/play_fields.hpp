#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace cyane::proto {

// PacketPlayOutPlayerInfo.EnumPlayerInfoAction 的序数
namespace player_info_action {
inline constexpr std::int32_t kAddPlayer = 0;
inline constexpr std::int32_t kUpdateGameMode = 1;
inline constexpr std::int32_t kUpdateLatency = 2;
inline constexpr std::int32_t kUpdateDisplayName = 3;
inline constexpr std::int32_t kRemovePlayer = 4;
}

namespace game_mode {
inline constexpr std::uint8_t kSurvival = 0;
inline constexpr std::uint8_t kCreative = 1;
inline constexpr std::uint8_t kAdventure = 2;
inline constexpr std::uint8_t kSpectator = 3;
inline constexpr std::uint8_t kHardcoreFlag = 0x08;

// GameType.byName：同时匹配全名与 shortName（survival/s、creative/c、adventure/a、spectator/sp）
[[nodiscard]] constexpr std::optional<std::uint8_t> parse(std::string_view name) noexcept {
    if (name == "survival" || name == "s") return kSurvival;
    if (name == "creative" || name == "c") return kCreative;
    if (name == "adventure" || name == "a") return kAdventure;
    if (name == "spectator" || name == "sp") return kSpectator;
    return std::nullopt;
}

// GameType.getByID：0..3，越界回落
[[nodiscard]] constexpr std::optional<std::uint8_t> from_id(std::int32_t id) noexcept {
    return id >= 0 && id <= 3 ? std::optional<std::uint8_t>{static_cast<std::uint8_t>(id)}
                              : std::nullopt;
}
}

// Entity.setFlag 的位序（EntityMetadata index 0 = SharedFlags）
namespace entity_flags {
inline constexpr std::uint8_t kOnFire = 1U << 0;    // setFire → setFlag(0)
inline constexpr std::uint8_t kSneaking = 1U << 1;  // setSneaking → setFlag(1)
inline constexpr std::uint8_t kSprinting = 1U << 3; // setSprinting → setFlag(3)
inline constexpr std::uint8_t kInvisible = 1U << 5;
inline constexpr std::uint8_t kGlowing = 1U << 6;
}

// PacketPlayOutNamedSoundEffect 的 SoundCategory 序数（EnumSoundCategory 声明顺序）
// SoundCategory 序号（EntityPlayer.getSoundCategory() = PLAYERS）
namespace sound_category {
inline constexpr std::int32_t kBlocks = 4;
inline constexpr std::int32_t kPlayers = 0;
}

// PacketPlayOutAbilities 的 flags 位
namespace abilities {
inline constexpr std::uint8_t kInvulnerable = 0x01;
inline constexpr std::uint8_t kFlying = 0x02;
inline constexpr std::uint8_t kAllowFlying = 0x04;
inline constexpr std::uint8_t kCreativeMode = 0x08;
}

inline constexpr std::int32_t kMaxViewDistance = 32;
inline constexpr std::int32_t kBiomeBytesPerChunk = 256;

}
