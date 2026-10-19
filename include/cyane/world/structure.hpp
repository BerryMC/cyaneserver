#pragma once

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cyane::world::structure {

// 1.12.2 原版结构三维包围盒（StructureBoundingBox 逐行转写）
struct StructureBoundingBox {
    int min_x{0};
    int min_y{0};
    int min_z{0};
    int max_x{0};
    int max_y{0};
    int max_z{0};

    constexpr StructureBoundingBox() noexcept = default;

    constexpr StructureBoundingBox(int x1, int y1, int z1, int x2, int y2, int z2) noexcept
        : min_x(x1), min_y(y1), min_z(z1), max_x(x2), max_y(y2), max_z(z2) {}

    [[nodiscard]] static constexpr StructureBoundingBox get_new_bounding_box() noexcept {
        return StructureBoundingBox(INT_MAX, INT_MAX, INT_MAX, INT_MIN, INT_MIN, INT_MIN);
    }

    [[nodiscard]] static constexpr StructureBoundingBox create_proper(int x1, int y1, int z1, int x2, int y2, int z2) noexcept {
        return StructureBoundingBox(
            std::min(x1, x2), std::min(y1, y2), std::min(z1, z2),
            std::max(x1, x2), std::max(y1, y2), std::max(z1, z2));
    }

    // 朝向投影变换
    [[nodiscard]] static constexpr StructureBoundingBox get_component_to_add_bounding_box(
        int sx, int sy, int sz, int x_min, int y_min, int z_min, int x_max, int y_max, int z_max, int facing) noexcept {
        switch (facing) {
            case 2: // NORTH
                return StructureBoundingBox(sx + x_min, sy + y_min, sz - z_max + 1 + z_min,
                                            sx + x_max - 1 + x_min, sy + y_max - 1 + y_min, sz + z_min);
            case 3: // SOUTH
                return StructureBoundingBox(sx + x_min, sy + y_min, sz + z_min,
                                            sx + x_max - 1 + x_min, sy + y_max - 1 + y_min, sz + z_max - 1 + z_min);
            case 4: // WEST
                return StructureBoundingBox(sx - z_max + 1 + z_min, sy + y_min, sz + x_min,
                                            sx + z_min, sy + y_max - 1 + y_min, sz + x_max - 1 + x_min);
            case 5: // EAST
                return StructureBoundingBox(sx + z_min, sy + y_min, sz + x_min,
                                            sx + z_max - 1 + z_min, sy + y_max - 1 + y_min, sz + x_max - 1 + x_min);
            default:
                return StructureBoundingBox(sx + x_min, sy + y_min, sz + z_min,
                                            sx + x_max - 1 + x_min, sy + y_max - 1 + y_min, sz + z_max - 1 + z_min);
        }
    }

    [[nodiscard]] constexpr bool intersects_with(const StructureBoundingBox& other) const noexcept {
        return max_x >= other.min_x && min_x <= other.max_x &&
               max_z >= other.min_z && min_z <= other.max_z &&
               max_y >= other.min_y && min_y <= other.max_y;
    }

    [[nodiscard]] constexpr bool intersects_with(int x1, int z1, int x2, int z2) const noexcept {
        return max_x >= x1 && min_x <= x2 && max_z >= z1 && min_z <= z2;
    }

    constexpr void expand_to(const StructureBoundingBox& other) noexcept {
        min_x = std::min(min_x, other.min_x);
        min_y = std::min(min_y, other.min_y);
        min_z = std::min(min_z, other.min_z);
        max_x = std::max(max_x, other.max_x);
        max_y = std::max(max_y, other.max_y);
        max_z = std::max(max_z, other.max_z);
    }

    constexpr void offset(int dx, int dy, int dz) noexcept {
        min_x += dx;
        min_y += dy;
        min_z += dz;
        max_x += dx;
        max_y += dy;
        max_z += dz;
    }

    [[nodiscard]] constexpr bool is_vec_inside(int x, int y, int z) const noexcept {
        return x >= min_x && x <= max_x && z >= min_z && z <= max_z && y >= min_y && y <= max_y;
    }

    [[nodiscard]] constexpr int get_x_size() const noexcept { return max_x - min_x + 1; }
    [[nodiscard]] constexpr int get_y_size() const noexcept { return max_y - min_y + 1; }
    [[nodiscard]] constexpr int get_z_size() const noexcept { return max_z - min_z + 1; }
};

// 1.12.2 结构组件名称注册与反序列化桥（MapGenStructureIO 逐行对齐）
class MapGenStructureIO {
public:
    static void register_structure(std::string_view name) {
        registered_structures_[name] = 1;
    }

    static void register_component(std::string_view name) {
        registered_components_[name] = 1;
    }

    [[nodiscard]] static bool is_registered_structure(std::string_view name) noexcept {
        return registered_structures_.contains(name);
    }

    [[nodiscard]] static bool is_registered_component(std::string_view name) noexcept {
        return registered_components_.contains(name);
    }

private:
    inline static std::unordered_map<std::string_view, int> registered_structures_ = {
        {"Mineshaft", 1},
        {"Village", 2},
        {"Fortress", 3},
        {"Stronghold", 4},
        {"Temple", 5},
        {"Monument", 6},
        {"EndCity", 7},
        {"Mansion", 8}
    };

    inline static std::unordered_map<std::string_view, int> registered_components_ = {
        {"SHStart", 1},
        {"ViStart", 2},
        {"MSStart", 3}
    };
};

}  // namespace cyane::world::structure
