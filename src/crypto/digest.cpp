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

}

std::string server_id(ByteSpan shared_secret, ByteSpan public_der) {
    // LoginListener$3: sha1(serverId 空串 + 共享密钥 + 公钥) 取正数十六进制
    return positive_hex(digest("SHA-1", {as_bytes(""), shared_secret, public_der}));
}

cyane::Uuid offline_uuid(std::string_view username) {
    // Spigot/Paper 离线 UUID：md5("OfflinePlayer:"+username) → RFC 4122 version 3
    const std::string input = std::string{"OfflinePlayer:"} + std::string{username};
    const Bytes digest_bytes = digest("MD5", {as_bytes(input)});
    cyane::Uuid::Bytes md5{};
    if (digest_bytes.size() != md5.size()) {
        return {};
    }
    for (std::size_t index = 0; index < md5.size(); ++index) {
        md5[index] = std::to_integer<std::uint8_t>(digest_bytes[index]);
    }
    return cyane::Uuid::md5_v3(md5);
}

Bytes random_bytes(std::size_t count) {
    Bytes out(count);
    if (RAND_bytes(reinterpret_cast<unsigned char*>(out.data()), static_cast<int>(count)) != 1) {
        out.clear();
    }
    return out;
}

}
