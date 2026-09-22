#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <zlib.h>

#include "cyane/core/bytes.hpp"

namespace cyane::test {

// 阻塞式测试客户端：只用于端到端验证服务端行为，不参与服务端代码路径
class TestClient {
public:
    TestClient() noexcept = default;

    ~TestClient() { close(); }

    TestClient(TestClient&& other) noexcept : fd_{other.fd_} { other.fd_ = -1; }

    TestClient& operator=(TestClient&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    TestClient(const TestClient&) = delete;
    TestClient& operator=(const TestClient&) = delete;

    [[nodiscard]] static std::optional<TestClient> connect(std::string_view host, std::uint16_t port) {
        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return std::nullopt;
        }
        TestClient client{fd};
        const int enable = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable));
        timeval timeout{};
        timeout.tv_sec = 5;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        if (::inet_pton(AF_INET, std::string{host}.c_str(), &address.sin_addr) != 1) {
            return std::nullopt;
        }
        if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            return std::nullopt;
        }
        return client;
    }

    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

    [[nodiscard]] bool send_bytes(ByteSpan bytes) const {
        std::size_t sent = 0;
        while (sent < bytes.size()) {
            const ssize_t count = ::send(fd_, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
            if (count <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(count);
        }
        return true;
    }

    [[nodiscard]] std::optional<Bytes> receive_packet() {
        auto length = read_varint();
        if (!length || *length < 0) {
            return std::nullopt;
        }
        Bytes body(static_cast<std::size_t>(*length));
        if (!read_exact(MutableByteSpan{body})) {
            return std::nullopt;
        }
        return body;
    }

    // Read a frame that uses the Minecraft compression format (sent after
    // SetCompression). Each frame has a trailing data_length varint: if it is
    // 0 the payload is plaintext, otherwise it is zlib-compressed.
    [[nodiscard]] std::optional<Bytes> receive_compressed_packet() {
        auto length = read_varint();
        if (!length || *length < 0) {
            return std::nullopt;
        }
        Bytes body(static_cast<std::size_t>(*length));
        if (!read_exact(MutableByteSpan{body})) {
            return std::nullopt;
        }
        ByteReader reader{ByteSpan{body}};
        auto data_length = reader.varint();
        if (!data_length) {
            return std::nullopt;
        }
        if (*data_length == 0) {
            // Uncompressed payload.
            return Bytes{reader.rest().begin(), reader.rest().end()};
        }
        // Compressed payload — decompress with zlib.
        Bytes decompressed(static_cast<std::size_t>(*data_length));
        uLongf dest_len = static_cast<uLongf>(decompressed.size());
        const auto result = ::uncompress(
            reinterpret_cast<Bytef*>(decompressed.data()), &dest_len,
            reinterpret_cast<const Bytef*>(reader.rest().data()),
            static_cast<uLong>(reader.rest().size()));
        if (result != Z_OK || dest_len != decompressed.size()) {
            return std::nullopt;
        }
        return decompressed;
    }

    [[nodiscard]] std::optional<Bytes> receive_raw(std::size_t count) {
        Bytes out(count);
        if (!read_exact(MutableByteSpan{out})) {
            return std::nullopt;
        }
        return out;
    }

    void close() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    explicit TestClient(int fd) noexcept : fd_{fd} {}

    [[nodiscard]] std::optional<std::int32_t> read_varint() {
        std::int32_t value = 0;
        for (int shift = 0; shift < 35; shift += 7) {
            std::uint8_t byte = 0;
            if (::recv(fd_, &byte, 1, 0) != 1) {
                return std::nullopt;
            }
            value |= static_cast<std::int32_t>(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0) {
                return value;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] bool read_exact(MutableByteSpan buffer) const {
        std::size_t filled = 0;
        while (filled < buffer.size()) {
            const ssize_t count = ::recv(fd_, buffer.data() + filled, buffer.size() - filled, 0);
            if (count <= 0) {
                return false;
            }
            filled += static_cast<std::size_t>(count);
        }
        return true;
    }

    int fd_{-1};
};

}
