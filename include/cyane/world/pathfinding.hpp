#pragma once

// vanilla 1.12.2 寻路系统的 C++ 转写（Forge 反混淆源码，逐行对齐）：
//   pathfinding/{PathFinder, PathHeap, PathPoint, Path, PathNodeType,
//                WalkNodeProcessor, PathNavigate, PathNavigateGround}
//   entity/ai/RandomPositionGenerator
// 类结构同原版：NodeProcessor 持有 entitySize、openPoint 去重表（pointMap）与
// PathNodeType 缓存，PathFinder 做 A*（≤200 次扩展、pathOptions[32]）。getSafePoint 的
// 跳台阶递归、maxFallHeight=3 的落差下探、对角邻居门控全部照抄——"不会走下悬崖"由
// 原版节点判定自然保证，不含任何自创检查。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <optional>
#include <random>
#include <unordered_map>
#include <vector>

#include "cyane/world/blocks.hpp"
#include "cyane/world/physics.hpp"

namespace cyane::world {

namespace {
[[nodiscard]] inline double path_rand() {
    thread_local std::mt19937 engine{std::random_device{}()};
    return std::uniform_real_distribution<double>(0.0, 1.0)(engine);
}
}  // namespace

// ---- PathNodeType（枚举顺序 = ordinal 顺序，EnumSet 按此遍历，顺序有意义） ----
enum class PathNodeType : std::uint8_t {
    blocked,
    open,
    walkable,
    trapdoor,
    fence,
    lava,
    water,
    rail,
    danger_fire,
    damage_fire,
    danger_cactus,
    damage_cactus,
    danger_other,
    damage_other,
    door_open,
    door_wood_closed,
    door_iron_closed,
};

inline constexpr std::size_t kPathNodeTypeCount = 17;

// PathNodeType.getPriority()：负值 = 不可走，正值 = 代价惩罚
[[nodiscard]] constexpr float path_priority(PathNodeType type) noexcept {
    switch (type) {
        case PathNodeType::blocked:
        case PathNodeType::fence:
        case PathNodeType::lava:
        case PathNodeType::damage_cactus:
        case PathNodeType::damage_other:
        case PathNodeType::door_wood_closed:
        case PathNodeType::door_iron_closed:
            return -1.0f;
        case PathNodeType::water:
        case PathNodeType::danger_fire:
        case PathNodeType::danger_cactus:
        case PathNodeType::danger_other:
            return 8.0f;
        case PathNodeType::damage_fire:
            return 16.0f;
        default:
            return 0.0f;
    }
}

// Block.getBoundingBox().maxY 的近似：满方块 1.0、半砖按其 meta、无碰撞盒 0.0
[[nodiscard]] inline double block_top(std::uint16_t state) noexcept {
    if (!is_solid(state)) {
        return 0.0;
    }
    if (block_id(state) == 44) {  // 台阶方块：上半砖顶面 = 1.0
        return (state_meta(state) & 0x8) != 0 ? 1.0 : 0.5;
    }
    return 1.0;
}

// PathNavigate.canEntityStandOnPos：下方是满方块（半砖/楼梯/栅栏/玻璃不算）
[[nodiscard]] inline bool is_full_block(std::uint16_t state) noexcept {
    if (!is_solid(state)) {
        return false;
    }
    switch (block_id(state)) {
        case 20: case 44: case 95:                      // 玻璃 / 半砖 / 染色玻璃
        case 53: case 67: case 108: case 109: case 114:  // 楼梯
        case 128: case 134: case 135: case 136: case 156:
        case 163: case 164: case 180: case 182: case 203: case 204:
        case 85: case 113: case 139:                     // 栅栏 / 地狱砖栅栏 / 圆石墙
        case 188: case 189: case 190: case 191: case 192:
            return false;
        default:
            return true;
    }
}

[[nodiscard]] constexpr bool is_rail_block(std::uint16_t state) noexcept {
    const auto id = block_id(state);
    return id == 27 || id == 28 || id == 66 || id == 157;
}

// WalkNodeProcessor.getPathNodeTypeRaw：按方块状态分类（纯函数，供无缓存探测复用）
[[nodiscard]] inline PathNodeType raw_node_type_of(std::uint16_t state) noexcept {
    const auto id = block_id(state);
    const auto meta = state_meta(state);
    switch (id) {
        case 0:
            return PathNodeType::open;
        case 96: case 167: case 111:  // 活板门 / 睡莲
            return PathNodeType::trapdoor;
        case 51:
            return PathNodeType::damage_fire;
        case 81:
            return PathNodeType::damage_cactus;
        case 64: case 193: case 194: case 195: case 196: case 197:
            return (meta & 0x4) != 0 ? PathNodeType::door_open : PathNodeType::door_wood_closed;
        case 71:
            return (meta & 0x4) != 0 ? PathNodeType::door_open : PathNodeType::door_iron_closed;
        case 27: case 28: case 66: case 157:
            return PathNodeType::rail;
        case 8: case 9:
            return PathNodeType::water;
        case 10: case 11:
            return PathNodeType::lava;
        case 85: case 113: case 139:  // 栅栏 / 地狱砖栅栏 / 圆石墙
            return PathNodeType::fence;
        case 107:  // 栅栏门：开启可过
            return (meta & 0x4) != 0 ? PathNodeType::open : PathNodeType::fence;
        case 188: case 189: case 190: case 191: case 192:  // 木栅栏
            return PathNodeType::fence;
        default:
            return is_solid(state) ? PathNodeType::blocked : PathNodeType::open;
    }
}

// WalkNodeProcessor.getPathNodeType(IBlockAccess, x, y, z)：单格 + 支撑判定 + 邻接危险。
// EntityMoveHelper STRAFE 的迈步探查就是用它（无缓存；每生物每 tick 至多 1 次）。
[[nodiscard]] inline PathNodeType path_node_type_single(World& world, std::int32_t x,
                                                       std::int32_t y, std::int32_t z) {
    auto type = raw_node_type_of(world.block_at(x, y, z));
    if (type == PathNodeType::open && y >= 1) {
        const auto below = raw_node_type_of(world.block_at(x, y - 1, z));
        type = (below != PathNodeType::walkable && below != PathNodeType::open &&
                below != PathNodeType::water && below != PathNodeType::lava)
                   ? PathNodeType::walkable
                   : PathNodeType::open;
        if (below == PathNodeType::damage_fire) {
            type = PathNodeType::damage_fire;
        } else if (below == PathNodeType::damage_cactus) {
            type = PathNodeType::damage_cactus;
        } else if (below == PathNodeType::damage_other) {
            type = PathNodeType::damage_other;
        }
    }
    if (type == PathNodeType::walkable) {
        for (std::int32_t dx = -1; dx <= 1; ++dx) {
            for (std::int32_t dz = -1; dz <= 1; ++dz) {
                if (dx == 0 && dz == 0) {
                    continue;
                }
                const auto neighbor = raw_node_type_of(world.block_at(x + dx, y, z + dz));
                if (neighbor == PathNodeType::damage_cactus) {
                    type = PathNodeType::danger_cactus;
                } else if (neighbor == PathNodeType::damage_fire) {
                    type = PathNodeType::danger_fire;
                } else if (neighbor == PathNodeType::damage_other) {
                    type = PathNodeType::danger_other;
                }
            }
        }
    }
    return type;
}

// ---- PathPoint ----
struct PathPoint {
    std::int32_t x{0};
    std::int32_t y{0};
    std::int32_t z{0};
    std::int32_t heap_index{-1};  // vanilla PathPoint.index（PathHeap 内下标）
    float total_path_distance{0.0f};
    float distance_to_next{0.0f};
    float distance_to_target{0.0f};
    float distance_from_origin{0.0f};
    float cost{0.0f};
    float cost_malus{0.0f};
    PathNodeType type{PathNodeType::blocked};
    bool visited{false};
    PathPoint* previous{nullptr};

