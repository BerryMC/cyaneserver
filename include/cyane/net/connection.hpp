#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <utility>
#include <vector>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>

#include "cyane/core/bytes.hpp"
#include "cyane/core/time.hpp"
#include "cyane/core/uuid.hpp"
#include "cyane/crypto/cipher.hpp"
#include "cyane/crypto/rsa.hpp"
#include "cyane/net/reactor.hpp"
#include "cyane/net/session_service.hpp"
#include "cyane/net/socket.hpp"
#include "cyane/proto/frame.hpp"
#include "cyane/proto/packet_ids.hpp"
#include "cyane/proto/play_fields.hpp"
#include "cyane/entity/player_manager.hpp"
#include "cyane/item/crafting.hpp"
#include "cyane/item/player_inventory.hpp"
#include "cyane/net/container_store.hpp"
#include "cyane/net/crafting_table_store.hpp"
#include "cyane/net/furnace_store.hpp"
#include "cyane/net/mob_manager.hpp"
#include "cyane/net/item_drop.hpp"
#include "cyane/net/player_hub.hpp"
#include "cyane/world/world.hpp"
#include "cyane/game/player_data.hpp"

namespace cyane::game {
class OpManager;
}

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
    // 玩家断开时触发世界存档（仅写脏区块，Server 注入）
    std::function<void()> save_world;
    entity::PlayerManager* player_manager{nullptr};
    PlayerHub* hub{nullptr};
    ItemDropManager* item_drops{nullptr};
    MobManager* mobs{nullptr};
    ContainerStore* containers{nullptr};
    FurnaceStore* furnaces{nullptr};
    world::CraftingTableStore* crafting_tables{nullptr};
    game::OpManager* op_manager{nullptr};
    item::CraftingRegistry* crafting{nullptr};
    cyane::world::World* world{nullptr};
    const cyane::TickStats* tick_stats{nullptr};
    std::int32_t view_distance{10};
    std::int32_t max_players{20};
    std::uint8_t game_mode{proto::game_mode::kCreative};
    // 世界出生点（level.dat；缺失时为超平坦默认 (0,4,0)）
    std::int32_t spawn_x{0};
    std::int32_t spawn_y{4};
    std::int32_t spawn_z{0};
    // 玩家数据持久化存储（加载/保存玩家背包、位置、游戏模式）
    game::PlayerDataStore* player_data_store{nullptr};
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
    [[nodiscard]] bool handle_tab_complete(ByteSpan payload);
    [[nodiscard]] bool handle_play_digging(ByteSpan payload);
    [[nodiscard]] bool handle_play_block_place(ByteSpan payload);
    [[nodiscard]] bool handle_play_held_item(ByteSpan payload);
    [[nodiscard]] bool handle_play_creative_action(ByteSpan payload);
    [[nodiscard]] bool handle_play_click_window(ByteSpan payload);
    [[nodiscard]] bool handle_play_close_window(ByteSpan payload);
    [[nodiscard]] bool handle_play_client_command(ByteSpan payload);
    // UseItem (0x20)：对空中右键——手持食物时进食
    [[nodiscard]] bool handle_play_use_item(ByteSpan payload);
    // 0x1D 挥臂动画：限流后转发 Animation(0x06) 给视距内玩家
    [[nodiscard]] bool handle_play_animation(ByteSpan payload);
    // 打开箱子容器：下发 OpenWindow + 容器 WindowItems
    void open_chest(std::int64_t chest_key);
    // 小容器（发射器/投掷器/漏斗）：窗口布局同箱子式（容器格 + 27 主背包 + 9 热区）
    void open_small_container(std::int64_t key, ContainerStore::SmallKind kind);
    // 熔炉：打开窗口、点击处理、进度条同步
    void open_furnace(std::int64_t furnace_key);
    void apply_furnace_click(std::int16_t slot, std::uint8_t button, std::int32_t mode,
                             const item::ItemStack& clicked);
    void sync_furnace_progress();
    // 熔炉进度条（WindowProperty 0/1/2/3）：force=true 时无条件全发（开窗时）
    void send_furnace_progress(bool force);
    // 把某熔炉槽写回权威存储（即时持久化 + 触发反应式重估）并回发该槽
    void commit_furnace_slot(std::size_t fslot, const item::ItemStack& in_slot);
    // 熔炉槽经权威存储更新后，立即回发三槽与进度条
    void send_furnace_slots_now();
    void apply_click(std::int16_t slot, std::uint8_t button, std::int32_t mode);
    // 合成：读取合成格匹配配方并刷新结果槽（0）；take=true 时消耗一份材料
    void refresh_crafting_result();
    // 拿取合成结果：all=false 放到游标，all=true（shift）批量进背包
    void take_craft_result(bool all);
    void apply_chest_click(std::int16_t slot, std::uint8_t button, std::int32_t mode,
                           const item::ItemStack& clicked);
    // 工作台窗口（windowId=4，10 槽：0-8 格、9 结果）：打开/点击/结果计算
    void open_crafting_table(std::int64_t table_key);
    void apply_table_click(std::int16_t slot, std::uint8_t button, std::int32_t mode,
                           const item::ItemStack& clicked);
    [[nodiscard]] item::ItemStack compute_table_result() const;
    // 结果被取走时消耗一份材料（每非空格 -1 并写回存储）
    void consume_table_materials();
    // 容器方块被破坏时通知客户端关窗（游标剩余落地）
    void close_client_window(std::uint8_t window_id);
    // 游标校验失配时的全量重同步：重发当前开窗 WindowItems + 游标 SetSlot
    void resync_open_window();
    // 切换游戏模式（用于 /gamemode 命令），广播 UpdateGameMode 给所有玩家
    void set_game_mode(std::uint8_t mode);
    // 按模式回发 PlayerAbilities（创造/旁观允许飞行，其余默认）
    void send_abilities_for(std::uint8_t mode);
    // 远端玩家经 hub 投递要求本连接切换游戏模式：更新行为 + 回发 PlayerAbilities
    void apply_remote_gamemode(std::uint8_t mode);
    // 把 moving 尽量并入 [lo,hi] 槽区间（先叠已有同类，再填空槽），就地更新剩余
    [[nodiscard]] bool merge_into_range(item::ItemStack& moving, std::size_t lo, std::size_t hi);
    // 手持食物且未满血：消耗 1 个并回血（返回是否进食）
    [[nodiscard]] bool eat_held_food();
    // Named Sound Effect (0x49)：向 16 格内玩家（含自己）广播方块交互音效
    void send_block_sound(std::int32_t x, std::int32_t y, std::int32_t z,
                          std::uint16_t block_id, bool on);
    void kill_player();
    void respawn_player();
    // 掉落物：生成、给自己补发已有、拾取入包
    void spawn_dropped_item(const DroppedItem& drop);
    // 在世界生成一个掉落物（spawn + 本地补发 + 附近玩家广播）
    void drop_stack(double x, double y, double z, item::ItemStack stack, std::int32_t bx, std::int32_t bz);
    // 生成掉落物的两个包（SpawnObject + EntityMetadata）编码到 out_spawn/out_meta
    void encode_dropped_item(const DroppedItem& drop, ByteWriter& out_spawn, ByteWriter& out_meta) const;
    void send_existing_drops();
    void send_existing_mobs();
    // 玩家聊天命令处理
    bool handle_player_command(std::string_view text);
    // 发送聊天框反馈
    void send_chat_feedback(std::string_view message);
    void collect_items(std::uint64_t now_ms);
    // 把一个堆叠尽量塞进玩家背包（热区栏优先，再主背包），返回未放下的剩余
    [[nodiscard]] item::ItemStack give_item(item::ItemStack stack);
    void send_inventory();
    void send_slot(std::int8_t window_id, std::int16_t slot, const item::ItemStack& item);
    // 修改一个方块：写世界 + 向自己与附近玩家广播 BlockChange
    void set_block_and_broadcast(std::int32_t wx, std::int32_t wy, std::int32_t wz, std::uint16_t state);
    void send_spawn_player();
    // 按玩家所在区块与视距，把缺失区块排入待发队列、卸载出界区块
    void update_view(world::ChunkPos center);
    // 从待发表取 limit 个区块发出（tick 周期调用，限制每 tick 突发量）
    void send_pending_chunks(std::size_t limit);
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
    void finish_login(cyane::Uuid uuid);
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

    // 玩家数据持久化：登录时加载、断开时保存
    void load_player_data();
    void save_player_data();

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
    // 最近一次 tick 的时间戳，供非 tick 路径（如掉落物出生时刻）读取
    std::uint64_t now_ms_{0};

    // 玩家动作状态（潜行/疾跑），供后续移动广播与碰撞使用
    bool sneaking_{false};
    bool sprinting_{false};

    // 玩家背包（windowId=0，46 槽）与当前选中热区栏槽（0..8）
    item::PlayerInventory inventory_;
    std::uint8_t selected_slot_{0};
    // 窗口点击时鼠标游标上握着的物品堆叠
    item::ItemStack cursor_item_{};
    // 当前打开的箱子容器键（0 表示只开着自身背包）；窗口 id 固定用 1
    std::int64_t open_chest_key_{0};
    bool chest_open_{false};
    static constexpr std::uint8_t kChestWindowId = 1;
    // 熔炉窗口（右键熔炉方块打开）
    std::int64_t open_furnace_key_{0};
    bool furnace_open_{false};
    static constexpr std::uint8_t kFurnaceWindowId = 2;
    std::int32_t last_burn_left_{-1};
    std::int32_t last_cook_time_{-1};
    // 熔炉窗口打开期间已同步给客户端的三个熔炉槽（结果产出时补发 SetSlot）
    std::array<item::ItemStack, 3> last_furnace_slots_{};
    // 工作台窗口（右键工作台方块打开）
    std::int64_t open_table_key_{0};
    bool table_open_{false};
    static constexpr std::uint8_t kCraftingTableWindowId = 4;
    // 小容器窗口（发射器/投掷器/漏斗）：布局同箱子式，容器格数 9 或 5
    std::int64_t open_small_key_{0};
    bool small_open_{false};
    std::size_t small_slots_{0};
    static constexpr std::uint8_t kSmallWindowId = 5;
    // 已按下的按钮（block_key | 释放 tick）——tick 中到期回弹并广播
    std::vector<std::pair<std::int64_t, std::uint64_t>> pressed_buttons_;
    // 打开期间的 3x3 格与最近一次结果（点击就地改，同步写回 CraftingTableStore）
    std::array<item::ItemStack, world::CraftingTableStore::kGridCells> table_grid_{};
    item::ItemStack table_result_{};
    // 挥臂动画广播限流（vanilla 有 4 tick 冷却）
    std::uint64_t last_anim_broadcast_ms_{0};

    // 生命与死亡状态（伤害/重生）
    float health_{20.0f};
    bool dead_{false};

    // 已发送给客户端的区块集合，与玩家所在区块 + 视距一同维护
    std::unordered_set<std::int64_t> loaded_chunks_;
    // 区块限流：跨区块时入队，tick 每 tick 最多发 kChunkPerTick 个（约 20/s @10Hz sweep）
    std::deque<world::ChunkPos> pending_chunks_;
    std::unordered_set<std::int64_t> pending_chunk_keys_;
    static constexpr std::size_t kChunkPerTick = 2;
    world::ChunkPos last_center_{};
    bool has_center_{false};

    // 移动广播去重：位置/朝向与上次广播一致则跳过
    double last_bcast_x_{0.0};
    double last_bcast_y_{0.0};
    double last_bcast_z_{0.0};
    std::uint8_t last_bcast_yaw_{0};
    std::uint8_t last_bcast_pitch_{0};
    bool has_bcast_{false};

    // 多人广播：本连接在 hub 中的条目（含收件箱），进入 play 后有效
    std::shared_ptr<PlayerHub::Entry> hub_entry_;
    Uuid uuid_;
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
