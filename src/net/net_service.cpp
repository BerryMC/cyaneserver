#include "cyane/net/net_service.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <format>
#include <memory>
#include <thread>
#include <utility>

#include "cyane/core/log.hpp"
#include "cyane/net/reactor.hpp"

namespace cyane::net {
namespace {

inline constexpr auto kSweepInterval = std::chrono::milliseconds{100};
inline constexpr auto kAcceptBackoff = std::chrono::milliseconds{20};

// 一个 I/O 线程一套：listener + reactor + 该线程独占的连接集合
class Acceptor final : public ReactorHandler {
public:
    Acceptor(Socket listener,
             Reactor& reactor,
             const ConnectionContext& context,
             std::atomic<std::uint64_t>& accepted,
             std::atomic<std::uint64_t>& active)
        : listener_{std::move(listener)},
          reactor_{&reactor},
          context_{context},
          accepted_{accepted},
          active_{active} {
        connections_.reserve(256);
    }

    ~Acceptor() override {
        for (auto& connection : connections_) {
            reactor_->remove(connection->fd());
        }
    }

    Acceptor(const Acceptor&) = delete;
    Acceptor& operator=(const Acceptor&) = delete;

    [[nodiscard]] int fd() const noexcept { return listener_.fd(); }

    void on_readable() override {
        for (;;) {
            std::string peer;
            auto accepted = listener_.accept(peer);
            if (!accepted) {
                if (errno == EMFILE || errno == ENFILE) {
                    log::warn("file descriptor limit reached, backing off");
                    std::this_thread::sleep_for(kAcceptBackoff);
                }
                return;
            }
            auto connection =
                std::make_unique<Connection>(std::move(*accepted), std::move(peer), *reactor_, context_);
            if (auto added = reactor_->add(connection->fd(), *connection); !added) {
                log::warn("cannot register connection: {}", added.error().message);
                continue;
            }
            connections_.push_back(std::move(connection));
            accepted_.fetch_add(1, std::memory_order_relaxed);
            active_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void on_writable() override {}
    void on_error() override {}

    void sweep() {
        // 先驱动每连接的周期逻辑（KeepAlive/超时），再回收已死连接
        const std::uint64_t now_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        for (auto& connection : connections_) {
            if (connection->alive()) {
                connection->tick(now_ms);
            }
        }
        const auto removed = std::erase_if(connections_, [](const std::unique_ptr<Connection>& connection) {
            return !connection->alive();
        });
        if (removed > 0) {
            active_.fetch_sub(static_cast<std::uint64_t>(removed), std::memory_order_relaxed);
        }
    }

private:
    Socket listener_;
    Reactor* reactor_;
    ConnectionContext context_;
    std::atomic<std::uint64_t>& accepted_;
    std::atomic<std::uint64_t>& active_;
    std::vector<std::unique_ptr<Connection>> connections_;
};

}

NetService::NetService(std::string bind_address, std::uint16_t port, unsigned threads, ConnectionContext context)
    : bind_address_{std::move(bind_address)},
      port_{port},
      threads_{threads == 0 ? 1 : threads},
      context_{std::move(context)} {}

NetService::~NetService() { stop(); }

Result<void> NetService::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return {};
    }

    const bool reuse_port = threads_ > 1;
    std::vector<Socket> listeners;
    listeners.reserve(threads_);
    for (unsigned index = 0; index < threads_; ++index) {
        auto listener = Socket::listen_tcp(bind_address_, port_, 512, reuse_port);
        if (!listener) {
            running_.store(false, std::memory_order_release);
            return std::unexpected{std::move(listener.error())};
        }
        listeners.push_back(std::move(*listener));
    }
    bound_port_ = listeners.front().local_port();

    workers_.reserve(threads_);
    for (unsigned index = 0; index < threads_; ++index) {
        workers_.emplace_back([this, index, listener = std::move(listeners[index])]() mutable {
            run_thread(index, std::move(listener));
        });
    }
    log::info("listening on {}:{} with {} I/O thread(s)", bind_address_, bound_port_, threads_);
    return {};
}

void NetService::run_thread(unsigned index, Socket listener) noexcept {
    log::set_thread_name(std::format("net{}", index));
    auto reactor = Reactor::create();
    if (!reactor) {
        log::error("net{}: {}", index, reactor.error().message);
        return;
    }

    Acceptor acceptor{std::move(listener), *reactor, context_, accepted_, active_};
    if (auto added = reactor->add(acceptor.fd(), acceptor); !added) {
        log::error("net{}: {}", index, added.error().message);
        return;
    }

    while (running_.load(std::memory_order_acquire)) {
        const int ready = reactor->wait(kSweepInterval);
        if (ready < 0 && errno != EINTR) {
            log::error("net{}: epoll_wait failed: {}", index, std::strerror(errno));
            break;
        }
        acceptor.sweep();
    }
    log::debug("net{}: reactor stopped", index);
}

void NetService::stop() noexcept {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    workers_.clear();
    active_.store(0, std::memory_order_relaxed);
}

}
