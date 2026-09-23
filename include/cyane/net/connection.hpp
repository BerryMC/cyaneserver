#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>

#include "cyane/core/bytes.hpp"
#include "cyane/crypto/cipher.hpp"
#include "cyane/crypto/rsa.hpp"
#include "cyane/net/reactor.hpp"
#include "cyane/net/session_service.hpp"
#include "cyane/net/socket.hpp"
#include "cyane/proto/frame.hpp"
#include "cyane/proto/packet_ids.hpp"
#include "cyane/proto/play_fields.hpp"
#include "cyane/entity/player_manager.hpp"
#include "cyane/item/player_inventory.hpp"
#include "cyane/net/player_hub.hpp"
#include "cyane/world/world.hpp"

namespace cyane::net {

// 游戏层注入到网络层的能力，避免 net 反向依赖 game
class StatusProvider {
public:
    StatusProvider() = default;
    virtual ~StatusProvider() = default;
    StatusProvider(const StatusProvider&) = delete;
    StatusProvider& operator=(const StatusProvider&) = delete;

    [[nodiscard]] virtual std::string build_status_json() const = 0;
};

struct ConnectionContext {
    const StatusProvider* status{nullptr};
    const crypto::RsaKeyPair* keys{nullptr};
    const SessionService* sessions{nullptr};
    bool online_mode{true};
    std::int32_t compression_threshold{proto::kDefaultCompressionThreshold};
    std::string disconnect_message{"CyaneServer"};
    entity::PlayerManager* player_manager{nullptr};
    PlayerHub* hub{nullptr};
    cyane::world::World* world{nullptr};
    std::int32_t view_distance{10};
    std::int32_t max_players{20};
    std::uint8_t game_mode{proto::game_mode::kCreative};
};

class Connection final : public ReactorHandler {
public:
    Connection(Socket socket, std::string peer, Reactor& reactor, ConnectionContext context);
    ~Connection() override;

    void on_readable() override;
    void on_writable() override;
    void on_error() override;

    [[nodiscard]] bool alive() const noexcept { return alive_; }
    [[nodiscard]] int fd() const noexcept { return socket_.fd(); }
    [[nodiscard]] std::string_view peer() const noexcept { return peer_; }
    [[nodiscard]] proto::State state() const noexcept { return state_; }
    [[nodiscard]] std::string_view username() const noexcept { return username_; }

    void send_packet(std::int32_t packet_id, ByteSpan fields);
    void disconnect(std::string_view reason);

    // 由所属 reactor 线程周期调用（sweep 时）：驱动 KeepAlive 与超时检测
    void tick(std::uint64_t now_ms);

