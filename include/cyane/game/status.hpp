#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "cyane/net/connection.hpp"

namespace cyane::game {

class ServerStatus final : public net::StatusProvider {
public:
    explicit ServerStatus(std::string motd, std::int32_t max_players, std::string favicon = {})
        : motd_{std::move(motd)}, favicon_{std::move(favicon)}, max_players_{max_players} {}

    [[nodiscard]] std::string build_status_json() const override;

    void set_online(std::int32_t count) noexcept { online_.store(count, std::memory_order_relaxed); }
    void set_max_players(std::int32_t count) noexcept { max_players_ = count; }
    void set_motd(std::string motd) { motd_ = std::move(motd); }
    [[nodiscard]] std::int32_t online() const noexcept { return online_.load(std::memory_order_relaxed); }

private:
    std::string motd_;
    std::string favicon_;
    std::atomic<std::int32_t> online_{0};
    std::int32_t max_players_;
};

}
