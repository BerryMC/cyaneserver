#include "cyane/crypto/digest.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <cstdint>
#include <memory>

namespace cyane::crypto {
namespace {

[[nodiscard]] Bytes digest(std::string_view algorithm, std::initializer_list<ByteSpan> parts) {
    EVP_MD_CTX* raw = EVP_MD_CTX_new();
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx{raw, EVP_MD_CTX_free};
    const EVP_MD* md = EVP_get_digestbyname(std::string{algorithm}.c_str());
    if (md == nullptr || EVP_DigestInit_ex(raw, md, nullptr) != 1) {
        return {};
    }
    for (const ByteSpan part : parts) {
        EVP_DigestUpdate(raw, part.data(), part.size());
    }
    Bytes out(static_cast<std::size_t>(EVP_MD_get_size(md)));
    unsigned int written = 0;
    EVP_DigestFinal_ex(raw, reinterpret_cast<unsigned char*>(out.data()), &written);
    out.resize(written);
    return out;
}

[[nodiscard]] std::string positive_hex(ByteSpan bytes) {
    std::size_t first = 0;
    while (first < bytes.size() && std::to_integer<std::uint8_t>(bytes[first]) == 0) {
        ++first;
    }
    if (first == bytes.size()) {
        return "0";
    }
    constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve((bytes.size() - first) * 2);
    for (const std::byte raw : bytes.subspan(first)) {
        const auto value = std::to_integer<std::uint8_t>(raw);
        out.push_back(kHex[value >> 4]);
        out.push_back(kHex[value & 0x0F]);
    }
    return out;
}

[[nodiscard]] int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

}

std::string server_id(ByteSpan shared_secret, ByteSpan public_der) {
    // LoginListener$3: sha1(serverId 空串 + 共享密钥 + 公钥) 取正数十六进制
    return positive_hex(digest("SHA-1", {as_bytes(""), shared_secret, public_der}));
}

std::string offline_uuid(std::string_view username) {
    const std::string input = std::string{"OfflinePlayer:"} + std::string{username};
    Bytes digest_bytes = digest("MD5", {as_bytes(input)});
    std::array<std::uint8_t, 16> uuid{};
    if (digest_bytes.size() != uuid.size()) {
        return {};
    }
    for (std::size_t index = 0; index < uuid.size(); ++index) {
        uuid[index] = std::to_integer<std::uint8_t>(digest_bytes[index]);
    }
    uuid[6] = static_cast<std::uint8_t>((uuid[6] & 0x0F) | 0x30);
    uuid[8] = static_cast<std::uint8_t>((uuid[8] & 0x3F) | 0x80);

    constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (std::size_t index = 0; index < uuid.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            out.push_back('-');
        }
        out.push_back(kHex[uuid[index] >> 4]);
        out.push_back(kHex[uuid[index] & 0x0F]);
    }
    return out;
}

std::string uuid_with_dashes(std::string_view compact) {
    std::string out;
    out.reserve(36);
    for (std::size_t index = 0; index < compact.size(); ++index) {
        if (index == 8 || index == 12 || index == 16 || index == 20) {
            out.push_back('-');
        }
        out.push_back(compact[index]);
    }
    return out;
}

std::array<std::uint8_t, 16> parse_uuid_string(std::string_view dashed) {
    std::array<std::uint8_t, 16> out{};
    std::size_t read = 0;
    for (std::size_t index = 0; index < dashed.size() && read < 16; ++index) {
        const char ch = dashed[index];
        if (ch == '-') {
            continue;
        }
        const int hi = hex_value(ch);
        if (hi < 0) {
            return {};
        }
        ++index;
        if (index >= dashed.size()) {
            return {};
        }
        const int lo = hex_value(dashed[index]);
        if (lo < 0) {
            return {};
        }
        out[read++] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return read == 16 ? out : std::array<std::uint8_t, 16>{};
}

Bytes random_bytes(std::size_t count) {
    Bytes out(count);
    if (RAND_bytes(reinterpret_cast<unsigned char*>(out.data()), static_cast<int>(count)) != 1) {
        out.clear();
    }
    return out;
}

}
