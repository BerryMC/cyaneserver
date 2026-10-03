#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace cyane::world {

// 可切换方块（按钮/拉杆/门/活板门/栅栏门）的开关音效。
// 名称与数值取自 vanilla 1.12.2 BlockButtonAbstract/BlockLever/BlockDoor/BlockTrapdoor/
// BlockFenceGate：按钮与拉杆由方块自身按 BLOCKS 类别 0.3 音量播报（按下 0.6、回弹 0.5），
// 门类为 1.0。音效名必须是客户端注册表中的名字，否则客户端静默丢弃。
// radius 为原版广播半径（格）：命名音效 16，门类的 World Event 为 64。
struct ToggleSound {
    std::string_view on;
    std::string_view off;
    float volume;
    float on_pitch;
    float off_pitch;
    std::int32_t radius{16};
};

[[nodiscard]] constexpr std::optional<ToggleSound> toggle_sound(std::uint16_t block_id) noexcept {
    switch (block_id) {
        case 64:  // 木门
            return ToggleSound{"block.wooden_door.open", "block.wooden_door.close", 1.0f, 1.0f, 1.0f,
                               64};
        case 69:  // 拉杆
            return ToggleSound{"block.lever.click", "block.lever.click", 0.3f, 0.6f, 0.5f, 16};
        case 71:  // 铁门
            return ToggleSound{"block.iron_door.open", "block.iron_door.close", 1.0f, 1.0f, 1.0f, 64};
        case 77:  // 石按钮
            return ToggleSound{"block.stone_button.click_on", "block.stone_button.click_off", 0.3f,
                               0.6f, 0.5f, 16};
        case 96:  // 木活板门
            return ToggleSound{"block.wooden_trapdoor.open", "block.wooden_trapdoor.close", 1.0f, 1.0f,
                               1.0f, 64};
        case 107:  // 栅栏门
            return ToggleSound{"block.fence_gate.open", "block.fence_gate.close", 1.0f, 1.0f, 1.0f,
                               64};
        case 143:  // 木按钮
            return ToggleSound{"block.wood_button.click_on", "block.wood_button.click_off", 0.3f, 0.6f,
                               0.5f, 16};
        default:
            return std::nullopt;
    }
}

}  // namespace cyane::world