    [[nodiscard]] bool is_assigned() const noexcept { return heap_index >= 0; }
    [[nodiscard]] bool same_coords(const PathPoint& other) const noexcept {
        return x == other.x && y == other.y && z == other.z;
    }
    [[nodiscard]] float distance_manhattan(const PathPoint& to) const noexcept {
        return static_cast<float>(std::abs(to.x - x) + std::abs(to.y - y) + std::abs(to.z - z));
    }
    [[nodiscard]] float distance_to(const PathPoint& to) const noexcept {
        const auto dx = static_cast<float>(to.x - x);
        const auto dy = static_cast<float>(to.y - y);
        const auto dz = static_cast<float>(to.z - z);
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }
};

// ---- PathHeap：按 distanceToTarget 的二叉最小堆（sortBack/sortForward） ----
class PathHeap {
public:
    void clear() { points_.clear(); }

    PathPoint* add_point(PathPoint* point) {
        point->heap_index = static_cast<std::int32_t>(points_.size());
        points_.push_back(point);
        sort_back(static_cast<std::int32_t>(points_.size()) - 1);
        return point;
    }

    void change_distance(PathPoint* point, float distance) {
        const float previous = point->distance_to_target;
        point->distance_to_target = distance;
        if (distance < previous) {
            sort_back(point->heap_index);
        } else {
            sort_forward(point->heap_index);
        }
    }

    [[nodiscard]] PathPoint* dequeue() {
        PathPoint* top = points_.front();
        points_.front() = points_.back();
        points_.pop_back();
        if (!points_.empty()) {
            points_.front()->heap_index = 0;
            sort_forward(0);
        }
        top->heap_index = -1;
        return top;
    }

