#pragma once

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <map>
#include <set>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "cyane/world/nbt.hpp"

namespace cyane::world::storage {

// 1.12.2 原版世界存档数据基类（WorldSavedData）
// 名字即 NBT 键（如 "map_0"），dirty 决定下次存档是否落盘。
class WorldSavedData {
public:
    virtual ~WorldSavedData() = default;

    explicit WorldSavedData(std::string name) : map_name_(std::move(name)) {}

    [[nodiscard]] const std::string& map_name() const noexcept { return map_name_; }

    virtual void read_from_nbt(const nbt::Value& nbt) = 0;
    [[nodiscard]] virtual nbt::Value write_to_nbt() const = 0;

    void mark_dirty() noexcept { dirty_ = true; }
    void set_dirty(bool dirty) noexcept { dirty_ = dirty; }
    [[nodiscard]] bool is_dirty() const noexcept { return dirty_; }

private:
    std::string map_name_;
    bool dirty_{false};
};

// 1.12.2 原版地图图标类型（MapDecoration.Type）
enum class MapDecorationType : std::uint8_t {
    Player = 0,
    Frame = 1,
    RedMarker = 2,
    BlueMarker = 3,
    RedX = 4,
    BlueX = 5,
    RedPointer = 6,
    WhitePointer = 7,
    WoodlandMansion = 8,
    OceanMonument = 9,
    DesertVillage = 10,
    PlainsVillage = 11,
    SavannaVillage = 12,
    SnowyVillage = 13,
    TaigaVillage = 14,
    JungleTemple = 15,
    SwampHut = 16,
};

[[nodiscard]] constexpr bool decoration_rendered_on_frame(MapDecorationType type) noexcept {
    return type == MapDecorationType::Frame || type == MapDecorationType::RedMarker ||
           type == MapDecorationType::BlueMarker || type == MapDecorationType::RedX ||
           type == MapDecorationType::BlueX;
}

// 1.12.2 原版地图装饰图标（MapDecoration）
struct MapDecoration {
    MapDecorationType type{MapDecorationType::Player};
    std::int8_t x{0};
    std::int8_t y{0};
    std::int8_t rotation{0};

    [[nodiscard]] std::uint8_t image() const noexcept { return static_cast<std::uint8_t>(type); }

    [[nodiscard]] bool operator==(const MapDecoration& other) const noexcept {
        return type == other.type && x == other.x && y == other.y && rotation == other.rotation;
    }
};

// 1.12.2 原版地图数据（MapData）
// colors 为 128×128 的平坦数组，每字节 (MapColor 索引 << 2) | brightness。
class MapData : public WorldSavedData {
public:
    static constexpr int kSize = 128;
    static constexpr int kArea = kSize * kSize;

    explicit MapData(std::string map_name) : WorldSavedData(std::move(map_name)) {
        colors_.assign(kArea, 0);
    }

    // vanilla calculateMapCenter：按 scale 对齐到 128*(1<<scale) 的中心
    constexpr void calculate_map_center(double x, double z, int map_scale) noexcept {
        const int span = kSize * (1 << map_scale);
        const int cell_x = static_cast<int>(std::floor((x + 64.0) / static_cast<double>(span)));
        const int cell_z = static_cast<int>(std::floor((z + 64.0) / static_cast<double>(span)));
        x_center_ = cell_x * span + span / 2 - 64;
        z_center_ = cell_z * span + span / 2 - 64;
    }

    [[nodiscard]] std::int32_t x_center() const noexcept { return x_center_; }
    [[nodiscard]] std::int32_t z_center() const noexcept { return z_center_; }
    [[nodiscard]] std::int32_t dimension() const noexcept { return dimension_; }
    void set_dimension(std::int32_t dimension) noexcept { dimension_ = dimension; }

    [[nodiscard]] std::uint8_t scale() const noexcept { return scale_; }
    void set_scale(std::uint8_t scale) noexcept { scale_ = static_cast<std::uint8_t>(std::min<std::uint8_t>(scale, 4)); }

