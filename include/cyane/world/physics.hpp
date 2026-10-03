#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "cyane/world/blocks.hpp"
#include "cyane/world/world.hpp"

namespace cyane::world {

// 实体物理：轴对齐包围盒 + 逐轴推进的方块碰撞（含上台阶）。
// 简化取舍：不做 vanilla 的 voxel shape 扫掠，方块按整数格取碰撞盒；
// 上台阶直接抬 1 格（vanilla 怪物是起跳，视觉等价，先这样）。
inline constexpr double kEntityGravity = -0.08;   // 格/tick²
inline constexpr double kEntityTerminalY = -3.92; // 自由落体终端速度（vanilla 含阻力）
inline constexpr double kPhysicsEpsilon = 1e-7;

// 以 (x, y, z) 为中心/脚底建盒：vanilla 实体坐标是脚底中心
struct Aabb {
    double min_x{0.0};
    double min_y{0.0};
    double min_z{0.0};
    double max_x{0.0};
    double max_y{0.0};
    double max_z{0.0};
};

[[nodiscard]] inline Aabb entity_box(double x, double y, double z, float width,
                                     float height) noexcept {
    const double half = static_cast<double>(width) * 0.5;
    return Aabb{x - half, y, z - half, x + half, y + static_cast<double>(height), z + half};
}

// 盒子是否与任何固体方块相交（按整数格取，整数边界用 epsilon 排除）
[[nodiscard]] inline bool box_hits_solid(World& world, const Aabb& box) {
    const auto x0 = static_cast<std::int32_t>(std::floor(box.min_x));
    const auto x1 = static_cast<std::int32_t>(std::floor(box.max_x - kPhysicsEpsilon));
    const auto y0 = static_cast<std::int32_t>(std::floor(box.min_y));
    const auto y1 = static_cast<std::int32_t>(std::floor(box.max_y - kPhysicsEpsilon));
    const auto z0 = static_cast<std::int32_t>(std::floor(box.min_z));
    const auto z1 = static_cast<std::int32_t>(std::floor(box.max_z - kPhysicsEpsilon));
    for (std::int32_t y = y0; y <= y1; ++y) {
        for (std::int32_t z = z0; z <= z1; ++z) {
            for (std::int32_t x = x0; x <= x1; ++x) {
                if (is_solid(world.block_at(x, y, z))) {
                    return true;
                }
            }
        }
    }
    return false;
}

[[nodiscard]] inline bool is_on_ground(World& world, const Aabb& box) {
    Aabb below = box;
    below.min_y -= 2.0 * kPhysicsEpsilon;
    below.max_y = box.min_y;
    return box_hits_solid(world, below);
}

struct MoveOutcome {
    bool blocked_x{false};
    bool blocked_y{false};
    bool blocked_z{false};
    bool on_ground{false};
    bool stepped_up{false};
};

// 按位移方向逐轴推进；撞到方块就在该轴停下（dx/dy/dz 置 0）。
// 水平被挡且站在地面时尝试抬 step_height 再走（上台阶）。
[[nodiscard]] inline MoveOutcome move_with_collision(World& world, Aabb& box, double& dx, double& dy,
                                                     double& dz, double step_height = 1.0) {
    MoveOutcome out;
    constexpr double kMaxIncrement = 0.05;  // 单次推进上限，避免穿透薄墙

    const auto advance = [&world](Aabb& target, double delta, int axis) {
        const int steps =
            std::max(1, static_cast<int>(std::ceil(std::abs(delta) / kMaxIncrement)));
        const double increment = delta / steps;
        for (int i = 0; i < steps; ++i) {
            Aabb next = target;
            switch (axis) {
                case 0: next.min_x += increment; next.max_x += increment; break;
                case 1: next.min_y += increment; next.max_y += increment; break;
                default: next.min_z += increment; next.max_z += increment; break;
            }
            if (box_hits_solid(world, next)) {
                return false;
            }
            target = next;
        }
        return true;
    };

    if (dy != 0.0 && !advance(box, dy, 1)) {
        out.blocked_y = true;
        if (dy < 0.0) {
            // 落地贴合：逐级缩小步长补掉推进分步留下的悬空间隙（否则脚下差几毫米）
            for (double gap = 0.05; gap > 1e-5; gap *= 0.1) {
                while (true) {
                    Aabb next = box;
                    next.min_y -= gap;
                    next.max_y -= gap;
                    if (box_hits_solid(world, next)) {
                        break;
                    }
                    box = next;
                }
            }
            out.on_ground = true;
        }
        dy = 0.0;
    }
    for (int axis = 0; axis < 3; axis += 2) {  // X 然后 Z
        double& delta = axis == 0 ? dx : dz;
        if (delta == 0.0) {
            continue;
        }
        if (advance(box, delta, axis)) {
            continue;
        }
        // 上台阶：抬 step_height（其上净空）后重试；失败则回退并记为被挡
        const Aabb before = box;
        Aabb lifted = box;
        lifted.min_y += step_height;
        lifted.max_y += step_height;
        if (is_on_ground(world, box) && !box_hits_solid(world, lifted)) {
            Aabb raised = lifted;
            if (advance(raised, delta, axis)) {
                box = raised;
                out.stepped_up = true;
                continue;
            }
        }
        box = before;
        delta = 0.0;
        if (axis == 0) {
            out.blocked_x = true;
        } else {
            out.blocked_z = true;
        }
    }
    out.on_ground = out.on_ground || is_on_ground(world, box);
    return out;
}

}  // namespace cyane::world