    [[nodiscard]] bool empty() const noexcept { return points_.empty(); }

private:
    void sort_back(std::int32_t index) {
        PathPoint* point = points_[static_cast<std::size_t>(index)];
        const float distance = point->distance_to_target;
        while (index > 0) {
            const std::int32_t parent = (index - 1) >> 1;
            PathPoint* above = points_[static_cast<std::size_t>(parent)];
            if (distance >= above->distance_to_target) {
                break;
            }
            points_[static_cast<std::size_t>(index)] = above;
            above->heap_index = index;
            index = parent;
        }
        points_[static_cast<std::size_t>(index)] = point;
        point->heap_index = index;
    }

    void sort_forward(std::int32_t index) {
        PathPoint* point = points_[static_cast<std::size_t>(index)];
        const float distance = point->distance_to_target;
        const auto count = static_cast<std::int32_t>(points_.size());
        while (true) {
            const std::int32_t left = 1 + (index << 1);
            const std::int32_t right = left + 1;
            if (left >= count) {
                break;
            }
            PathPoint* left_point = points_[static_cast<std::size_t>(left)];
            const float left_distance = left_point->distance_to_target;
            PathPoint* right_point = nullptr;
            float right_distance = std::numeric_limits<float>::infinity();
            if (right < count) {
                right_point = points_[static_cast<std::size_t>(right)];
                right_distance = right_point->distance_to_target;
            }
            if (left_distance < right_distance) {
                if (left_distance >= distance) {
                    break;
                }
                points_[static_cast<std::size_t>(index)] = left_point;
                left_point->heap_index = index;
                index = left;
            } else {
                if (right_distance >= distance) {
                    break;
                }
                points_[static_cast<std::size_t>(index)] = right_point;
                right_point->heap_index = index;
                index = right;
            }
        }
        points_[static_cast<std::size_t>(index)] = point;
        point->heap_index = index;
    }

    std::vector<PathPoint*> points_;
};

// ---- Path：节点坐标序列 + currentPathIndex ----
struct Path {
    struct Node {
        std::int32_t x{0};
        std::int32_t y{0};
        std::int32_t z{0};
    };
    struct Vec {
        double x{0.0};
        double y{0.0};
        double z{0.0};
    };

    std::vector<Node> points;
    std::size_t index{0};  // currentPathIndex

    [[nodiscard]] bool finished() const noexcept { return index >= points.size(); }
    [[nodiscard]] std::size_t length() const noexcept { return points.size(); }
    [[nodiscard]] const Node& from_index(std::size_t i) const noexcept { return points[i]; }
    [[nodiscard]] const Node& current_pos() const noexcept { return points[index]; }

    // Path.getVectorFromIndex：节点中心 + (int)(width+1)*0.5 偏移
    [[nodiscard]] Vec vector_from_index(float width, std::size_t i) const noexcept {
        const double offset =
            static_cast<double>(static_cast<std::int32_t>(width + 1.0f)) * 0.5;
        const auto& node = points[i];
        return Vec{static_cast<double>(node.x) + offset, static_cast<double>(node.y),
                   static_cast<double>(node.z) + offset};
    }
};

// ---- 实体占位尺寸（NodeProcessor.init） ----
struct MobShape {
    float width{0.6f};
    float height{1.8f};
    std::int32_t size_x{1};  // floor(width + 1)
    std::int32_t size_y{2};  // floor(height + 1)
    std::int32_t size_z{1};
    float step_height{0.6f};  // Entity.stepHeight 默认 0.6
};

[[nodiscard]] inline MobShape mob_shape(float width, float height) noexcept {
    const auto sx = static_cast<std::int32_t>(std::floor(width + 1.0f));
    return MobShape{width, height, sx, static_cast<std::int32_t>(std::floor(height + 1.0f)), sx,
                    0.6f};
}

// ---- WalkNodeProcessor / NodeProcessor ----
class WalkNodeProcessor {
public:
    // vanilla：节点判定读 IBlockAccess（PathFinder 收到的是 ChunkCache），只有两处
    // AABB 碰撞检查用 entity.world —— 这里对应 blocks_ 快照 + world_ 实体
    WalkNodeProcessor(World& world, const World::BlockCache& blocks, const MobShape& shape)
        : world_{world}, blocks_{blocks}, shape_{shape} {}

    // 跟随捷径的安全性检查按原版用 canOpenDoors=true/canEnterDoors=true 的节点判定
    void set_can_open_doors(bool value) noexcept { can_open_doors_ = value; }

