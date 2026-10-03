#pragma once

#include <cstdint>

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
}

// PacketPlayOutNamedSoundEffect 的 SoundCategory 序数（EnumSoundCategory 声明顺序）
namespace sound_category {
inline constexpr std::int32_t kBlocks = 4;
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
