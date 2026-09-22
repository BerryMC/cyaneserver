#pragma once

#include <memory>

#include "cyane/core/bytes.hpp"
#include "cyane/core/error.hpp"

namespace cyane::crypto {

// 服务端 RSA-1024 密钥对：公开密钥以 X.509 SPKI DER 导出（162 字节），私钥用于解密会话密钥
class RsaKeyPair {
public:
    [[nodiscard]] static Result<RsaKeyPair> generate(unsigned bits = 1024);

    [[nodiscard]] Result<Bytes> decrypt(ByteSpan ciphertext) const;
    [[nodiscard]] Result<Bytes> encrypt(ByteSpan plaintext) const;
    [[nodiscard]] const Bytes& public_der() const noexcept { return public_der_; }

private:
    struct PkeyDeleter {
        void operator()(void* key) const noexcept;
    };
    std::unique_ptr<void, PkeyDeleter> key_;
    Bytes public_der_;
};

}