    static constexpr std::size_t kReadChunk = 16 * 1024;
    static constexpr std::size_t kMaxInboxBytes = static_cast<std::size_t>(proto::kMaxFrameBytes) + kReadChunk;
    static constexpr std::size_t kOutboxHighWater = 1U << 20;

private:
    void teardown() noexcept;
    void process_inbox();
    [[nodiscard]] bool handle_packet(std::int32_t packet_id, ByteSpan payload);
    [[nodiscard]] bool handle_handshake(ByteSpan payload);
    [[nodiscard]] bool handle_status(std::int32_t packet_id, ByteSpan payload);
    [[nodiscard]] bool handle_login(std::int32_t packet_id, ByteSpan payload);
    [[nodiscard]] bool handle_play(std::int32_t packet_id, ByteSpan payload);
    [[nodiscard]] bool handle_play_keepalive(ByteSpan payload);
    [[nodiscard]] bool handle_play_position(std::int32_t packet_id, ByteSpan payload);
    [[nodiscard]] bool handle_play_entity_action(ByteSpan payload);
    [[nodiscard]] bool handle_play_chat(ByteSpan payload);
    [[nodiscard]] bool handle_play_digging(ByteSpan payload);
    [[nodiscard]] bool handle_play_block_place(ByteSpan payload);
    [[nodiscard]] bool handle_play_held_item(ByteSpan payload);
    [[nodiscard]] bool handle_play_creative_action(ByteSpan payload);
    [[nodiscard]] bool handle_play_click_window(ByteSpan payload);
    void send_inventory();
    void send_slot(std::int8_t window_id, std::int16_t slot, const item::ItemStack& item);
    // 修改一个方块：写世界 + 向自己与附近玩家广播 BlockChange
    void set_block_and_broadcast(std::int32_t wx, std::int32_t wy, std::int32_t wz, std::uint16_t state);
    void send_spawn_player();
    // 按玩家所在区块与视距，加载缺失区块、卸载出界区块
    void update_view(world::ChunkPos center);
    void send_chunk(world::ChunkPos pos);
    void unload_chunk(world::ChunkPos pos);
    // 多人可见性：广播自己、补发他人、投递收件箱
    void broadcast_spawn();
    void spawn_existing_players();
    void broadcast_despawn();
    void drain_mailbox();
    [[nodiscard]] bool handle_login_start(ByteSpan payload);
    [[nodiscard]] bool handle_encryption_response(ByteSpan payload);
    void send_encryption_request();
    void finish_login(std::string uuid_with_dashes);
    void send_login_success(std::string uuid_with_dashes);
    void send_join_game();
    void send_world_state();
    void send_initial_teleport();
    void register_in_hub();
    void broadcast_movement(const entity::Position& pos);
    [[nodiscard]] entity::Position spawn_point() const noexcept;
    void enable_cipher(ByteSpan session_key);
    void flush_outbox();
    void set_writable(bool writable);

    Socket socket_;
    std::string peer_;
    Reactor* reactor_{nullptr};
    ConnectionContext context_;

    proto::State state_{proto::State::handshake};
    std::string username_;
    std::int32_t protocol_version_{0};
    std::int32_t compression_threshold_{-1};  // 当前生效阈值，-1 表示未启用压缩
    Bytes verify_token_;
    std::int32_t teleport_id_{0};
    std::uint32_t player_id_{0};

    // KeepAlive 保活：进入 play 后每 kKeepAliveIntervalMs 发一个带 id 的心跳，
    // 客户端须在 kKeepAliveTimeoutMs 内回同一 id，否则断开。
    std::int64_t last_keepalive_id_{0};
    std::uint64_t last_keepalive_sent_ms_{0};
    std::uint64_t last_keepalive_recv_ms_{0};
    bool awaiting_keepalive_{false};
    static constexpr std::uint64_t kKeepAliveIntervalMs = 10'000;
    static constexpr std::uint64_t kKeepAliveTimeoutMs = 30'000;

    // 玩家动作状态（潜行/疾跑），供后续移动广播与碰撞使用
    bool sneaking_{false};
    bool sprinting_{false};

    // 玩家背包（windowId=0，46 槽）与当前选中热区栏槽（0..8）
    item::PlayerInventory inventory_;
    std::uint8_t selected_slot_{0};

    // 已发送给客户端的区块集合，与玩家所在区块 + 视距一同维护
    std::unordered_set<std::int64_t> loaded_chunks_;
    world::ChunkPos last_center_{};
    bool has_center_{false};

    // 多人广播：本连接在 hub 中的条目（含收件箱），进入 play 后有效
    std::shared_ptr<PlayerHub::Entry> hub_entry_;
    std::array<std::uint8_t, 16> uuid_bytes_{};
    entity::Position player_pos_{};

    std::unique_ptr<crypto::StreamCipher> decrypt_cipher_;
    std::unique_ptr<crypto::StreamCipher> encrypt_cipher_;

    Bytes inbox_;
    std::size_t inbox_offset_{0};
    Bytes outbox_;
    std::size_t outbox_offset_{0};
    Bytes scratch_;

    bool want_write_{false};
    bool alive_{true};
    bool close_after_flush_{false};
};

}