    // WalkNodeProcessor.getStart
    [[nodiscard]] PathPoint* get_start(double pos_x, double pos_y, double pos_z, bool on_ground) {
        const auto bx = static_cast<std::int32_t>(std::floor(pos_x));
        const auto bz = static_cast<std::int32_t>(std::floor(pos_z));
        std::int32_t y = 0;
        if (on_ground) {
            y = static_cast<std::int32_t>(std::floor(pos_y + 0.5));
        } else {
            auto by = static_cast<std::int32_t>(std::floor(pos_y));
            while (by > 0 && !is_solid(blocks_.at(bx, by, bz))) {
                --by;
            }
            y = by + 1;
        }
        if (path_priority(node_type(bx, y, bz)) < 0.0f) {
            // 起点被挡（卡在栅栏里等）：试包围盒四角
            const double half = static_cast<double>(shape_.width) / 2.0;
            const auto x0 = static_cast<std::int32_t>(std::floor(pos_x - half));
            const auto x1 = static_cast<std::int32_t>(std::floor(pos_x + half));
            const auto z0 = static_cast<std::int32_t>(std::floor(pos_z - half));
            const auto z1 = static_cast<std::int32_t>(std::floor(pos_z + half));
            const std::int32_t corners[4][2] = {{x0, z0}, {x0, z1}, {x1, z0}, {x1, z1}};
            for (const auto& corner : corners) {
                if (path_priority(node_type(corner[0], y, corner[1])) >= 0.0f) {
                    return open_point(corner[0], y, corner[1]);
                }
            }
        }
        return open_point(bx, y, bz);
    }

    [[nodiscard]] PathPoint* get_path_point_to_coords(double x, double y, double z) {
        return open_point(static_cast<std::int32_t>(std::floor(x)),
                          static_cast<std::int32_t>(std::floor(y)),
                          static_cast<std::int32_t>(std::floor(z)));
    }

    // WalkNodeProcessor.findPathOptions
    void find_path_options(std::vector<PathPoint*>& options, PathPoint* current,
                           const PathPoint& target, float max_distance) {
        options.clear();
        std::int32_t jump = 0;
        if (path_priority(node_type(current->x, current->y + 1, current->z)) >= 0.0f) {
            jump = static_cast<std::int32_t>(std::floor(std::max(1.0f, shape_.step_height)));
        }
        const double fall_from = static_cast<double>(current->y) -
                                 (1.0 - block_top(blocks_.at(current->x, current->y - 1,
                                                             current->z)));
        // 邻居顺序同原版：SOUTH(z+1) / WEST(x-1) / EAST(x+1) / NORTH(z-1)
        PathPoint* south = get_safe_point(current->x, current->y, current->z + 1, jump, fall_from, 0, 1);
        PathPoint* west = get_safe_point(current->x - 1, current->y, current->z, jump, fall_from, -1, 0);
        PathPoint* east = get_safe_point(current->x + 1, current->y, current->z, jump, fall_from, 1, 0);
        PathPoint* north = get_safe_point(current->x, current->y, current->z - 1, jump, fall_from, 0, -1);

        const auto acceptable = [&](const PathPoint* point) {
            return point != nullptr && !point->visited && point->distance_to(target) < max_distance;
        };
        for (PathPoint* point : {south, west, east, north}) {
            if (acceptable(point)) {
                options.push_back(point);
            }
        }
        const bool north_open = north == nullptr || north->type == PathNodeType::open || north->cost_malus != 0.0f;
        const bool south_open = south == nullptr || south->type == PathNodeType::open || south->cost_malus != 0.0f;
        const bool east_open = east == nullptr || east->type == PathNodeType::open || east->cost_malus != 0.0f;
        const bool west_open = west == nullptr || west->type == PathNodeType::open || west->cost_malus != 0.0f;

        const auto diagonal = [&](std::int32_t dx, std::int32_t dz, std::int32_t face_z) {
            PathPoint* point = get_safe_point(current->x + dx, current->y, current->z + dz, jump,
                                              fall_from, 0, face_z);
            if (acceptable(point)) {
                options.push_back(point);
            }
        };
        if (north_open && west_open) {
            diagonal(-1, -1, -1);
        }
        if (north_open && east_open) {
            diagonal(1, -1, -1);
        }
        if (south_open && west_open) {
            diagonal(-1, 1, 1);
        }
        if (south_open && east_open) {
            diagonal(1, 1, 1);
        }
    }

    [[nodiscard]] const MobShape& shape() const noexcept { return shape_; }

