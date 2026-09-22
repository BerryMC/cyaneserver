#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "cyane/core/bytes.hpp"
#include "cyane/crypto/cipher.hpp"
#include "cyane/crypto/digest.hpp"
#include "cyane/crypto/rsa.hpp"
#include "test_framework.hpp"

namespace {
constexpr std::array<std::byte, 16> kAesKey = {
    std::byte{0x00}, std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
    std::byte{0x04}, std::byte{0x05}, std::byte{0x06}, std::byte{0x07},
    std::byte{0x08}, std::byte{0x09}, std::byte{0x0a}, std::byte{0x0b},
    std::byte{0x0c}, std::byte{0x0d}, std::byte{0x0e}, std::byte{0x0f}};
constexpr std::array<std::byte, 16> kAesIv = {
    std::byte{0x00}, std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
    std::byte{0x04}, std::byte{0x05}, std::byte{0x06}, std::byte{0x07},
    std::byte{0x08}, std::byte{0x09}, std::byte{0x0a}, std::byte{0x0b},
    std::byte{0x0c}, std::byte{0x0d}, std::byte{0x0e}, std::byte{0x0f}};
constexpr std::array<std::byte, 16> kAesPlaintext = {
    std::byte{0x00}, std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
    std::byte{0x04}, std::byte{0x05}, std::byte{0x06}, std::byte{0x07},
    std::byte{0x08}, std::byte{0x09}, std::byte{0x0a}, std::byte{0x0b},
    std::byte{0x0c}, std::byte{0x0d}, std::byte{0x0e}, std::byte{0x0f}};
constexpr std::array<std::byte, 16> kAesExpected = {
    std::byte{0x0a}, std::byte{0x22}, std::byte{0xf7}, std::byte{0x96},
    std::byte{0xe1}, std::byte{0xb9}, std::byte{0x3e}, std::byte{0x90},
    std::byte{0x32}, std::byte{0xcf}, std::byte{0xf8}, std::byte{0x04},
    std::byte{0x83}, std::byte{0x8a}, std::byte{0xdf}, std::byte{0xc3}};
}

CYANE_TEST(cipher_aes_cfb8_roundtrip) {
    auto encrypt = cyane::crypto::StreamCipher::aes_cfb8(kAesKey, true);
    auto decrypt = cyane::crypto::StreamCipher::aes_cfb8(kAesIv, false);
    CYANE_CHECK(encrypt && decrypt);

    cyane::Bytes encrypted(kAesPlaintext.begin(), kAesPlaintext.end());
    CYANE_CHECK(encrypt->update(cyane::MutableByteSpan{encrypted}));
    CYANE_CHECK(std::equal(encrypted.begin(), encrypted.end(), kAesExpected.begin()));

    cyane::Bytes decrypted(kAesExpected.begin(), kAesExpected.end());
    CYANE_CHECK(decrypt->update(cyane::MutableByteSpan{decrypted}));
    CYANE_CHECK(std::equal(decrypted.begin(), decrypted.end(), kAesPlaintext.begin()));
}

CYANE_TEST(cipher_aes_cfb8_empty_input) {
    auto encrypt = cyane::crypto::StreamCipher::aes_cfb8(kAesKey, true);
    CYANE_CHECK(encrypt);
}

CYANE_TEST(rsa_generate_and_decrypt) {
    auto key_pair = cyane::crypto::RsaKeyPair::generate(1024);
    CYANE_CHECK(key_pair);

    cyane::Bytes plaintext = {std::byte{'H'}, std::byte{'e'}, std::byte{'l'}, std::byte{'l'}, std::byte{'o'}};
    auto encrypted = key_pair->encrypt(plaintext);
    CYANE_CHECK(encrypted && !encrypted->empty());

    auto decrypted = key_pair->decrypt(*encrypted);
    CYANE_CHECK(decrypted && decrypted->size() == plaintext.size());
    CYANE_CHECK_EQ(*decrypted, plaintext);
}

CYANE_TEST(rsa_padding_is_pkcs1) {
    auto key_pair = cyane::crypto::RsaKeyPair::generate(1024);
    CYANE_CHECK(key_pair);

    cyane::Bytes plaintext = {std::byte{'T'}, std::byte{'e'}, std::byte{'s'}, std::byte{'t'}};
    auto enc1 = key_pair->encrypt(plaintext);
    auto enc2 = key_pair->encrypt(plaintext);
    CYANE_CHECK(enc1 && enc2);
    CYANE_CHECK_EQ(enc1->size(), enc2->size());
    auto dec1 = key_pair->decrypt(*enc1);
    auto dec2 = key_pair->decrypt(*enc2);
    CYANE_CHECK(dec1 && dec2);
    CYANE_CHECK_EQ(*dec1, plaintext);
    CYANE_CHECK_EQ(*dec2, plaintext);
}

CYANE_TEST(rsa_1024_output_size) {
    auto key_pair = cyane::crypto::RsaKeyPair::generate(1024);
    CYANE_CHECK(key_pair);
    auto encrypted = key_pair->encrypt({});
    CYANE_CHECK(encrypted && encrypted->size() == 128);
}
