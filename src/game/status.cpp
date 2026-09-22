#include "cyane/game/status.hpp"

#include <string>

#include "cyane/proto/json.hpp"
#include "cyane/proto/packet_ids.hpp"

namespace cyane::game {

std::string ServerStatus::build_status_json() const {
    std::string out;
    out.reserve(256);
    out += "{\"description\":";
    out += proto::chat_text(motd_);
    out += ",\"players\":{\"max\":";
    out += std::to_string(max_players_);
    out += ",\"online\":";
    out += std::to_string(online_.load(std::memory_order_relaxed));
    out += "},\"version\":{\"name\":";
    out += proto::json_string(proto::kMinecraftVersion);
    out += ",\"protocol\":";
    out += std::to_string(proto::kProtocolVersion);
    out += "}";
    if (!favicon_.empty()) {
        out += ",\"favicon\":";
        out += proto::json_string(favicon_);
    }
    out += "}";
    return out;
}

}