    // PathNavigateGround.isDirectPathBetweenPoints：起点到目标点之间能否直线走过去
    // （pathFollow 的节点捷径就用它，避免抄近路跨过坑/水/岩浆/火）
    [[nodiscard]] bool is_direct_path_between_points(const Path::Vec& from, const Path::Vec& to) {
        set_can_open_doors(true);
        std::int32_t x = static_cast<std::int32_t>(std::floor(from.x));
        std::int32_t z = static_cast<std::int32_t>(std::floor(from.z));
        double dir_x = to.x - from.x;
        double dir_z = to.z - from.z;
        const double length_sq = dir_x * dir_x + dir_z * dir_z;
        if (length_sq < 1.0e-8) {
            return false;
        }
        const double scale = 1.0 / std::sqrt(length_sq);
        dir_x *= scale;
        dir_z *= scale;
        const auto size_y = static_cast<std::int32_t>(std::ceil(shape_.height));
        const auto base_size = static_cast<std::int32_t>(std::ceil(shape_.width));
        const auto y = static_cast<std::int32_t>(from.y);
        std::int32_t size_x = base_size + 2;
        std::int32_t size_z = base_size + 2;
        if (!is_safe_to_stand_at(x, y, z, size_x, size_y, size_z, from, dir_x, dir_z)) {
            return false;
        }
        size_x -= 2;
        size_z -= 2;
        const double step_x = 1.0 / std::abs(dir_x);
        const double step_z = 1.0 / std::abs(dir_z);
        double next_x = static_cast<double>(x) - from.x;
        double next_z = static_cast<double>(z) - from.z;
        if (dir_x >= 0.0) {
            next_x += 1.0;
        }
        if (dir_z >= 0.0) {
            next_z += 1.0;
        }
        next_x /= dir_x;
        next_z /= dir_z;
        const std::int32_t sign_x = dir_x < 0.0 ? -1 : 1;
        const std::int32_t sign_z = dir_z < 0.0 ? -1 : 1;
        const auto target_x = static_cast<std::int32_t>(std::floor(to.x));
        const auto target_z = static_cast<std::int32_t>(std::floor(to.z));
        std::int32_t remaining_x = target_x - x;
        std::int32_t remaining_z = target_z - z;
        while (remaining_x * sign_x > 0 || remaining_z * sign_z > 0) {
            if (next_x < next_z) {
                next_x += step_x;
                x += sign_x;
                remaining_x = target_x - x;
            } else {
                next_z += step_z;
                z += sign_z;
                remaining_z = target_z - z;
            }
            if (!is_safe_to_stand_at(x, y, z, size_x, size_y, size_z, from, dir_x, dir_z)) {
                return false;
            }
        }
        return true;
    }

    // 一次搜索的规模上界（A* 200 次扩展 + 8 邻域），避免哈希表中途 rehash
    void reserve(std::size_t points, std::size_t types) {
        point_map_.reserve(points);
        raw_cache_.reserve(types);
    }

private:
    // PathNavigateGround.isSafeToStandAt
    [[nodiscard]] bool is_safe_to_stand_at(std::int32_t x, std::int32_t y, std::int32_t z,
                                           std::int32_t size_x, std::int32_t size_y,
                                           std::int32_t size_z, const Path::Vec& from, double dir_x,
                                           double dir_z) {
        const std::int32_t min_x = x - size_x / 2;
        const std::int32_t min_z = z - size_z / 2;
        if (!is_position_clear(min_x, y, min_z, size_x, size_y, size_z, from, dir_x, dir_z)) {
            return false;
        }
        for (std::int32_t k = min_x; k < min_x + size_x; ++k) {
            for (std::int32_t l = min_z; l < min_z + size_z; ++l) {
                const double dx = static_cast<double>(k) + 0.5 - from.x;
                const double dz = static_cast<double>(l) + 0.5 - from.z;
                if (dx * dir_x + dz * dir_z < 0.0) {
                    continue;
                }
                const auto below = node_type(k, y - 1, l);
                if (below == PathNodeType::water || below == PathNodeType::lava ||
                    below == PathNodeType::open) {
                    return false;
                }
                const auto type = node_type(k, y, l);
                const float priority = path_priority(type);
                if (priority < 0.0f || priority >= 8.0f) {
                    return false;
                }
                if (type == PathNodeType::damage_fire || type == PathNodeType::danger_fire ||
                    type == PathNodeType::damage_other) {
                    return false;
                }
            }
        }
        return true;
    }

    // PathNavigateGround.isPositionClear：行进方向上的格子必须都是可穿过的
    [[nodiscard]] bool is_position_clear(std::int32_t x, std::int32_t y, std::int32_t z,
                                         std::int32_t size_x, std::int32_t size_y,
                                         std::int32_t size_z, const Path::Vec& from, double dir_x,
                                         double dir_z) {
        for (std::int32_t bx = x; bx < x + size_x; ++bx) {
            for (std::int32_t by = y; by < y + size_y; ++by) {
                for (std::int32_t bz = z; bz < z + size_z; ++bz) {
                    const double dx = static_cast<double>(bx) + 0.5 - from.x;
                    const double dz = static_cast<double>(bz) + 0.5 - from.z;
                    if (dx * dir_x + dz * dir_z < 0.0) {
                        continue;
                    }
                    if (is_solid(blocks_.at(bx, by, bz))) {
                        return false;
                    }
                }
            }
        }
        return true;
    }
    // NodeProcessor.openPoint：按坐标去重，A* 里同一格必须是同一个对象
    [[nodiscard]] PathPoint* open_point(std::int32_t x, std::int32_t y, std::int32_t z) {
        const auto key = pack_block_pos(x, y, z);
        if (const auto it = point_map_.find(key); it != point_map_.end()) {
            return it->second;
        }
        arena_.push_back(PathPoint{x, y, z});
        PathPoint* point = &arena_.back();
        point_map_.emplace(key, point);
        return point;
    }

