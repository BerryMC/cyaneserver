#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

#include "cyane/net/packet_writers.hpp"
#include "cyane/net/player_hub.hpp"
#include "cyane/world/world.hpp"

namespace cyane::net {

// 世界级延迟方块更新（目前只有按钮回弹）。状态放在世界里而不是某个连接上：
// 按下按钮的玩家在回弹前断线，按钮也必须照常弹起（vanilla 的方块延迟更新同理）。
// 线程约定：schedule 从 reactor 线程调用，tick 从 tick 线程调用，内部自持锁。
class BlockTicks {
public:
    // 石按钮 1s / 木按钮 0.75s 后清除 0x8 位（vanilla 1.12.2）
    void schedule_button_release(std::int32_t x, std::int32_t y, std::int32_t z,
                                 std::uint64_t due_ms) {
        std::lock_guard<std::mutex> lock{mutex_};
        pending_.push_back(Pending{world::pack_block_pos(x, y, z), due_ms});
    }

    // 到期项：清 0x8 并广播 BlockChange + 回弹音（回弹音客户端不预测，必须服务端发）
    void tick(std::uint64_t now_ms, world::World& world, PlayerHub& hub, std::int32_t view_distance);

    [[nodiscard]] std::size_t pending() const {
        std::lock_guard<std::mutex> lock{mutex_};
        return pending_.size();
    }

private:
    struct Pending {
        std::int64_t key{0};
        std::uint64_t due_ms{0};
    };

    mutable std::mutex mutex_;
    std::vector<Pending> pending_;
};

}  // namespace cyane::net
