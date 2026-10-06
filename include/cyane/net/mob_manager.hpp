#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "cyane/entity/player_entity.hpp"
#include "cyane/entity/player_manager.hpp"
#include "cyane/net/player_hub.hpp"
#include "cyane/world/mob_types.hpp"
#include "cyane/world/pathfinding.hpp"
#include "cyane/world/physics.hpp"
#include "cyane/world/world.hpp"

namespace cyane::net {

// 角度 float(度) → 1/256 圈的字节角（协议线格式）
[[nodiscard]] inline std::uint8_t angle_byte(float degrees) noexcept {
    return static_cast<std::uint8_t>(static_cast<int>(degrees * 256.0f / 360.0f) & 0xFF);
}

// AI 状态（被动：待机/漫游/受击逃窜；敌对：追击）
enum class MobAi : std::uint8_t {
    idle,
    wander,
    chase,
    retreat,
};

// 生物：状态 + 物理 + AI。物种属性按 type 从 world::mob_type() 取，不在此重复存储。
struct Mob {
    std::uint32_t entity_id{0};
    std::int32_t type{90};
    entity::Position pos{};
    float health{20.0f};
    // 物理：竖直速度 + 击退动量（每 tick 衰减）
    double velocity_y{0.0};
    double velocity_x{0.0};
    double velocity_z{0.0};
    bool on_ground{false};
    // AI
    MobAi ai{MobAi::idle};
    std::int32_t state_ticks{0};   // 当前状态剩余 tick
    std::int32_t scan_ticks{0};    // 目标扫描节流
    std::vector<world::Path::Node> path;  // PathNavigate：当前路径（空 = 无）
    std::size_t path_index{0};
    bool has_path{false};
    std::int32_t repath_ticks{0};  // PathfinderGoalMeleeAttack 的重寻路间隔（4 + rand(7)）
    double stuck_x{0.0};           // checkForStuck：100 tick 位移 <1.5 视为卡住清路径
    double stuck_z{0.0};
    std::int32_t stuck_ticks{0};
    std::int32_t unseen_ticks{0};  // EntityAITarget.unseenMemoryTicks：丢视线后目标记忆 60 tick
    std::uint32_t target_player{0};  // 敌对目标玩家实体 id（0 = 无）
    std::int32_t attack_cooldown{0};
    std::int32_t fuse_state{-1};   // 苦力怕 swell 状态（datawatcher 索引 12：-1 熄灭 / 1 引信中）
    std::int32_t fuse_ticks{0};    // 苦力怕引信计数（EntityCreeper.fuseTicks，0..30）
    bool bow_drawing{false};       // 骷髅举弓（SWINGING_ARMS，索引 12 Boolean）
    std::int32_t draw_ticks{0};    // 举弓时长（EntityAIAttackRangedBow：蓄力 20 tick 才放箭）
    std::int32_t see_ticks{0};     // 目标持续可见时长（seeTime ≥ 20 才停下走位）
    std::int8_t strafe_fwd{1};     // 走位前后轴（远处前进 / 贴脸后退）
    std::int8_t strafe_side{1};    // 走位左右轴
    std::int32_t strafe_ticks{0};  // 走位翻转计时（每 20 tick 各 0.3 概率翻转）
    // 环境音计时器（<=0 时播放并重置为 -talkInterval）
    std::int32_t living_sound_timer{0};
    // 死亡倒计时（-1 表示存活，否则剩余 tick，到 0 时真正删除）
    std::int32_t death_timer{-1};
    // 方块落差伤害：离开地面时记录起始 Y，落地时计算差值
    double fall_start_y{0.0};
    bool was_on_ground_last_tick{false};
    // 生物消失计时器（despawn）
    std::int32_t idle_ticks{0};
    // 无敌窗（hurtResistantTime=max 20，>10 时仅更高伤害可破防，只结算差值）
    std::int32_t hurt_resistant_ticks{0};
    float last_damage{0.0f};
    // 交互状态
    bool sheared{false};     // 羊：被剪毛后不再掉落羊毛
    bool in_love{false};     // 动物：爱心模式（繁殖冷却）
    std::int32_t love_timer{0};  // 爱心模式倒计时
};

// 苦力怕 swell 状态变化：Server 据此播引信音效并广播 metadata（-1 熄灭 / 1 引信中）
struct MobIgnition {
    std::uint32_t mob_id{0};
    double x{0.0};
    double y{0.0};
    double z{0.0};
    std::int32_t fuse_state{-1};
};

// 骷髅射出的箭：起点与目标（Server 负责生成投射物实体）
struct MobShot {
    std::uint32_t mob_id{0};
    std::uint32_t target_player{0};
    double x{0.0};
    double y{0.0};
    double z{0.0};
    double tx{0.0};
    double ty{0.0};
    double tz{0.0};
};

// 骷髅举弓/收弓：Server 据此广播 SWINGING_ARMS metadata（客户端据此播放蓄力动画）
struct MobDraw {
    std::uint32_t mob_id{0};
    double x{0.0};
    double y{0.0};
    double z{0.0};
    bool drawing{false};
};

struct MobExplosion {
    double x{0.0};
    double y{0.0};
    double z{0.0};
    float power{3.0f};  // vanilla 苦力怕 explosionRadius=3
};

// 持久化形状：类型 + 位置 + 朝向 + 血量（存档 Entities 列表）
struct MobState {
    std::uint8_t type{90};
    double x{0.0};
    double y{0.0};
    double z{0.0};
    float yaw{0.0f};
    float pitch{0.0f};
    float health{20.0f};
};

// 本 tick 的事件：由 Server 落地成广播/伤害/掉落（MobManager 不依赖连接与邮箱）
struct MobMove {
    std::uint32_t entity_id{0};
    double x{0.0};
    double y{0.0};
    double z{0.0};
    float yaw{0.0f};
};

struct MobAttack {
    std::uint32_t mob_id{0};
    std::uint32_t target_player{0};
    float damage{0.0f};
    double x{0.0};
    double z{0.0};
};

struct MobDeath {
    std::uint32_t mob_id{0};
    std::int32_t type{90};
    double x{0.0};
    double y{0.0};
    double z{0.0};
    bool despawn{false};  // true = 消失（despawn/死亡动画结束），只发 DestroyEntities
    int experience{0};    // 死亡时掉落的经验点数
};

struct MobSound {
    std::uint32_t entity_id{0};
    std::int32_t sound_id{0};
    double x{0.0};
    double y{0.0};
    double z{0.0};
};

struct MobTickResult {
    std::vector<MobMove> moved;
    std::vector<MobAttack> attacks;
    std::vector<MobDeath> deaths;
    std::vector<MobShot> shots;        // 骷髅射箭
    std::vector<MobExplosion> explosions;  // 苦力怕引爆（自爆即死亡）
    std::vector<MobIgnition> ignitions;    // 苦力怕点燃（引信开始）
    std::vector<MobDraw> draws;            // 骷髅举弓/收弓
    std::vector<MobSound> sounds;          // 环境音、伤害音等
};

// 受伤结果：是否命中、是否致死、剩余血量
struct MobHurt {
    bool found{false};
    bool died{false};
    float health{0.0f};
    double x{0.0};
    double y{0.0};
    double z{0.0};
};

// 生物登记表：Server::tick 驱动 AI 与物理（20Hz），连接线程读快照补发。
class MobManager {
public:
    // 在中心点附近随机生成 count 只被动生物，落在地表（world 提供 surface_y）
    void spawn_passive(std::size_t count, world::World& world, double center_x, double center_z);

