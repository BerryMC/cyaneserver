#pragma once

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "cyane/core/bytes.hpp"
#include "cyane/core/error.hpp"

namespace cyane::net {

enum class IoStatus : std::uint8_t {
    ok,
    would_block,
    closed,
    error,
};

// 热路径不构造 Error 字符串：EAGAIN 是常态而非错误
struct IoResult {
    IoStatus status{IoStatus::error};
    std::size_t bytes{0};
};

[[nodiscard]] inline std::unexpected<Error> socket_error(std::string_view what) {
    return make_error(ErrorCode::net, std::format("{}: {}", what, std::strerror(errno)));
}

class Socket {
public:
    Socket() noexcept = default;
    explicit Socket(int fd) noexcept : fd_{fd} {}

    ~Socket() { close(); }

    Socket(Socket&& other) noexcept : fd_{std::exchange(other.fd_, -1)} {}

    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    [[nodiscard]] static Result<Socket> listen_tcp(
        std::string_view bind_address, std::uint16_t port, int backlog = 512, bool reuse_port = false) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;

        const std::string address{bind_address};
        const std::string service = std::to_string(port);
        addrinfo* resolved = nullptr;
        const int status = ::getaddrinfo(address.empty() ? nullptr : address.c_str(),
                                        service.c_str(),
                                        &hints,
                                        &resolved);
        if (status != 0) {
            return make_error(ErrorCode::net, std::format("cannot resolve {}:{}: {}", address, port, ::gai_strerror(status)));
        }
        const struct AddressGuard {
            addrinfo* node;
            ~AddressGuard() { ::freeaddrinfo(node); }
        } guard{resolved};

        for (addrinfo* node = resolved; node != nullptr; node = node->ai_next) {
            const int fd = ::socket(node->ai_family, node->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, node->ai_protocol);
            if (fd < 0) {
                continue;
            }
            Socket candidate{fd};
            const int enable = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
            if (reuse_port) {
                ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(enable));
            }
            if (node->ai_family == AF_INET6) {
                ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &enable, sizeof(enable));
            }
            if (::bind(fd, node->ai_addr, static_cast<socklen_t>(node->ai_addrlen)) != 0) {
                continue;
            }
            if (::listen(fd, backlog) != 0) {
                continue;
            }
            return candidate;
        }
        return socket_error(std::format("cannot listen on {}:{}", address, port));
    }

    [[nodiscard]] Result<Socket> accept(std::string& peer_out) noexcept {
        sockaddr_storage peer{};
        socklen_t peer_length = sizeof(peer);
        const int fd = ::accept4(fd_, reinterpret_cast<sockaddr*>(&peer), &peer_length, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            return socket_error("accept");
        }
        char host[NI_MAXHOST]{};
        char service[NI_MAXSERV]{};
        if (::getnameinfo(reinterpret_cast<sockaddr*>(&peer),
                          peer_length,
                          host,
                          sizeof(host),
                          service,
                          sizeof(service),
                          NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
            peer_out.assign(host);
        } else {
            peer_out.clear();
        }
        Socket accepted{fd};
        const int enable = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable));
        return accepted;
    }

    [[nodiscard]] IoResult try_read(MutableByteSpan buffer) noexcept {
        const ssize_t count = ::recv(fd_, buffer.data(), buffer.size(), 0);
        if (count > 0) {
            return IoResult{IoStatus::ok, static_cast<std::size_t>(count)};
        }
        if (count == 0) {
            return IoResult{IoStatus::closed, 0};
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return IoResult{IoStatus::would_block, 0};
        }
        if (errno == EINTR) {
            return IoResult{IoStatus::ok, 0};
        }
        return IoResult{IoStatus::error, 0};
    }

    [[nodiscard]] IoResult try_write(ByteSpan buffer) noexcept {
        const ssize_t count = ::send(fd_, buffer.data(), buffer.size(), MSG_NOSIGNAL);
        if (count >= 0) {
            return IoResult{IoStatus::ok, static_cast<std::size_t>(count)};
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return IoResult{IoStatus::would_block, 0};
        }
        if (errno == EINTR) {
            return IoResult{IoStatus::ok, 0};
        }
        return IoResult{IoStatus::error, 0};
    }

    [[nodiscard]] int fd() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

    // 绑定端口为 0 时由内核分配，测试与服务端日志都需要读回真实端口
    [[nodiscard]] std::uint16_t local_port() const noexcept {
        sockaddr_storage address{};
        socklen_t length = sizeof(address);
        if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            return 0;
        }
        if (address.ss_family == AF_INET) {
            return ntohs(reinterpret_cast<const sockaddr_in*>(&address)->sin_port);
        }
        if (address.ss_family == AF_INET6) {
            return ntohs(reinterpret_cast<const sockaddr_in6*>(&address)->sin6_port);
        }
        return 0;
    }

    void close() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_{-1};
};

}
