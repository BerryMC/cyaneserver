#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

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

// AxisAlignedBB.calculateIntercept：线段与盒的六面拦截，取离起点最近者。
// 返回参数 t ∈ [0,1]（命中点 = from + t×(to−from)）；平行判据照抄
// Vec3d.intersectXPlane 的分量平方 < 1.0000000116860974e-7。
[[nodiscard]] inline std::optional<double> segment_aabb_intercept(const Aabb& box, double ax,
                                                                  double ay, double az, double bx,
                                                                  double by, double bz) {
    std::optional<double> best;
    const double dx = bx - ax;
    const double dy = by - ay;
    const double dz = bz - az;
    const auto plane_t = [](double num, double den) -> std::optional<double> {
        if (den * den < 1.0000000116860974e-7) {
            return std::nullopt;
        }
        const double t = num / den;
        if (t < 0.0 || t > 1.0) {
            return std::nullopt;
        }
        return t;
    };
    const auto in_y = [&](double t) {
        return ay + dy * t >= box.min_y && ay + dy * t <= box.max_y;
    };
    const auto in_z = [&](double t) {
        return az + dz * t >= box.min_z && az + dz * t <= box.max_z;
    };
    const auto in_x = [&](double t) {
        return ax + dx * t >= box.min_x && ax + dx * t <= box.max_x;
    };
    const auto consider = [&](std::optional<double> t, bool in_bounds) {
        if (t && in_bounds && (!best || *t < *best)) {
            best = t;
        }
    };
    if (auto t = plane_t(box.min_x - ax, dx)) {
        consider(t, in_y(*t) && in_z(*t));
    }
    if (auto t = plane_t(box.max_x - ax, dx)) {
        consider(t, in_y(*t) && in_z(*t));
    }
    if (auto t = plane_t(box.min_y - ay, dy)) {
        consider(t, in_x(*t) && in_z(*t));
    }
    if (auto t = plane_t(box.max_y - ay, dy)) {
        consider(t, in_x(*t) && in_z(*t));
    }
    if (auto t = plane_t(box.min_z - az, dz)) {
        consider(t, in_x(*t) && in_y(*t));
    }
    if (auto t = plane_t(box.max_z - az, dz)) {
        consider(t, in_x(*t) && in_y(*t));
    }
    return best;
}

// world.rayTraceBlocks（简化）：线段体素步进（Amanatides & Woo），
// 返回首个实心方块的进入参数 t ∈ [0,1]，起点已在实心方块内返回 0。
[[nodiscard]] inline std::optional<double> block_ray_hit(World& world, double ax, double ay,
                                                         double az, double bx, double by,
                                                         double bz) {
    const auto solid_at = [&](double px, double py, double pz) {
        return is_solid(world.block_at(static_cast<std::int32_t>(std::floor(px)),
                                       static_cast<std::int32_t>(std::floor(py)),
                                       static_cast<std::int32_t>(std::floor(pz))));
    };
    if (solid_at(ax, ay, az)) {
        return 0.0;
    }
    auto x = static_cast<std::int32_t>(std::floor(ax));
    auto y = static_cast<std::int32_t>(std::floor(ay));
    auto z = static_cast<std::int32_t>(std::floor(az));
    const double dx = bx - ax;
    const double dy = by - ay;
    const double dz = bz - az;
    const int step_x = dx > 0 ? 1 : (dx < 0 ? -1 : 0);
    const int step_y = dy > 0 ? 1 : (dy < 0 ? -1 : 0);
    const int step_z = dz > 0 ? 1 : (dz < 0 ? -1 : 0);
    const auto boundary = [](double pos, double dir, std::int32_t voxel) {
        // 到下一体素边界的距离 / 方向（dir=0 时给 inf）
        if (dir == 0.0) {
            return std::numeric_limits<double>::infinity();
        }
        const double bound = dir > 0 ? static_cast<double>(voxel + 1) : static_cast<double>(voxel);
        return (bound - pos) / dir;
    };
    double t_max_x = boundary(ax, dx, x);
    double t_max_y = boundary(ay, dy, y);
    double t_max_z = boundary(az, dz, z);
    const double t_dx = step_x != 0 ? std::abs(1.0 / dx) : std::numeric_limits<double>::infinity();
    const double t_dy = step_y != 0 ? std::abs(1.0 / dy) : std::numeric_limits<double>::infinity();
    const double t_dz = step_z != 0 ? std::abs(1.0 / dz) : std::numeric_limits<double>::infinity();
    double t = 0.0;
    while (t <= 1.0) {
        if (t_max_x <= t_max_y && t_max_x <= t_max_z) {
            x += step_x;
            t = t_max_x;
            t_max_x += t_dx;
        } else if (t_max_y <= t_max_z) {
            y += step_y;
            t = t_max_y;
            t_max_y += t_dy;
        } else {
            z += step_z;
            t = t_max_z;
            t_max_z += t_dz;
        }
        if (t > 1.0) {
            break;
        }
        if (solid_at(ax + dx * t, ay + dy * t, az + dz * t)) {
            return t;
        }
    }
    return std::nullopt;
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
