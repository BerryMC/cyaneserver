#include "cyane/net/connection.hpp"

#include "cyane/net/packet_writers.hpp"
#include "cyane/proto/packet_ids.hpp"

namespace cyane::net {

void Connection::add_experience(int value) {
    // 原版 addExperience：累加 total，experience bar 满 1.0 升一级
    experience_total_ += value;
    // xpBarCap 公式
    auto xp_bar_cap = [](int level) -> int {
        if (level >= 30) {
            return 112 + (level - 30) * 9;
        } else if (level >= 15) {
            return 37 + (level - 15) * 5;
        } else {
            return 7 + level * 2;
        }
    };

    float xp = static_cast<float>(value) / static_cast<float>(xp_bar_cap(experience_level_));
    experience_ += xp;
    while (experience_ >= 1.0f) {
        experience_ = (experience_ - 1.0f) * static_cast<float>(xp_bar_cap(experience_level_));
        ++experience_level_;
    }

    // 发送 SetExperience 包
    ByteWriter exp;
    net::writers::write_set_experience(exp, experience_, experience_level_, experience_total_);
    send_packet(proto::play_cb::kSetExperience, exp.data());
}

}  // namespace cyane::net