    // WalkNodeProcessor.getPathNodeTypeRaw（含 PathNodeType 缓存：同一次搜索内同格只判一次）
    [[nodiscard]] PathNodeType raw_type(std::int32_t x, std::int32_t y, std::int32_t z) {
        const auto key = pack_block_pos(x, y, z);
        if (const auto it = raw_cache_.find(key); it != raw_cache_.end()) {
            return it->second;
        }
        const auto type = raw_node_type_of(blocks_.at(x, y, z));
        raw_cache_.emplace(key, type);
        return type;
    }

    // getPathNodeType(IBlockAccess, x, y, z)：单格 + 支撑判定 + 邻接危险
    [[nodiscard]] PathNodeType single_type(std::int32_t x, std::int32_t y, std::int32_t z) {
        auto type = raw_type(x, y, z);
        if (type == PathNodeType::open && y >= 1) {
            const auto below = raw_type(x, y - 1, z);
            type = (below != PathNodeType::walkable && below != PathNodeType::open &&
                    below != PathNodeType::water && below != PathNodeType::lava)
                       ? PathNodeType::walkable
                       : PathNodeType::open;
            if (below == PathNodeType::damage_fire) {
                type = PathNodeType::damage_fire;
            } else if (below == PathNodeType::damage_cactus) {
                type = PathNodeType::damage_cactus;
            } else if (below == PathNodeType::damage_other) {
                type = PathNodeType::damage_other;
            }
        }
        if (type == PathNodeType::walkable) {
            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                for (std::int32_t dz = -1; dz <= 1; ++dz) {
                    if (dx == 0 && dz == 0) {
                        continue;
                    }
                    const auto neighbor = raw_type(x + dx, y, z + dz);
                    if (neighbor == PathNodeType::damage_cactus) {
                        type = PathNodeType::danger_cactus;
                    } else if (neighbor == PathNodeType::damage_fire) {
                        type = PathNodeType::danger_fire;
                    } else if (neighbor == PathNodeType::damage_other) {
                        type = PathNodeType::danger_other;
                    }
                }
            }
        }
        return type;
    }

    // getPathNodeType(..., entitySizeX/Y/Z, canOpenDoors, canEnterDoors=true)
    [[nodiscard]] PathNodeType node_type(std::int32_t x, std::int32_t y, std::int32_t z) {
        const bool base_rail = is_rail_block(blocks_.at(x, y, z)) ||
                               is_rail_block(blocks_.at(x, y - 1, z));
        bool seen[kPathNodeTypeCount] = {};
        auto base = PathNodeType::blocked;
        for (std::int32_t i = 0; i < shape_.size_x; ++i) {
            for (std::int32_t j = 0; j < shape_.size_y; ++j) {
                for (std::int32_t k = 0; k < shape_.size_z; ++k) {
                    auto type = single_type(x + i, y + j, z + k);
                    if (type == PathNodeType::door_wood_closed && can_open_doors_) {
                        type = PathNodeType::walkable;
                    }
                    if (type == PathNodeType::door_open) {
                        continue;  // canEnterDoors = true
                    }
                    if (type == PathNodeType::rail && !base_rail) {
                        type = PathNodeType::fence;
                    }
                    if (i == 0 && j == 0 && k == 0) {
                        base = type;
                    }
                    seen[static_cast<std::size_t>(type)] = true;
                }
            }
        }
        if (seen[static_cast<std::size_t>(PathNodeType::fence)]) {
            return PathNodeType::fence;
        }
        auto best = PathNodeType::blocked;
        for (std::size_t ordinal = 0; ordinal < kPathNodeTypeCount; ++ordinal) {
            const auto type = static_cast<PathNodeType>(ordinal);
            if (!seen[ordinal]) {
                continue;
            }
            if (path_priority(type) < 0.0f) {
                return type;
            }
            if (path_priority(type) >= path_priority(best)) {
                best = type;
            }
        }
        if (base == PathNodeType::open && path_priority(best) == 0.0f) {
            return PathNodeType::open;
        }
        return best;
    }