    [[nodiscard]] bool tracking_position() const noexcept { return tracking_position_; }
    void set_tracking_position(bool value) noexcept { tracking_position_ = value; }

    [[nodiscard]] const std::vector<std::uint8_t>& colors() const noexcept { return colors_; }
    [[nodiscard]] std::uint8_t color_at(int x, int z) const noexcept {
        return colors_[static_cast<std::size_t>(z * kSize + x)];
    }
    void set_color_at(int x, int z, std::uint8_t value) noexcept {
        colors_[static_cast<std::size_t>(z * kSize + x)] = value;
    }

    // 装饰图标按玩家/结构名索引（vanilla mapDecorations 是 LinkedHashMap<String, MapDecoration>）
    [[nodiscard]] const std::map<std::string, MapDecoration>& decorations() const noexcept { return decorations_; }

    void add_decoration(const std::string& id, const MapDecoration& decoration) {
        const auto it = decorations_.find(id);
        if (it == decorations_.end() || !(it->second == decoration)) {
            decorations_[id] = decoration;
            mark_dirty();
        }
    }

    void remove_decoration(const std::string& id) { decorations_.erase(id); }

    void read_from_nbt(const nbt::Value& value) override {
        if (const auto v = field_int(value, "dimension")) dimension_ = static_cast<std::int32_t>(*v);
        if (const auto v = field_int(value, "xCenter")) x_center_ = static_cast<std::int32_t>(*v);
        if (const auto v = field_int(value, "zCenter")) z_center_ = static_cast<std::int32_t>(*v);
        if (const auto v = field_int(value, "scale")) {
            scale_ = static_cast<std::uint8_t>(std::clamp<std::int64_t>(*v, 0, 4));
        }
        if (const auto* flag = value.find("trackingPosition")) {
            tracking_position_ = flag->scalar().value_or(1) != 0;
        }
        if (const auto* colors = value.find("colors")) {
            if (const auto* bytes = colors->get_if<Bytes>(); bytes != nullptr) {
                colors_.assign(kArea, 0);
                const std::size_t count = std::min(bytes->size(), static_cast<std::size_t>(kArea));
                for (std::size_t i = 0; i < count; ++i) {
                    colors_[i] = static_cast<std::uint8_t>((*bytes)[i]);
                }
            }
        }
        if (const auto* decorations = value.find("Decorations")) {
            if (const auto* list = decorations->get_if<nbt::List>(); list != nullptr) {
                decorations_.clear();
                for (const auto& entry : *list) {
                    const auto id_it = entry.find("id");
                    const auto type_it = entry.find("type");
                    if (id_it == nullptr || type_it == nullptr) continue;
                    const auto id = id_it->text().value_or("");
                    MapDecoration decoration;
                    decoration.type = static_cast<MapDecorationType>(type_it->scalar().value_or(0));
                    if (const auto* v = entry.find("x"); v != nullptr) decoration.x = static_cast<std::int8_t>(v->scalar().value_or(0));
                    if (const auto* v = entry.find("y"); v != nullptr) decoration.y = static_cast<std::int8_t>(v->scalar().value_or(0));
                    if (const auto* v = entry.find("rot"); v != nullptr) decoration.rotation = static_cast<std::int8_t>(v->scalar().value_or(0));
                    decorations_[std::string{id}] = decoration;
                }
            }
        }
    }

