#include "cyane/crypto/rsa.hpp"

#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <cstring>
#include <format>
#include <utility>

namespace cyane::crypto {

void RsaKeyPair::PkeyDeleter::operator()(void* key) const noexcept {
    EVP_PKEY_free(static_cast<EVP_PKEY*>(key));
}

Result<RsaKeyPair> RsaKeyPair::generate(unsigned bits) {
    EVP_PKEY_CTX* raw_ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (raw_ctx == nullptr) {
        return make_error(ErrorCode::protocol, "EVP_PKEY_CTX_new_id failed");
    }
    const std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx{raw_ctx, EVP_PKEY_CTX_free};
    if (EVP_PKEY_keygen_init(raw_ctx) != 1 || EVP_PKEY_CTX_set_rsa_keygen_bits(raw_ctx, static_cast<int>(bits)) != 1) {
        return make_error(ErrorCode::protocol, "RSA keygen init failed");
    }
    EVP_PKEY* raw_key = nullptr;
    if (EVP_PKEY_keygen(raw_ctx, &raw_key) != 1 || raw_key == nullptr) {
        return make_error(ErrorCode::protocol, "RSA keygen failed");
    }

    RsaKeyPair pair;
    pair.key_.reset(raw_key);

    // 导出 X.509 SubjectPublicKeyInfo DER（协议 340 约定）
    const std::unique_ptr<BIO, decltype(&BIO_free)> bio{BIO_new(BIO_s_mem()), BIO_free};
    if (bio == nullptr || i2d_PUBKEY_bio(bio.get(), raw_key) != 1) {
        return make_error(ErrorCode::protocol, "i2d_PUBKEY_bio failed");
    }
    const char* data_ptr = nullptr;
    const long der_length = BIO_get_mem_data(bio.get(), &data_ptr);
    if (der_length <= 0) {
        return make_error(ErrorCode::protocol, "BIO_get_mem_data failed");
    }
    pair.public_der_.resize(static_cast<std::size_t>(der_length));
    std::memcpy(pair.public_der_.data(), data_ptr, static_cast<std::size_t>(der_length));
    return pair;
}

Result<Bytes> RsaKeyPair::decrypt(ByteSpan ciphertext) const {
    EVP_PKEY* raw = static_cast<EVP_PKEY*>(key_.get());
    if (raw == nullptr) {
        return make_error(ErrorCode::protocol, "keypair not initialized");
    }
    EVP_PKEY_CTX* raw_ctx = EVP_PKEY_CTX_new(raw, nullptr);
    if (raw_ctx == nullptr) {
        return make_error(ErrorCode::protocol, "EVP_PKEY_CTX_new failed");
    }
    const std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx{raw_ctx, EVP_PKEY_CTX_free};
    if (EVP_PKEY_decrypt_init(raw_ctx) != 1 ||
        EVP_PKEY_CTX_set_rsa_padding(raw_ctx, RSA_PKCS1_PADDING) != 1) {
        return make_error(ErrorCode::protocol, "decrypt init failed");
    }
    std::size_t output_length = 0;
    if (EVP_PKEY_decrypt(raw_ctx, nullptr, &output_length,
                         reinterpret_cast<const unsigned char*>(ciphertext.data()), ciphertext.size()) != 1) {
        return make_error(ErrorCode::protocol, "decrypt sizing failed");
    }
    Bytes output(output_length);
    if (EVP_PKEY_decrypt(raw_ctx, reinterpret_cast<unsigned char*>(output.data()), &output_length,
                         reinterpret_cast<const unsigned char*>(ciphertext.data()), ciphertext.size()) != 1) {
        return make_error(ErrorCode::protocol, "decrypt failed");
    }
    output.resize(output_length);
    return output;
}

Result<Bytes> RsaKeyPair::encrypt(ByteSpan plaintext) const {
    EVP_PKEY* raw = static_cast<EVP_PKEY*>(key_.get());
    if (raw == nullptr) {
        return make_error(ErrorCode::protocol, "keypair not initialized");
    }
    EVP_PKEY_CTX* raw_ctx = EVP_PKEY_CTX_new(raw, nullptr);
    if (raw_ctx == nullptr) {
        return make_error(ErrorCode::protocol, "EVP_PKEY_CTX_new failed");
    }
    const std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx{raw_ctx, EVP_PKEY_CTX_free};
    if (EVP_PKEY_encrypt_init(raw_ctx) != 1 ||
        EVP_PKEY_CTX_set_rsa_padding(raw_ctx, RSA_PKCS1_PADDING) != 1) {
        return make_error(ErrorCode::protocol, "encrypt init failed");
    }
    std::size_t output_length = 0;
    if (EVP_PKEY_encrypt(raw_ctx, nullptr, &output_length,
                         reinterpret_cast<const unsigned char*>(plaintext.data()), plaintext.size()) != 1) {
        return make_error(ErrorCode::protocol, "encrypt sizing failed");
    }
    Bytes output(output_length);
    if (EVP_PKEY_encrypt(raw_ctx, reinterpret_cast<unsigned char*>(output.data()), &output_length,
                         reinterpret_cast<const unsigned char*>(plaintext.data()), plaintext.size()) != 1) {
        return make_error(ErrorCode::protocol, "encrypt failed");
    }
    output.resize(output_length);
    return output;
}
}
