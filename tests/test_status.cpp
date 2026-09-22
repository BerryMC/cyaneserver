#include <cstdint>
#include <string>

#include "cyane/game/status.hpp"
#include "cyane/proto/frame.hpp"
#include "cyane/proto/packet_ids.hpp"
#include "test_framework.hpp"

namespace {

[[nodiscard]] std::string to_hex(cyane::ByteSpan bytes) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::byte raw : bytes) {
        const auto value = std::to_integer<std::uint8_t>(raw);
        out.push_back(kHex[(value >> 4) & 0x0F]);
        out.push_back(kHex[value & 0x0F]);
    }
    return out;
}

// 原版 1.12.2 服务端（motd=OracleServer, max=5, online=0）的 Server List Ping 响应**包体**（不含长度前缀）原文
constexpr std::string_view kVanillaStatusHex =
    "00717b226465736372697074696f6e223a7b2274657874223a224f7261636c65536572766572227d2c22706c6179657273223a7b226d6178223a"
    "352c226f6e6c696e65223a307d2c2276657273696f6e223a7b226e616d65223a22312e31322e32222c2270726f746f636f6c223a3334307d7d";

}

CYANE_TEST(status_response_is_byte_identical_to_vanilla_1_12_2) {
    const cyane::game::ServerStatus status{"OracleServer", 5};
    const std::string json = status.build_status_json();

    cyane::ByteWriter fields;
    fields.string(json);
    cyane::Bytes frame;
    cyane::proto::encode_frame(frame, cyane::proto::status_cb::kResponse, fields.data(), -1);

    cyane::ByteReader reader{cyane::ByteSpan{frame}};
    const auto length = reader.varint();
    CYANE_CHECK(length.has_value());
    const auto body = reader.rest().first(static_cast<std::size_t>(length.value_or(0)));
    CYANE_CHECK_EQ(to_hex(body), std::string{kVanillaStatusHex});
}

CYANE_TEST(status_reports_online_and_max_players) {
    cyane::game::ServerStatus status{"motd", 40};
    status.set_online(7);
    const std::string json = status.build_status_json();
    CYANE_CHECK(json.find(R"("players":{"max":40,"online":7})") != std::string::npos);
    CYANE_CHECK_EQ(status.online(), 7);
}

CYANE_TEST(status_keeps_key_order_expected_by_clients) {
    cyane::game::ServerStatus status{"m", 1};
    const std::string json = status.build_status_json();
    const auto description = json.find("\"description\"");
    const auto players = json.find("\"players\"");
    const auto version = json.find("\"version\"");
    CYANE_CHECK(description != std::string::npos);
    CYANE_CHECK(players != std::string::npos);
    CYANE_CHECK(version != std::string::npos);
    CYANE_CHECK(description < players);
    CYANE_CHECK(players < version);
}

CYANE_TEST(status_includes_favicon_only_when_set) {
    const cyane::game::ServerStatus plain{"m", 1};
    CYANE_CHECK(plain.build_status_json().find("favicon") == std::string::npos);

    const cyane::game::ServerStatus with_icon{"m", 1, "data:image/png;base64,AAAA"};
    const std::string json = with_icon.build_status_json();
    CYANE_CHECK(json.find(R"("favicon":"data:image/png;base64,AAAA")") != std::string::npos);
}

CYANE_TEST(status_escapes_motd_control_characters) {
    const cyane::game::ServerStatus status{std::string{"a\"b\nc"}, 1};
    const std::string json = status.build_status_json();
    CYANE_CHECK(json.find(R"("description":{"text":"a\"b\nc"})") != std::string::npos);
}
