#pragma once

#include <cstdint>
#include <string_view>

namespace cyane::proto {

// 协议事实：1.12.2 = protocol 340。data/registry.toml 中的同名值由测试断言保持一致
inline constexpr std::int32_t kProtocolVersion = 340;
inline constexpr std::string_view kMinecraftVersion = "1.12.2";

enum class State : std::uint8_t {
    handshake = 0,
    status = 1,
    login = 2,
    play = 3,
};

namespace handshake_sb {
inline constexpr std::int32_t kHandshake = 0x00;
}

namespace status_sb {
inline constexpr std::int32_t kRequest = 0x00;
inline constexpr std::int32_t kPing = 0x01;
}

namespace status_cb {
inline constexpr std::int32_t kResponse = 0x00;
inline constexpr std::int32_t kPong = 0x01;
}

namespace login_sb {
inline constexpr std::int32_t kLoginStart = 0x00;
inline constexpr std::int32_t kEncryptionResponse = 0x01;
}

namespace login_cb {
inline constexpr std::int32_t kDisconnect = 0x00;
inline constexpr std::int32_t kEncryptionRequest = 0x01;
inline constexpr std::int32_t kSuccess = 0x02;
inline constexpr std::int32_t kSetCompression = 0x03;
}

// 包 ID 来自 EnumProtocol 中 Packet 类的注册顺序（tools/probe 从 spigot 1.12.2 jar 反射导出）
namespace play_cb {
inline constexpr std::int32_t kSpawnObject = 0x00;
inline constexpr std::int32_t kSpawnExperienceOrb = 0x01;
inline constexpr std::int32_t kSpawnGlobalEntity = 0x02;
inline constexpr std::int32_t kSpawnMob = 0x03;
inline constexpr std::int32_t kSpawnPainting = 0x04;
inline constexpr std::int32_t kSpawnPlayer = 0x05;
inline constexpr std::int32_t kAnimation = 0x06;
inline constexpr std::int32_t kStatistics = 0x07;
inline constexpr std::int32_t kBlockBreakAnimation = 0x08;
inline constexpr std::int32_t kBlockEntityData = 0x09;
inline constexpr std::int32_t kBlockAction = 0x0A;
inline constexpr std::int32_t kBlockChange = 0x0B;
inline constexpr std::int32_t kBossBar = 0x0C;
inline constexpr std::int32_t kServerDifficulty = 0x0D;
inline constexpr std::int32_t kTabComplete = 0x0E;
inline constexpr std::int32_t kChatMessage = 0x0F;
inline constexpr std::int32_t kMultiBlockChange = 0x10;
inline constexpr std::int32_t kConfirmTransaction = 0x11;
inline constexpr std::int32_t kCloseWindow = 0x12;
inline constexpr std::int32_t kOpenWindow = 0x13;
inline constexpr std::int32_t kWindowItems = 0x14;
inline constexpr std::int32_t kWindowProperty = 0x15;
inline constexpr std::int32_t kSetSlot = 0x16;
inline constexpr std::int32_t kSetCooldown = 0x17;
inline constexpr std::int32_t kPluginMessage = 0x18;
inline constexpr std::int32_t kCustomSoundEffect = 0x19;
inline constexpr std::int32_t kDisconnect = 0x1A;
inline constexpr std::int32_t kEntityStatus = 0x1B;
inline constexpr std::int32_t kExplosion = 0x1C;
inline constexpr std::int32_t kUnloadChunk = 0x1D;
inline constexpr std::int32_t kGameStateChange = 0x1E;
inline constexpr std::int32_t kKeepAlive = 0x1F;
inline constexpr std::int32_t kChunkData = 0x20;
inline constexpr std::int32_t kWorldEvent = 0x21;
inline constexpr std::int32_t kParticles = 0x22;
inline constexpr std::int32_t kJoinGame = 0x23;
inline constexpr std::int32_t kMap = 0x24;
inline constexpr std::int32_t kEntity = 0x25;
inline constexpr std::int32_t kEntityRelMove = 0x26;
inline constexpr std::int32_t kEntityRelMoveLook = 0x27;
inline constexpr std::int32_t kEntityLook = 0x28;
inline constexpr std::int32_t kVehicleMove = 0x29;
inline constexpr std::int32_t kOpenSignEditor = 0x2A;
inline constexpr std::int32_t kCraftRecipeResponse = 0x2B;
inline constexpr std::int32_t kPlayerAbilities = 0x2C;
inline constexpr std::int32_t kCombatEvent = 0x2D;
// 1.12.2 PlayerInfo action = EnumPlayerInfoAction 序数（不是 1.14+ 的编号！）
inline constexpr std::int32_t kPlayerInfoAddPlayer = 0x00;
inline constexpr std::int32_t kPlayerInfoRemovePlayer = 0x01;
inline constexpr std::int32_t kPlayerInfoUpdateGameType = 0x02;
inline constexpr std::int32_t kPlayerInfoUpdateLatency = 0x03;
inline constexpr std::int32_t kPlayerInfoUpdateDisplayName = 0x04;
inline constexpr std::int32_t kPlayerInfo = 0x2E;
inline constexpr std::int32_t kConfirmTeleport = 0x00;
inline constexpr std::int32_t kPlayerPositionLook = 0x2F;
inline constexpr std::int32_t kUseBed = 0x30;
inline constexpr std::int32_t kUnlockRecipes = 0x31;
inline constexpr std::int32_t kDestroyEntities = 0x32;
inline constexpr std::int32_t kRemoveEntityEffect = 0x33;
inline constexpr std::int32_t kResourcePackSend = 0x34;
inline constexpr std::int32_t kRespawn = 0x35;
inline constexpr std::int32_t kEntityHeadLook = 0x36;
inline constexpr std::int32_t kSelectAdvancementTab = 0x37;
inline constexpr std::int32_t kWorldBorder = 0x38;
inline constexpr std::int32_t kCamera = 0x39;
inline constexpr std::int32_t kHeldItemChange = 0x3A;
inline constexpr std::int32_t kDisplayScoreboard = 0x3B;
inline constexpr std::int32_t kEntityMetadata = 0x3C;
inline constexpr std::int32_t kAttachEntity = 0x3D;
inline constexpr std::int32_t kEntityVelocity = 0x3E;
inline constexpr std::int32_t kEntityEquipment = 0x3F;
inline constexpr std::int32_t kSetExperience = 0x40;
inline constexpr std::int32_t kUpdateHealth = 0x41;
inline constexpr std::int32_t kScoreboardObjective = 0x42;
inline constexpr std::int32_t kSetPassengers = 0x43;
inline constexpr std::int32_t kTeams = 0x44;
inline constexpr std::int32_t kUpdateScore = 0x45;
inline constexpr std::int32_t kSpawnPosition = 0x46;
inline constexpr std::int32_t kTimeUpdate = 0x47;
inline constexpr std::int32_t kTitle = 0x48;
inline constexpr std::int32_t kSoundEffect = 0x49;
inline constexpr std::int32_t kPlayerListHeaderFooter = 0x4A;
inline constexpr std::int32_t kCollectItem = 0x4B;
inline constexpr std::int32_t kEntityTeleport = 0x4C;
inline constexpr std::int32_t kAdvancements = 0x4D;
inline constexpr std::int32_t kEntityProperties = 0x4E;
inline constexpr std::int32_t kEntityEffect = 0x4F;
}

namespace play_sb {
inline constexpr std::int32_t kConfirmTeleport = 0x00;
inline constexpr std::int32_t kTabComplete = 0x01;
inline constexpr std::int32_t kChatMessage = 0x02;
inline constexpr std::int32_t kClientCommand = 0x03;
inline constexpr std::int32_t kSettings = 0x04;
inline constexpr std::int32_t kConfirmTransaction = 0x05;
inline constexpr std::int32_t kEnchantItem = 0x06;
inline constexpr std::int32_t kClickWindow = 0x07;
inline constexpr std::int32_t kCloseWindow = 0x08;
inline constexpr std::int32_t kPluginMessage = 0x09;
inline constexpr std::int32_t kUseEntity = 0x0A;
inline constexpr std::int32_t kKeepAlive = 0x0B;
inline constexpr std::int32_t kFlying = 0x0C;
inline constexpr std::int32_t kPosition = 0x0D;
inline constexpr std::int32_t kPositionLook = 0x0E;
inline constexpr std::int32_t kLook = 0x0F;
inline constexpr std::int32_t kVehicleMove = 0x10;
inline constexpr std::int32_t kBoatMove = 0x11;
inline constexpr std::int32_t kCraftRecipe = 0x12;
inline constexpr std::int32_t kAbilities = 0x13;
inline constexpr std::int32_t kPlayerDigging = 0x14;
inline constexpr std::int32_t kEntityAction = 0x15;
inline constexpr std::int32_t kSteerVehicle = 0x16;
inline constexpr std::int32_t kRecipeDisplayed = 0x17;
inline constexpr std::int32_t kResourcePackStatus = 0x18;
inline constexpr std::int32_t kAdvancementTab = 0x19;
inline constexpr std::int32_t kHeldItemChange = 0x1A;
inline constexpr std::int32_t kCreativeInventoryAction = 0x1B;
inline constexpr std::int32_t kUpdateSign = 0x1C;
inline constexpr std::int32_t kAnimation = 0x1D;
inline constexpr std::int32_t kSpectate = 0x1E;
inline constexpr std::int32_t kBlockPlace = 0x1F;
inline constexpr std::int32_t kUseItem = 0x20;
}

}