    // 按类型在指定位置生成一只生物，返回实体 id（未知类型返回 0）
    std::uint32_t spawn(std::int32_t type, double x, double y, double z, float yaw);

    // 造成伤害（含击退方向）：死亡时从表里移除并回报坐标。
    // 击退强度：近战 0.4（EntityLiving.a(entity, 0.4F, ...)），爆炸传 impact。
    MobHurt damage(std::uint32_t id, float amount, double from_x, double from_z,
                   float knockback = 0.4f);

    // 推进 AI 与物理一个 tick；daytime = 世界是否白天（日光燃烧用）
    [[nodiscard]] MobTickResult tick(world::World& world,
                                     std::span<const PlayerSnapshot> players,
                                     bool daytime = false);

    [[nodiscard]] std::vector<Mob> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return mobs_;
    }

    [[nodiscard]] std::optional<Mob> by_id(std::uint32_t id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& m : mobs_) {
            if (m.entity_id == id) {
                return m;
            }
        }
        return std::nullopt;
    }

    // 从存档恢复生物（分配新实体 id，AI 重置为待机）
    void restore(std::span<const MobState> restored) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& s : restored) {
            const auto species = world::mob_type(s.type);
            if (!species) {
                continue;  // 不认识的物种不复活（下次保存时磁盘副本会被清掉）
            }
            Mob m;
            m.entity_id = entity::allocate_entity_id();
            m.type = s.type;
            m.pos.x = s.x;
            m.pos.y = s.y;
            m.pos.z = s.z;
            m.pos.yaw = s.yaw;
            m.pos.pitch = s.pitch;
            m.health = s.health > 0.0f ? s.health : species->health;
            m.state_ticks = 20;
            mobs_.push_back(std::move(m));
        }
    }

    // 全量快照（保存存档用）
    [[nodiscard]] std::vector<MobState> all_mobs() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<MobState> out;
        out.reserve(mobs_.size());
        for (const auto& m : mobs_) {
            out.push_back(MobState{static_cast<std::uint8_t>(m.type), m.pos.x, m.pos.y, m.pos.z,
                                   m.pos.yaw, m.pos.pitch, m.health});
        }
        return out;
    }

    // 移除生物（返回 true 表示存在且已移除）
    bool remove(std::uint32_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        return remove_locked(id);
    }

    // 交互（右键生物）：喂食繁殖 / 剪羊毛 / 挤奶桶。返回是否处理过。
    // 由 Connection::handle_play_use_entity(type=0) 调用。
    [[nodiscard]] bool interact(std::uint32_t id, std::int16_t held_item_id,
                                 std::int16_t held_item_damage);

    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return mobs_.size();
    }


private:
    [[nodiscard]] bool remove_locked(std::uint32_t id);

    mutable std::mutex mutex_;
    std::vector<Mob> mobs_;
};

}  // namespace cyane::net
