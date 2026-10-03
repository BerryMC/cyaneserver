#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "cyane/core/config.hpp"
#include "cyane/game/server.hpp"
#include "cyane/proto/frame.hpp"
#include "cyane/proto/packet_ids.hpp"
#include "test_framework.hpp"
#include "test_net_client.hpp"

namespace {

// 起一个监听内核分配端口、单 I/O 线程的实例，析构时保证停机
class TestServer {
public:
    explicit TestServer(std::unique_ptr<cyane::Server> server) : server_{std::move(server)} {
        runner_ = std::jthread{[this] { server_->run(0); }};
        for (int attempt = 0; attempt < 400 && bound_port() == 0; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    }

    ~TestServer() {
        server_->request_stop();
        if (runner_.joinable()) {
            runner_.join();
        }
    }

    TestServer(const TestServer&) = delete;
    TestServer& operator=(const TestServer&) = delete;

    [[nodiscard]] std::uint16_t bound_port() const noexcept { return server_->network().bound_port(); }
    [[nodiscard]] bool ready() const noexcept { return bound_port() != 0; }
    [[nodiscard]] bool is_online_mode() const noexcept { return server_->config().online_mode; }

private:
    std::unique_ptr<cyane::Server> server_;
    std::jthread runner_;
};

[[nodiscard]] std::unique_ptr<TestServer> make_server(std::string_view extra = {}) {
    const std::string text = std::string{
                                 "[network]\nbind = \"127.0.0.1\"\nport = 0\nio_threads = 1\nmotd = "
                                 "\"IntegrationTest\"\n"} +
                             std::string{extra} +
                             "[server]\ntick_rate = 100\nmax_players = 12\nworker_threads = 1\n[log]\nfile = \"\"\n";
    auto config = cyane::Config::parse(text);
    if (!config) {
        return nullptr;
    }
    auto settings = cyane::ServerConfig::from(*config);
    if (!settings) {
        return nullptr;
    }
    auto server = cyane::Server::create(std::move(*settings));
    if (!server) {
        return nullptr;
    }
    return std::make_unique<TestServer>(std::move(*server));
}

[[nodiscard]] cyane::Bytes handshake_frame(std::uint16_t port, std::int32_t protocol, std::int32_t next_state) {
    cyane::ByteWriter handshake;
    handshake.varint(protocol);
    handshake.string("127.0.0.1");
    handshake.u16(port);
    handshake.varint(next_state);
    cyane::Bytes frame;
    cyane::proto::encode_frame(frame, cyane::proto::handshake_sb::kHandshake, handshake.data(), -1);
    return frame;
}

[[nodiscard]] cyane::Bytes frame_with(std::int32_t packet_id, const cyane::ByteWriter& fields) {
    cyane::Bytes frame;
    cyane::proto::encode_frame(frame, packet_id, fields.data(), -1);
    return frame;
}

}

CYANE_TEST(integration_handshake_status_ping_pong) {
    auto server = make_server("[network]\nonline_mode = false\n");
    CYANE_CHECK(server != nullptr);
    if (server == nullptr || !server->ready()) {
        return;
    }
    const std::uint16_t port = server->bound_port();

    auto client = cyane::test::TestClient::connect("127.0.0.1", port);
    CYANE_CHECK(client.has_value());
    if (!client) {
        return;
    }

    cyane::Bytes request = handshake_frame(port, cyane::proto::kProtocolVersion, 1);
    cyane::append(request, frame_with(cyane::proto::status_sb::kRequest, {}));
    CYANE_CHECK(client->send_bytes(cyane::ByteSpan{request}));

    auto response = client->receive_packet();
    CYANE_CHECK(response.has_value());
    if (!response) {
        return;
    }
    cyane::ByteReader reader{*response};
    CYANE_CHECK_EQ(reader.varint().value_or(-1), cyane::proto::status_cb::kResponse);
    const std::string json = reader.string().value_or("");
    CYANE_CHECK(json.find(R"("max":12)") != std::string::npos);
    CYANE_CHECK(json.find("IntegrationTest") != std::string::npos);
    CYANE_CHECK(json.find(R"("protocol":340)") != std::string::npos);
    CYANE_CHECK(json.find(R"("name":"1.12.2")") != std::string::npos);

    cyane::ByteWriter ping;
    ping.i64(0x1122334455667788LL);
    CYANE_CHECK(client->send_bytes(cyane::ByteSpan{frame_with(cyane::proto::status_sb::kPing, ping)}));

    auto pong = client->receive_packet();
    CYANE_CHECK(pong.has_value());
    if (!pong) {
        return;
    }
    cyane::ByteReader pong_reader{*pong};
    CYANE_CHECK_EQ(pong_reader.varint().value_or(-1), cyane::proto::status_cb::kPong);
    CYANE_CHECK_EQ(pong_reader.i64().value_or(0), 0x1122334455667788LL);
}

CYANE_TEST(integration_login_start_enters_world) {
    auto server = make_server("online_mode = false\n");
    CYANE_CHECK(server != nullptr);
    if (server == nullptr || !server->ready()) {
        return;
    }
    CYANE_CHECK(!server->is_online_mode());
    const std::uint16_t port = server->bound_port();

    auto client = cyane::test::TestClient::connect("127.0.0.1", port);
    CYANE_CHECK(client.has_value());
    if (!client) {
        return;
    }

    cyane::Bytes request = handshake_frame(port, cyane::proto::kProtocolVersion, 2);
    cyane::ByteWriter login;
    login.string("Notch");
    cyane::append(request, frame_with(cyane::proto::login_sb::kLoginStart, login));
    CYANE_CHECK(client->send_bytes(cyane::ByteSpan{request}));

    // 首包若是 SetCompression，则读取阈值并切到压缩帧
    auto response = client->receive_packet();
    if (!response) {
        return;
    }
    cyane::ByteReader reader{*response};
    const std::int32_t first_id = reader.varint().value_or(-1);
    if (first_id == cyane::proto::login_cb::kSetCompression) {
        (void)reader.varint();  // threshold
        response = client->receive_compressed_packet();
        if (!response) {
            return;
        }
        reader = cyane::ByteReader{*response};
    }
    CYANE_CHECK_EQ(reader.varint().value_or(-1), cyane::proto::login_cb::kSuccess);
    const std::string uuid = reader.string().value_or("");
    const std::string name = reader.string().value_or("");
    CYANE_CHECK_EQ(name, "Notch");
    CYANE_CHECK_EQ(uuid.size(), 36u);

    // 进入 play：应看到 JoinGame、ChunkData、PlayerPositionLook，且不断开。
    // 区块按 tick 限流（2/tick @10Hz ≈ 20/s）在传送包之后陆续到达
    bool saw_join_game = false;
    bool saw_chunk = false;
    bool saw_position_look = false;
    bool saw_disconnect = false;
    for (int i = 0; i < 2000 && (!saw_chunk || !saw_position_look); ++i) {
        auto packet = client->receive_compressed_packet();
        if (!packet) {
            break;
        }
        cyane::ByteReader pr{*packet};
        const std::int32_t id = pr.varint().value_or(-1);
        if (id == cyane::proto::play_cb::kJoinGame) {
            saw_join_game = true;
        } else if (id == cyane::proto::play_cb::kChunkData) {
            saw_chunk = true;
        } else if (id == cyane::proto::play_cb::kPlayerPositionLook) {
            saw_position_look = true;
        } else if (id == cyane::proto::play_cb::kDisconnect) {
            saw_disconnect = true;
            break;
        }
    }
    CYANE_CHECK(saw_join_game);
    CYANE_CHECK(saw_chunk);
    CYANE_CHECK(saw_position_look);
    CYANE_CHECK(!saw_disconnect);
}

CYANE_TEST(integration_old_protocol_gets_outdated_message) {
    auto server = make_server("[network]\nonline_mode = false\n");
    CYANE_CHECK(server != nullptr);
    if (server == nullptr || !server->ready()) {
        return;
    }
    const std::uint16_t port = server->bound_port();

    auto client = cyane::test::TestClient::connect("127.0.0.1", port);
    CYANE_CHECK(client.has_value());
    if (!client) {
        return;
    }

    cyane::Bytes request = handshake_frame(port, 47, 2);
    cyane::ByteWriter login;
    login.string("OldClient");
    cyane::append(request, frame_with(cyane::proto::login_sb::kLoginStart, login));
    CYANE_CHECK(client->send_bytes(cyane::ByteSpan{request}));

    auto response = client->receive_packet();
    CYANE_CHECK(response.has_value());
    if (!response) {
        return;
    }
    cyane::ByteReader reader{*response};
    CYANE_CHECK_EQ(reader.varint().value_or(-1), cyane::proto::login_cb::kDisconnect);
    CYANE_CHECK(reader.string().value_or("").find("Outdated client") != std::string::npos);
}

CYANE_TEST(integration_survives_fragmented_and_coalesced_frames) {
    auto server = make_server("[network]\nonline_mode = false\n");
    CYANE_CHECK(server != nullptr);
    if (server == nullptr || !server->ready()) {
        return;
    }
    const std::uint16_t port = server->bound_port();

    auto client = cyane::test::TestClient::connect("127.0.0.1", port);
    CYANE_CHECK(client.has_value());
    if (!client) {
        return;
    }

    cyane::Bytes request = handshake_frame(port, cyane::proto::kProtocolVersion, 1);
    cyane::append(request, frame_with(cyane::proto::status_sb::kRequest, {}));

    // 逐字节发送以制造半包
    for (const std::byte value : request) {
        const std::byte single[1]{value};
        CYANE_CHECK(client->send_bytes(cyane::ByteSpan{single}));
        std::this_thread::sleep_for(std::chrono::microseconds{200});
    }
    auto response = client->receive_packet();
    CYANE_CHECK(response.has_value());
    CYANE_CHECK(response.has_value() &&
                cyane::ByteReader{*response}.varint().value_or(-1) == cyane::proto::status_cb::kResponse);
}

CYANE_TEST(integration_rejects_absurd_frame_length) {
    auto server = make_server("[network]\nonline_mode = false\n");
    CYANE_CHECK(server != nullptr);
    if (server == nullptr || !server->ready()) {
        return;
    }
    const std::uint16_t port = server->bound_port();

    auto client = cyane::test::TestClient::connect("127.0.0.1", port);
    CYANE_CHECK(client.has_value());
    if (!client) {
        return;
    }

    cyane::Bytes bogus = handshake_frame(port, cyane::proto::kProtocolVersion, 1);
    cyane::append_varint(bogus, 0x7FFFFFFF);
    CYANE_CHECK(client->send_bytes(cyane::ByteSpan{bogus}));

    CYANE_CHECK(!client->receive_raw(1).has_value());
}

// 回归（R-022 期间实测）：空文本 + assumeCommand=true 的 TabComplete 曾让
// handle_tab_complete 的 substr(1) 抛 out_of_range 并 terminate 整个进程。
// 现在处理器有守卫 + handle_play 有异常兜底：连接最多被断，进程必须活着。
CYANE_TEST(integration_malformed_tab_complete_does_not_kill_server) {
    auto server = make_server("online_mode = false\n");
    CYANE_CHECK(server != nullptr);
    if (server == nullptr || !server->ready()) {
        return;
    }
    const std::uint16_t port = server->bound_port();

    auto client = cyane::test::TestClient::connect("127.0.0.1", port);
    CYANE_CHECK(client.has_value());
    if (!client) {
        return;
    }
    cyane::Bytes request = handshake_frame(port, cyane::proto::kProtocolVersion, 2);
    cyane::ByteWriter login;
    login.string("Fuzzer");
    cyane::append(request, frame_with(cyane::proto::login_sb::kLoginStart, login));
    CYANE_CHECK(client->send_bytes(cyane::ByteSpan{request}));

    // 读到 LoginSuccess（首包 SetCompression 未压缩，与既有登录测试一致）
    auto first = client->receive_packet();
    CYANE_CHECK(first.has_value());
    bool entered = false;
    if (first) {
        cyane::ByteReader pr{*first};
        if (pr.varint().value_or(-1) == cyane::proto::login_cb::kSetCompression) {
            auto second = client->receive_compressed_packet();
            if (second) {
                cyane::ByteReader sr{*second};
                entered = sr.varint().value_or(-1) == cyane::proto::login_cb::kSuccess;
            }
        }
    }
    CYANE_CHECK(entered);

    // 畸形 TabComplete：text="" + assumeCommand=true（曾触发 substr 越界）
    cyane::ByteWriter evil;
    evil.string("");      // 空文本
    evil.u8(1);           // assumeCommand=true
    CYANE_CHECK(client->send_bytes(cyane::ByteSpan{frame_with(cyane::proto::play_sb::kTabComplete, evil)}));
    // 混着再发一条合法的（防连接被断后仍"通过"）
    cyane::ByteWriter hello;
    hello.string("hi");
    hello.u8(0);
    CYANE_CHECK(client->send_bytes(cyane::ByteSpan{frame_with(cyane::proto::play_sb::kTabComplete, hello)}));

    // 服务端必须仍然活着：status ping 能正常应答
    auto prober = cyane::test::TestClient::connect("127.0.0.1", port);
    CYANE_CHECK(prober.has_value());
    if (!prober) {
        return;
    }
    cyane::Bytes status_request = handshake_frame(port, cyane::proto::kProtocolVersion, 1);
    cyane::append(status_request, frame_with(cyane::proto::status_sb::kRequest, {}));
    CYANE_CHECK(prober->send_bytes(cyane::ByteSpan{status_request}));
    auto response = prober->receive_packet();
    CYANE_CHECK(response.has_value());
    if (response) {
        cyane::ByteReader rr{*response};
        CYANE_CHECK_EQ(rr.varint().value_or(-1), cyane::proto::status_cb::kResponse);
    }
}
