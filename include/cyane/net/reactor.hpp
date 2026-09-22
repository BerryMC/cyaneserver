#pragma once

#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <vector>

#include "cyane/core/error.hpp"

namespace cyane::net {

class ReactorHandler {
public:
    ReactorHandler() = default;
    virtual ~ReactorHandler() = default;
    ReactorHandler(const ReactorHandler&) = delete;
    ReactorHandler& operator=(const ReactorHandler&) = delete;

    virtual void on_readable() = 0;
    virtual void on_writable() = 0;
    virtual void on_error() = 0;
};

// epoll 封装：handler 指针直接放进 epoll_event.data.ptr，事件分发不做表查找
class Reactor {
public:
    Reactor() noexcept = default;

    ~Reactor() { destroy(); }

    Reactor(Reactor&& other) noexcept
        : epoll_fd_{other.epoll_fd_}, events_{std::move(other.events_)} {
        other.epoll_fd_ = -1;
    }

    Reactor& operator=(Reactor&& other) noexcept {
        if (this != &other) {
            destroy();
            epoll_fd_ = other.epoll_fd_;
            events_ = std::move(other.events_);
            other.epoll_fd_ = -1;
        }
        return *this;
    }

    Reactor(const Reactor&) = delete;
    Reactor& operator=(const Reactor&) = delete;

    [[nodiscard]] static Result<Reactor> create(unsigned max_events = 1024) {
        const int fd = ::epoll_create1(EPOLL_CLOEXEC);
        if (fd < 0) {
            return make_error(ErrorCode::net, std::format("epoll_create1: {}", std::strerror(errno)));
        }
        Reactor reactor;
        reactor.epoll_fd_ = fd;
        reactor.events_.resize(max_events);
        return reactor;
    }

    [[nodiscard]] Result<void> add(int fd, ReactorHandler& handler) {
        epoll_event event{};
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
        event.data.ptr = &handler;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event) != 0) {
            return make_error(ErrorCode::net, std::format("epoll_ctl(ADD, {}): {}", fd, std::strerror(errno)));
        }
        return {};
    }

    [[nodiscard]] Result<void> set_writable(int fd, ReactorHandler& handler, bool writable) {
        epoll_event event{};
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP | (writable ? EPOLLOUT : 0u);
        event.data.ptr = &handler;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event) != 0) {
            return make_error(ErrorCode::net, std::format("epoll_ctl(MOD, {}): {}", fd, std::strerror(errno)));
        }
        return {};
    }

    void remove(int fd) noexcept { ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr); }

    [[nodiscard]] int wait(std::chrono::milliseconds timeout) {
        const int count = ::epoll_wait(epoll_fd_, events_.data(), static_cast<int>(events_.size()),
                                       static_cast<int>(timeout.count()));
        if (count <= 0) {
            return count;
        }
        for (int index = 0; index < count; ++index) {
            const auto& event = events_[static_cast<std::size_t>(index)];
            auto* handler = static_cast<ReactorHandler*>(event.data.ptr);
            if (handler == nullptr) {
                continue;
            }
            if ((event.events & (EPOLLHUP | EPOLLERR)) != 0) {
                handler->on_error();
                continue;
            }
            if ((event.events & (EPOLLIN | EPOLLRDHUP)) != 0) {
                handler->on_readable();
            }
            if ((event.events & EPOLLOUT) != 0) {
                handler->on_writable();
            }
        }
        return count;
    }

    [[nodiscard]] bool valid() const noexcept { return epoll_fd_ >= 0; }

private:
    void destroy() noexcept {
        if (epoll_fd_ >= 0) {
            ::close(epoll_fd_);
            epoll_fd_ = -1;
        }
    }

    int epoll_fd_{-1};
    std::vector<epoll_event> events_;
};

}