    // WalkNodeProcessor.getSafePoint
    [[nodiscard]] PathPoint* get_safe_point(std::int32_t x, std::int32_t y, std::int32_t z,
                                            std::int32_t jump, double fall_from, std::int32_t face_x,
                                            std::int32_t face_z) {
        PathPoint* point = nullptr;
        const double ground = static_cast<double>(y) -
                              (1.0 - block_top(blocks_.at(x, y - 1, z)));
        if (ground - fall_from > 1.125) {
            return nullptr;
        }
        const auto type = node_type(x, y, z);
        const float priority = path_priority(type);
        const double half = static_cast<double>(shape_.width) / 2.0;

        if (priority >= 0.0f) {
            point = open_point(x, y, z);
            point->type = type;
            point->cost_malus = std::max(point->cost_malus, priority);
        }
        if (type == PathNodeType::walkable) {
            return point;
        }
        if (point == nullptr && jump > 0 && type != PathNodeType::fence &&
            type != PathNodeType::trapdoor) {
            point = get_safe_point(x, y + 1, z, jump - 1, fall_from, face_x, face_z);
            if (point != nullptr &&
                (point->type == PathNodeType::open || point->type == PathNodeType::walkable) &&
                shape_.width < 1.0f) {
                // 起跳位置（朝向来向的那一格）在抬升后的净空检查
                const double cx = static_cast<double>(x - face_x) + 0.5;
                const double cz = static_cast<double>(z - face_z) + 0.5;
                const double expand = block_top(blocks_.at(x, y, z)) - 0.002;
                const Aabb box{cx - half, static_cast<double>(y) + 0.001 - expand, cz - half,
                               cx + half, static_cast<double>(y) + static_cast<double>(shape_.height) + expand,
                               cz + half};
                if (box_hits_solid(world_, box)) {
                    point = nullptr;
                }
            }
        }
        if (type == PathNodeType::open) {
            const Aabb box{static_cast<double>(x) - half + 0.5, static_cast<double>(y) + 0.001,
                           static_cast<double>(z) - half + 0.5, static_cast<double>(x) + half + 0.5,
                           static_cast<double>(y) + static_cast<double>(shape_.height),
                           static_cast<double>(z) + half + 0.5};
            if (box_hits_solid(world_, box)) {
                return nullptr;
            }
            if (shape_.width >= 1.0f && node_type(x, y - 1, z) == PathNodeType::blocked) {
                point = open_point(x, y, z);
                point->type = PathNodeType::walkable;
                point->cost_malus = std::max(point->cost_malus, priority);
                return point;
            }
            std::int32_t steps = 0;
            auto current_y = y;
            auto current_type = type;
            while (current_y > 0 && current_type == PathNodeType::open) {
                --current_y;
                if (steps++ >= kMaxFallHeight) {
                    return nullptr;
                }
                current_type = node_type(x, current_y, z);
                const float current_priority = path_priority(current_type);
                if (current_type != PathNodeType::open && current_priority >= 0.0f) {
                    point = open_point(x, current_y, z);
                    point->type = current_type;
                    point->cost_malus = std::max(point->cost_malus, current_priority);
                    break;
                }
                if (current_priority < 0.0f) {
                    return nullptr;
                }
            }
        }
        return point;
    }

    static constexpr std::int32_t kMaxFallHeight = 3;  // EntityLiving.getMaxFallHeight

    World& world_;                    // 仅 AABB 碰撞检查（vanilla: entity.world）
    const World::BlockCache& blocks_;  // 节点判定（vanilla: PathFinder 的 ChunkCache）
    MobShape shape_;
    bool can_open_doors_{false};
    std::deque<PathPoint> arena_;
    std::unordered_map<std::int64_t, PathPoint*> point_map_;
    std::unordered_map<std::int64_t, PathNodeType> raw_cache_;
};

// ---- PathFinder（A*，逐行转写） ----
[[nodiscard]] inline std::optional<Path> find_path(World& world, double sx, double sy, double sz,
                                                   bool on_ground, float width, float height,
                                                   double tx, double ty, double tz,
                                                   float max_distance) {
    // PathNavigate.getPathToPos/getPathToEntityLiving：搜索框 = pathSearchRange + 8/16 格，
    // 据此给 PathFinder 传一份覆盖该范围的区块快照
    const auto radius = static_cast<std::int32_t>(max_distance + 16.0f) / 16 + 1;
    const auto cache = world.block_cache(ChunkPos::from_world(static_cast<std::int32_t>(std::floor(sx)),
                                                              static_cast<std::int32_t>(std::floor(sz)))
                                              ->x,
                                          ChunkPos::from_world(static_cast<std::int32_t>(std::floor(sx)),
                                                              static_cast<std::int32_t>(std::floor(sz)))
                                              ->z,
                                          radius);
    WalkNodeProcessor processor{world, cache, mob_shape(width, height)};
    processor.reserve(256, 1024);
    PathPoint* start = processor.get_start(sx, sy, sz, on_ground);
    PathPoint* target = processor.get_path_point_to_coords(tx, ty, tz);

    start->total_path_distance = 0.0f;
    start->distance_to_next = start->distance_manhattan(*target);
    start->distance_to_target = start->distance_to_next;
    PathHeap open;
    open.add_point(start);
    PathPoint* best = start;
    std::vector<PathPoint*> options;
    options.reserve(32);
    std::int32_t iterations = 0;
    while (!open.empty()) {
        if (++iterations >= 200) {
            break;
        }
        PathPoint* current = open.dequeue();
        if (current->same_coords(*target)) {
            best = current;
            break;
        }
        if (current->distance_manhattan(*target) < best->distance_manhattan(*target)) {
            best = current;
        }
        current->visited = true;
        processor.find_path_options(options, current, *target, max_distance);
        for (PathPoint* option : options) {
            const float step = current->distance_manhattan(*option);
            option->distance_from_origin = current->distance_from_origin + step;
            option->cost = step + option->cost_malus;
            const float candidate = current->total_path_distance + option->cost;
            if (option->distance_from_origin < max_distance &&
                (!option->is_assigned() || candidate < option->total_path_distance)) {
                option->previous = current;
                option->total_path_distance = candidate;
                option->distance_to_next =
                    option->distance_manhattan(*target) + option->cost_malus;
                if (option->is_assigned()) {
                    open.change_distance(option,
                                         option->total_path_distance + option->distance_to_next);
                } else {
                    option->distance_to_target =
                        option->total_path_distance + option->distance_to_next;
                    open.add_point(option);
                }
            }
        }
    }

    if (best == start) {
        return std::nullopt;
    }
    Path path;
    for (const PathPoint* node = best; node != nullptr; node = node->previous) {
        path.points.push_back(Path::Node{node->x, node->y, node->z});
    }
    std::reverse(path.points.begin(), path.points.end());
    if (path.points.size() <= 1) {
        return std::nullopt;
    }
    return path;
}

