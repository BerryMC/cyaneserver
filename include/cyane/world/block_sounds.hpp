#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace cyane::world {

// 可切换方块（按钮/拉杆/门/活板门/栅栏门）的开关音效。
//
// 线上格式用 id：1.12.2 的 Named Sound Effect (0x49) 首字段是 SoundEffect 注册表 id（VarInt），
// 不是名字。注册表按 SoundEvent.b() 的名字顺序从 0 编号（客户端 qf/qe 与服务端 SoundEffect 两侧
// 完全一致，见 tests/fixtures/sound_registry_ids.txt）。名字只作审计用。
//
// 数值取自 vanilla 1.12.2 BlockButtonAbstract/BlockLever/BlockDoor/BlockTrapdoor/BlockFenceGate：
// 按钮与拉杆由方块自身按 BLOCKS 类别 0.3 音量播报（按下 0.6、回弹 0.5），门类为 1.0。
// radius 为原版广播半径（格）：命名音效 16，门类的 World Event 为 64。
//
// predicted：客户端已本地播放该音效（按下按钮、开关门由客户端预测；回弹与拉杆不预测）。
// vanilla 对预测音效传 entityhuman 给 World.a 把操作者排除在广播外——否则操作者听到两遍。
struct ToggleSound {
    std::string_view on;
    std::string_view off;
    std::int32_t on_id{0};
    std::int32_t off_id{0};
    float volume{1.0f};
    float on_pitch{1.0f};
    float off_pitch{1.0f};
    std::int32_t radius{16};
    bool on_predicted{false};
    bool off_predicted{false};
};

[[nodiscard]] constexpr std::optional<ToggleSound> toggle_sound(std::uint16_t block_id) noexcept {
    switch (block_id) {
        case 64:   // 木门（橡木）
        case 193:  // 云杉门
        case 194:  // 白桦门
        case 195:  // 丛林木门
        case 196:  // 金合欢门
        case 197:  // 深色橡木门
            return ToggleSound{.on = "block.wooden_door.open", .off = "block.wooden_door.close",
                               .on_id = 129, .off_id = 128, .radius = 64, .on_predicted = true,
                               .off_predicted = true};
        case 69:  // 拉杆
            return ToggleSound{.on = "block.lever.click", .off = "block.lever.click",
                               .on_id = 62, .off_id = 62, .volume = 0.3f, .on_pitch = 0.6f,
                               .off_pitch = 0.5f};
        case 71:  // 铁门
            return ToggleSound{.on = "block.iron_door.open", .off = "block.iron_door.close",
                               .on_id = 51, .off_id = 50, .radius = 64, .on_predicted = true,
                               .off_predicted = true};
        case 77:  // 石按钮：按下由客户端预测，回弹只能由服务端发（延迟 1s）
            return ToggleSound{.on = "block.stone_button.click_on",
                               .off = "block.stone_button.click_off", .on_id = 110, .off_id = 109,
                               .volume = 0.3f, .on_pitch = 0.6f, .off_pitch = 0.5f,
                               .on_predicted = true};
        case 96:  // 木活板门
            return ToggleSound{.on = "block.wooden_trapdoor.open",
                               .off = "block.wooden_trapdoor.close", .on_id = 131, .off_id = 130,
                               .radius = 64, .on_predicted = true, .off_predicted = true};
        case 107:  // 栅栏门
            return ToggleSound{.on = "block.fence_gate.open", .off = "block.fence_gate.close",
                               .on_id = 31, .off_id = 30, .radius = 64, .on_predicted = true,
                               .off_predicted = true};
        case 143:  // 木按钮
            return ToggleSound{.on = "block.wood_button.click_on",
                               .off = "block.wood_button.click_off", .on_id = 125, .off_id = 124,
                               .volume = 0.3f, .on_pitch = 0.6f, .off_pitch = 0.5f,
                               .on_predicted = true};
        default:
            return std::nullopt;
    }
}

}  // namespace cyane::world
