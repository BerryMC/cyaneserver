#pragma once

#include <memory>

#include "cyane/core/bytes.hpp"
#include "cyane/core/error.hpp"

namespace cyane::crypto {

// AES-128-CFB8 流密码，IV = 密钥本身（协议 340 约定，见 NetworkManager.a(SecretKey)）
class StreamCipher {
public:
    StreamCipher() = default;

    [[nodiscard]] static Result<StreamCipher> aes_cfb8(ByteSpan key, bool encrypt);

    [[nodiscard]] Result<void> update(MutableByteSpan data);

private:
    struct CtxDeleter {
        void operator()(void* ctx) const noexcept;
    };
    std::unique_ptr<void, CtxDeleter> ctx_;
};

}