// ---- PathNavigateGround.isDirectPathBetweenPoints（pathFollow 的节点捷径） ----
[[nodiscard]] inline bool is_direct_path_between_points(World& world, const MobShape& shape,
                                                        const Path::Vec& from, const Path::Vec& to) {
    const auto start = ChunkPos::from_world(static_cast<std::int32_t>(std::floor(from.x)),
                                            static_cast<std::int32_t>(std::floor(from.z)));
    const auto cache = world.block_cache(start->x, start->z, 2);
    WalkNodeProcessor processor{world, cache, shape};
    processor.reserve(128, 512);
    return processor.is_direct_path_between_points(from, to);
}

// ---- PathNavigateGround.getPathToPos：随机落点先归一到能站的方块 ----
// 原版 RandomStroll/Panic 用 tryMoveToXYZ → getPathToPos：目标是空气就下探到地面再取其上，
// 目标是固体就上探到第一个非固体方块；这一步决定了"落到山体里"的候选点实际走到表面。
[[nodiscard]] inline Path::Node path_target_block(World& world, std::int32_t x, std::int32_t y,
                                                  std::int32_t z) {
    constexpr std::int32_t kHeight = 256;  // World.getHeight()
    if (block_id(world.block_at(x, y, z)) == 0) {
        auto below = y - 1;
        while (below > 0 && block_id(world.block_at(x, below, z)) == 0) {
            --below;
        }
        if (below > 0) {
            return Path::Node{x, below + 1, z};
        }
        while (below < kHeight && block_id(world.block_at(x, below, z)) == 0) {
            ++below;
        }
        y = below;
    }
    if (!is_solid(world.block_at(x, y, z))) {
        return Path::Node{x, y, z};
    }
    auto above = y + 1;
    while (above < kHeight && is_solid(world.block_at(x, above, z))) {
        ++above;
    }
    return Path::Node{x, above, z};
}

// ---- RandomPositionGenerator.generateRandomPos（无 home 分支；land=true 跳过水面调整） ----
[[nodiscard]] inline std::optional<Path::Node> random_position(World& world, double px, double py,
                                                               double pz, std::int32_t xz,
                                                               std::int32_t y_range, double dir_x,
                                                               double dir_z) {
    bool found = false;
    float best_weight = -99999.0f;
    std::int32_t best_x = 0;
    std::int32_t best_y = 0;
    std::int32_t best_z = 0;
    for (std::int32_t attempt = 0; attempt < 10; ++attempt) {
        const auto off_x = static_cast<std::int32_t>(path_rand() * (2.0 * xz + 1.0)) - xz;
        const auto off_y =
            static_cast<std::int32_t>(path_rand() * (2.0 * y_range + 1.0)) - y_range;
        const auto off_z = static_cast<std::int32_t>(path_rand() * (2.0 * xz + 1.0)) - xz;
        if ((dir_x != 0.0 || dir_z != 0.0) &&
            static_cast<double>(off_x) * dir_x + static_cast<double>(off_z) * dir_z < 0.0) {
            continue;
        }
        const auto bx = static_cast<std::int32_t>(std::floor(px)) + off_x;
        const auto by = static_cast<std::int32_t>(std::floor(py)) + off_y;
        const auto bz = static_cast<std::int32_t>(std::floor(pz)) + off_z;
        if (!is_full_block(world.block_at(bx, by - 1, bz))) {
            continue;
        }
        const float weight = 0.0f;  // Entity.getBlockPathWeight 默认 0（无生物群系偏好）
        if (weight > best_weight) {
            best_weight = weight;
            best_x = off_x;
            best_y = off_y;
            best_z = off_z;
            found = true;
        }
    }
    if (!found) {
        return std::nullopt;
    }
    return Path::Node{static_cast<std::int32_t>(std::floor(px)) + best_x,
                      static_cast<std::int32_t>(std::floor(py)) + best_y,
                      static_cast<std::int32_t>(std::floor(pz)) + best_z};
}

}  // namespace cyane::world
