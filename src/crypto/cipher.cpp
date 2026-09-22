#include "cyane/crypto/cipher.hpp"

#include <openssl/evp.h>

#include <format>
#include <utility>

namespace cyane::crypto {

void StreamCipher::CtxDeleter::operator()(void* ctx) const noexcept {
    EVP_CIPHER_CTX_free(static_cast<EVP_CIPHER_CTX*>(ctx));
}

Result<StreamCipher> StreamCipher::aes_cfb8(ByteSpan key, bool encrypt) {
    if (key.size() != 16) {
        return make_error(ErrorCode::protocol,
                          std::format("AES-128 session key must be 16 bytes, got {}", key.size()));
    }
    EVP_CIPHER_CTX* raw = EVP_CIPHER_CTX_new();
    if (raw == nullptr) {
        return make_error(ErrorCode::jvm, "EVP_CIPHER_CTX_new failed");
    }
    StreamCipher cipher;
    cipher.ctx_.reset(raw);
    // IV = 密钥本身；CFB8 是字节流，update 可任意长度原地处理
    if (EVP_CipherInit_ex(raw, EVP_aes_128_cfb8(), nullptr,
                          reinterpret_cast<const unsigned char*>(key.data()),
                          reinterpret_cast<const unsigned char*>(key.data()), encrypt ? 1 : 0) != 1) {
        return make_error(ErrorCode::protocol, "EVP_CipherInit_ex failed");
    }
    return cipher;
}

Result<void> StreamCipher::update(MutableByteSpan data) {
    EVP_CIPHER_CTX* raw = static_cast<EVP_CIPHER_CTX*>(ctx_.get());
    if (raw == nullptr) {
        return make_error(ErrorCode::protocol, "cipher not initialized");
    }
    int written = 0;
    if (EVP_CipherUpdate(raw, reinterpret_cast<unsigned char*>(data.data()), &written,
                         reinterpret_cast<const unsigned char*>(data.data()), static_cast<int>(data.size())) != 1) {
        return make_error(ErrorCode::protocol, "EVP_CipherUpdate failed");
    }
    if (written != static_cast<int>(data.size())) {
        return make_error(ErrorCode::protocol,
                          std::format("cipher produced {} bytes for {} input", written, data.size()));
    }
    return {};
}

}
