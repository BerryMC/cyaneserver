#include "cyane/net/block_ticks.hpp"

#include <algorithm>
#include <utility>

#include "cyane/core/log.hpp"
#include "cyane/proto/packet_ids.hpp"
#include "cyane/proto/play_fields.hpp"
#include "cyane/world/block_sounds.hpp"

namespace cyane::net {

void BlockTicks::tick(std::uint64_t now_ms, world::World& world, PlayerHub& hub,
                      std::int32_t view_distance) {
    std::vector<Pending> due;
    {
        std::lock_guard<std::mutex> lock{mutex_};
        std::erase_if(pending_, [&](const Pending& p) {
            if (p.due_ms > now_ms) {
                return false;
            }
            due.push_back(p);
            return true;
        });
    }
    if (due.empty()) {
        return;
    }
    const std::int32_t chunk_radius = std::clamp(view_distance, 2, 8);
    for (const auto& p : due) {
        const auto [bx, by, bz] = world::unpack_block_pos(p.key);
        const auto state = world.block_at(bx, by, bz);
        const auto cid = world::block_id(state);
        // 已被破坏/替换/提前回弹：什么都不做
        if (!world::is_button(cid) || (state & 0x08) == 0) {
            continue;
        }
        const auto released = static_cast<std::uint16_t>(state & ~0x08);
        world.set_block(bx, by, bz, released);
        ByteWriter change;
        change.position(bx, by, bz);
        change.varint(static_cast<std::int32_t>(released));
        if (const auto cpos = world::ChunkPos::from_world(bx, bz)) {
            hub.broadcast_near(cpos->x, cpos->z, chunk_radius, kNoExclude,
                               proto::play_cb::kBlockChange, change.data());
        }
        const auto sound = world::toggle_sound(cid);
        if (!sound) {
            continue;
        }
        ByteWriter out;
        writers::write_named_sound(out, sound->off_id, proto::sound_category::kBlocks, bx, by, bz,
                                   sound->volume, sound->off_pitch);
        if (const auto cpos = world::ChunkPos::from_world(bx, bz)) {
            const std::int32_t radius = std::max(1, sound->radius / 16);
            hub.broadcast_near(cpos->x, cpos->z, radius, kNoExclude, proto::play_cb::kSoundEffect,
                               out.data());
        }
    }
}

}  // namespace cyane::net