    [[nodiscard]] nbt::Value write_to_nbt() const override {
        nbt::Compound root;
        root.emplace_back("dimension", nbt::make_i32(dimension_));
        root.emplace_back("xCenter", nbt::make_i32(x_center_));
        root.emplace_back("zCenter", nbt::make_i32(z_center_));
        root.emplace_back("scale", nbt::make_i8(static_cast<std::int8_t>(scale_)));
        root.emplace_back("trackingPosition", nbt::make_i8(tracking_position_ ? 1 : 0));
        Bytes colors;
        colors.reserve(colors_.size());
        for (const auto value : colors_) colors.push_back(static_cast<std::byte>(value));
        root.emplace_back("colors", nbt::make_byte_array(std::move(colors)));

        if (!decorations_.empty()) {
            nbt::List list;
            list.reserve(decorations_.size());
            for (const auto& [id, decoration] : decorations_) {
                nbt::Compound entry;
                entry.emplace_back("id", nbt::make_string(id));
                entry.emplace_back("type", nbt::make_i8(static_cast<std::int8_t>(decoration.type)));
                entry.emplace_back("x", nbt::make_i8(decoration.x));
                entry.emplace_back("y", nbt::make_i8(decoration.y));
                entry.emplace_back("rot", nbt::make_i8(decoration.rotation));
                list.push_back(nbt::make_compound(std::move(entry)));
            }
            root.emplace_back("Decorations", nbt::make_list(std::move(list)));
        }
        return nbt::make_compound(std::move(root));
    }

private:
    [[nodiscard]] static std::optional<std::int64_t> field_int(const nbt::Value& value, std::string_view key) {
        const auto* field = value.find(key);
        return field == nullptr ? std::nullopt : field->scalar();
    }

    std::int32_t x_center_{0};
    std::int32_t z_center_{0};
    std::int32_t dimension_{0};
    std::uint8_t scale_{0};
    bool tracking_position_{true};
    std::vector<std::uint8_t> colors_;
    std::map<std::string, MapDecoration> decorations_;
};

// 1.12.2 原版世界存档数据管理器（MapStorage）
// 每个维度一份：idCounts 记录地图号，map_<n> 为具体地图数据。
class MapStorage {
public:
    [[nodiscard]] MapData* get_or_create_map_data(std::string_view name) {
        const auto key = std::string{name};
        if (const auto it = maps_.find(key); it != maps_.end()) return it->second.get();
        auto data = std::make_unique<MapData>(key);
        auto* raw = data.get();
        maps_.emplace(key, std::move(data));
        dirty_maps_.insert(key);
        return raw;
    }

    // vanilla getUniqueDataId("map")：取 idCounts 当前值并自增
    [[nodiscard]] std::int32_t next_map_id() noexcept { return id_counts_++; }

    [[nodiscard]] MapData* get_map(std::int32_t id) { return get_or_create_map_data("map_" + std::to_string(id)); }

    [[nodiscard]] MapData* find_map_data(std::string_view name) noexcept {
        const auto it = maps_.find(std::string{name});
        return it == maps_.end() ? nullptr : it->second.get();
    }

    [[nodiscard]] std::size_t size() const noexcept { return maps_.size(); }

    // 需要落盘的数据名（vanilla MapStorage.saveAllData）
    [[nodiscard]] std::vector<std::string> pending_saves() const {
        std::vector<std::string> out;
        for (const auto& [name, data] : maps_) {
            if (data->is_dirty() || dirty_maps_.contains(name)) out.push_back(name);
        }
        return out;
    }

    void clear_dirty() {
        for (auto& [name, data] : maps_) data->set_dirty(false);
        dirty_maps_.clear();
    }

    // idCounts 序列化（vanilla idcounts.dat：{"map": <last>}）
    [[nodiscard]] nbt::Value write_id_counts() const {
        nbt::Compound root;
        root.emplace_back("map", nbt::make_i32(id_counts_));
        return nbt::make_compound(std::move(root));
    }

    void read_id_counts(const nbt::Value& value) {
        if (const auto* field = value.find("map"); field != nullptr) {
            id_counts_ = static_cast<std::int32_t>(field->scalar().value_or(0));
        }
    }

    // 载入一张已存在的 map_<n>（存档目录里读出的 NBT）
    MapData* load_map(std::string_view name, const nbt::Value& value) {
        auto* data = get_or_create_map_data(name);
        data->read_from_nbt(value);
        data->set_dirty(false);
        dirty_maps_.erase(std::string{name});
        return data;
    }

private:
    std::map<std::string, std::unique_ptr<MapData>> maps_;
    std::set<std::string> dirty_maps_;
    std::int32_t id_counts_{0};
};

}  // namespace cyane::world::storage
