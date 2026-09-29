#include "cyane/net/session_service.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <format>
#include <memory>
#include <string>

#include "cyane/core/error.hpp"
#include "cyane/core/log.hpp"

namespace cyane::net {
namespace {

[[nodiscard]] std::string url_encode(std::string_view text) {
    constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.') {
            out.push_back(static_cast<char>(c));
        } else {
            out += "%";
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

struct SslContextDeleter {
    void operator()(SSL_CTX* ctx) const noexcept { SSL_CTX_free(ctx); }
};

struct SslDeleter {
    void operator()(SSL* ssl) const noexcept { SSL_free(ssl); }
};

// RAII 关闭描述符：https_get 的错误路径全部直接返回
struct FdGuard {
    explicit FdGuard(int f) noexcept : fd{f} {}
    ~FdGuard() { ::close(fd); }
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;
    int fd;
};

// 阻塞式最小 HTTPS GET：仅在线验证使用，一次请求一个连接
[[nodiscard]] Result<std::string> https_get(
    std::string_view host, std::string_view path, std::chrono::milliseconds timeout) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* resolved = nullptr;
    const std::string host_text{host};
    if (::getaddrinfo(host_text.c_str(), "443", &hints, &resolved) != 0) {
        return make_error(ErrorCode::net, "cannot resolve " + host_text);
    }
    const std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> guard{resolved, ::freeaddrinfo};

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return make_error(ErrorCode::net, "socket failed");
    }
    const FdGuard fd_guard{fd};

    timeval tv{};
    tv.tv_sec = static_cast<time_t>(timeout.count() / 1000);
    tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (::connect(fd, resolved->ai_addr, static_cast<socklen_t>(resolved->ai_addrlen)) != 0) {
        return make_error(ErrorCode::net, std::format("connect to {} failed: {}", host_text, std::strerror(errno)));
    }

    const std::unique_ptr<SSL_CTX, SslContextDeleter> ctx{SSL_CTX_new(TLS_client_method())};
    const std::unique_ptr<SSL, SslDeleter> ssl{SSL_new(ctx.get())};
    if (ctx == nullptr || ssl == nullptr) {
        return make_error(ErrorCode::net, "TLS context creation failed");
    }
    SSL_set_fd(ssl.get(), fd);
    SSL_set_tlsext_host_name(ssl.get(), host_text.c_str());
    if (SSL_connect(ssl.get()) != 1) {
        return make_error(ErrorCode::net, "TLS handshake with " + host_text + " failed");
    }

    const std::string request = std::format("GET {} HTTP/1.1\r\nHost: {}\r\nConnection: close\r\n\r\n", path, host_text);
    if (SSL_write(ssl.get(), request.data(), static_cast<int>(request.size())) <= 0) {
        return make_error(ErrorCode::net, "request write failed");
    }

    std::string response;
    std::array<char, 4096> buffer{};
    for (;;) {
        const int count = SSL_read(ssl.get(), buffer.data(), static_cast<int>(buffer.size()));
        if (count > 0) {
            response.append(buffer.data(), static_cast<std::size_t>(count));
            continue;
        }
        if (count == 0) {
            break;
        }
        const int error = SSL_get_error(ssl.get(), count);
        if (error == SSL_ERROR_ZERO_RETURN || error == SSL_ERROR_SYSCALL) {
            break;
        }
        return make_error(ErrorCode::net, std::format("response read failed (SSL error {})", error));
    }

    const auto header_end = response.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return make_error(ErrorCode::net, "malformed HTTP response");
    }
    const std::string_view head{response.data(), header_end};
    if (head.find("HTTP/1.1 200") == std::string_view::npos && head.find("HTTP/1.0 200") == std::string_view::npos) {
        return make_error(ErrorCode::net, std::format("unexpected status: {}", head.substr(0, 32)));
    }
    return response.substr(header_end + 4);
}

}

std::optional<std::string> MojangSessionService::has_joined(
    std::string_view username, std::string_view server_id, std::string_view ip) const {
    const std::string path = std::format("/session/minecraft/hasJoined?username={}&serverId={}&ip={}",
                                         url_encode(username),
                                         url_encode(server_id),
                                         url_encode(ip));
    auto body = https_get("sessionserver.mojang.com", path, timeout);
    if (!body) {
        log::debug("hasJoined failed: {}", body.error().message);
        return std::nullopt;
    }
    // {"id":"<32位hex>","name":"..."}
    const std::string_view marker{"\"id\":\""};
    const auto start = body->find(marker);
    if (start == std::string::npos) {
        return std::nullopt;
    }
    const auto id_start = start + marker.size();
    const auto id_end = body->find('"', id_start);
    if (id_end == std::string::npos || id_end - id_start != 32) {
        return std::nullopt;
    }
    return body->substr(id_start, 32);
}

}
