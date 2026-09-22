#include <cstdint>
#include <string>
#include <string_view>

#include "cyane/proto/frame.hpp"
#include "test_framework.hpp"

namespace {

[[nodiscard]] cyane::Bytes make_body(std::string_view text) {
    cyane::ByteWriter writer;
    writer.varint(0x42);
    writer.string(text);
    return writer.take();
}

}

CYANE_TEST(frame_round_trips_without_compression) {
    const auto body = make_body("hello cyane");
    cyane::Bytes frame;
    frame.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(body.size())));
    cyane::append(frame, body);

    const auto frame_body = cyane::ByteSpan{frame}.subspan(1);
    cyane::Bytes scratch;
    const auto decoded = cyane::proto::decode_frame(frame_body, scratch, -1);
    CYANE_CHECK(decoded.has_value());
    CYANE_CHECK_EQ(decoded->packet_id, 0x42);

    cyane::ByteReader reader{decoded->payload};
    CYANE_CHECK_EQ(reader.string().value_or(""), std::string{"hello cyane"});
}

CYANE_TEST(frame_encode_decode_round_trip) {
    for (const std::string_view text : {std::string_view{"tiny"}, std::string_view{std::string(4000, 'z')}}) {
        cyane::ByteWriter fields;
        fields.string(text);

        cyane::Bytes frame;
        cyane::proto::encode_frame(frame, 0x23, fields.data(), -1);

        cyane::ByteReader header{cyane::ByteSpan{frame}};
        const auto length = header.varint();
        CYANE_CHECK(length.has_value());
        CYANE_CHECK_EQ(static_cast<std::size_t>(length.value_or(0)), header.remaining());

        cyane::Bytes scratch;
        const auto decoded = cyane::proto::decode_frame(header.rest(), scratch, -1);
        CYANE_CHECK(decoded.has_value());
        CYANE_CHECK_EQ(decoded->packet_id, 0x23);
        cyane::ByteReader payload{decoded->payload};
        CYANE_CHECK_EQ(payload.string().value_or(""), std::string{text});
    }
}

CYANE_TEST(frame_compression_round_trips_large_payload) {
    const std::string text(8192, 'q');
    cyane::ByteWriter fields;
    fields.string(text);

    cyane::Bytes frame;
    cyane::proto::encode_frame(frame, 0x20, fields.data(), 256);

    cyane::ByteReader header{cyane::ByteSpan{frame}};
    const auto length = header.varint();
    CYANE_CHECK(length.has_value());

    cyane::Bytes scratch;
    const auto decoded = cyane::proto::decode_frame(header.rest(), scratch, 256);
    CYANE_CHECK(decoded.has_value());
    CYANE_CHECK_EQ(decoded->packet_id, 0x20);
    cyane::ByteReader payload{decoded->payload};
    CYANE_CHECK_EQ(payload.string().value_or(""), text);
    CYANE_CHECK(frame.size() < text.size());
}

CYANE_TEST(frame_sends_below_threshold_uncompressed) {
    cyane::ByteWriter fields;
    fields.string("short");

    cyane::Bytes frame;
    cyane::proto::encode_frame(frame, 0x03, fields.data(), 256);

    cyane::ByteReader header{cyane::ByteSpan{frame}};
    const auto length = header.varint();
    CYANE_CHECK(length.has_value());
    CYANE_CHECK_EQ(*length, static_cast<std::int32_t>(header.rest().size()));

    cyane::ByteReader probe{header.rest()};
    CYANE_CHECK_EQ(probe.varint().value_or(-1), 0);

    cyane::Bytes scratch;
    const auto decoded = cyane::proto::decode_frame(header.rest(), scratch, 256);
    CYANE_CHECK(decoded.has_value());
    CYANE_CHECK_EQ(decoded->packet_id, 0x03);
}

CYANE_TEST(frame_rejects_declared_length_below_threshold) {
    cyane::Bytes body;
    cyane::append_varint(body, 100);
    cyane::append_varint(body, 0x00);

    cyane::Bytes scratch;
    const auto decoded = cyane::proto::decode_frame(cyane::ByteSpan{body}, scratch, 256);
    CYANE_CHECK(!decoded.has_value());
    CYANE_CHECK(decoded.error().code == cyane::ErrorCode::protocol);
}

CYANE_TEST(frame_rejects_negative_uncompressed_length) {
    cyane::Bytes body;
    cyane::append_varint(body, -1);
    cyane::append_varint(body, 0x00);

    cyane::Bytes scratch;
    CYANE_CHECK(!cyane::proto::decode_frame(cyane::ByteSpan{body}, scratch, 256).has_value());
}

CYANE_TEST(frame_rejects_inflated_size_over_limit) {
    cyane::Bytes body;
    cyane::append_varint(body, cyane::proto::kMaxFrameBytes + 1024);
    cyane::append_varint(body, 0x00);

    cyane::Bytes scratch;
    const auto decoded = cyane::proto::decode_frame(cyane::ByteSpan{body}, scratch, 256);
    CYANE_CHECK(!decoded.has_value());
}

CYANE_TEST(frame_rejects_corrupt_compressed_payload) {
    cyane::Bytes body;
    cyane::append_varint(body, 1024);
    for (int i = 0; i < 32; ++i) {
        body.push_back(static_cast<std::byte>(0x5A));
    }

    cyane::Bytes scratch;
    CYANE_CHECK(!cyane::proto::decode_frame(cyane::ByteSpan{body}, scratch, 256).has_value());
}

CYANE_TEST(frame_deflate_inflate_round_trips_binary) {
    cyane::Bytes input;
    input.reserve(5000);
    for (int index = 0; index < 5000; ++index) {
        input.push_back(static_cast<std::byte>(index % 251));
    }
    auto compressed = cyane::proto::deflate(cyane::ByteSpan{input}, 6);
    CYANE_CHECK(compressed.has_value());
    auto restored = cyane::proto::inflate(cyane::ByteSpan{*compressed}, input.size());
    CYANE_CHECK(restored.has_value());
    CYANE_CHECK_EQ(restored->size(), input.size());
    CYANE_CHECK(std::equal(restored->begin(), restored->end(), input.begin()));
}
