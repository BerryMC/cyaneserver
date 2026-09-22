#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cyane/entity/player_manager.hpp"
#include "cyane/net/connection.hpp"
#include "cyane/net/socket.hpp"

namespace cyane::net {

class NetService {
public:
    NetService(std::string bind_address, std::uint16_t port, unsigned threads, ConnectionContext context);
    ~NetService();

    NetService(const NetService&) = delete;
    NetService& operator=(const NetService&) = delete;

    [[nodiscard]] Result<void> start();
    void stop() noexcept;

    [[nodiscard]] std::uint64_t accepted() const noexcept { return accepted_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t active() const noexcept { return active_.load(std::memory_order_relaxed); }
    [[nodiscard]] unsigned threads() const noexcept { return threads_; }
    [[nodiscard]] std::uint16_t bound_port() const noexcept { return bound_port_; }

    [[nodiscard]] entity::PlayerManager& player_manager() noexcept { return *player_manager_; }

private:
    void run_thread(unsigned index, Socket listener) noexcept;

    std::string bind_address_;
    std::uint16_t port_;
    std::uint16_t bound_port_{0};
    unsigned threads_;
    ConnectionContext context_;
    std::vector<std::jthread> workers_;
    std::atomic<std::uint64_t> accepted_{0};
    std::atomic<std::uint64_t> active_{0};
    std::atomic<bool> running_{false};
    std::unique_ptr<entity::PlayerManager> player_manager_;
};

}