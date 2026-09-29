#include "cyane/game/player_data.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

#include "cyane/core/uuid.hpp"
#include "cyane/proto/frame.hpp"
#include "cyane/world/nbt.hpp"

namespace cyane::game {

namespace {

namespace nbt = cyane::world::nbt;
using nbt::Compound;
using nbt::List;
using nbt::Tag;
using nbt::Value;

// 1.12.2 DataVersion（与 Anvil 区块一致）
constexpr std::int32_t kDataVersion1343 = 1343;

// 原版 Inventory NBT 槽位：0-8 热区、9-35 主背包、100-103 护甲（100=脚…103=头）、40 副手。
// 我们的窗口布局：5-8 护甲（头/胸/腿/脚）、9-35 主背包、36-44 热区、45 副手。
// 窗口 0-4（合成结果与 2x2 格）原版不持久化，跳过。
[[nodiscard]] std::optional<std::uint8_t> window_slot_to_nbt(std::uint8_t window) noexcept {
    if (window >= 36 && window <= 44) {
        return static_cast<std::uint8_t>(window - 36);
    }
    if (window >= 9 && window <= 35) {
        return window;
    }
    if (window >= 5 && window <= 8) {
        return static_cast<std::uint8_t>(108 - window);
    }
    if (window == 45) {
        return static_cast<std::uint8_t>(40);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::uint8_t> nbt_slot_to_window(std::int32_t nbt_slot) noexcept {
    if (nbt_slot >= 0 && nbt_slot <= 8) {
        return static_cast<std::uint8_t>(nbt_slot + 36);
    }
    if (nbt_slot >= 9 && nbt_slot <= 35) {
        return static_cast<std::uint8_t>(nbt_slot);
    }
    if (nbt_slot >= 100 && nbt_slot <= 103) {
        return static_cast<std::uint8_t>(108 - nbt_slot);
    }
    if (nbt_slot == 40) {
        return static_cast<std::uint8_t>(45);
    }
    return std::nullopt;
}

[[nodiscard]] Value make_double_list(const std::array<double, 3>& values) {
    List items;
    for (const double v : values) {
        items.push_back(Value{Tag::f64, v});
    }
    return nbt::make_list(std::move(items));
}

[[nodiscard]] Value make_float_list(const std::array<float, 2>& values) {
    List items;
    for (const float v : values) {
        items.push_back(Value{Tag::f32, v});
    }
    return nbt::make_list(std::move(items));
}

[[nodiscard]] double list_double_get(const Value& list, std::size_t index, double fallback) noexcept {
    const auto* items = list.get_if<List>();
    if (items == nullptr || index >= items->size()) {
        return fallback;
    }
    const auto* v = (*items)[index].get_if<double>();
    return v != nullptr ? *v : fallback;
}

[[nodiscard]] float list_float_get(const Value& list, std::size_t index, float fallback) noexcept {
    const auto* items = list.get_if<List>();
    if (items == nullptr || index >= items->size()) {
        return fallback;
    }
    const auto* v = (*items)[index].get_if<float>();
    return v != nullptr ? *v : fallback;
}

// abilities 按 game_mode 派生（原版语义：创造=instabuild+mayfly+invulnerable，旁观=flying+mayfly）
[[nodiscard]] Value make_abilities(std::uint8_t game_mode) {
    const bool creative = game_mode == proto::game_mode::kCreative;
    const bool spectator = game_mode == proto::game_mode::kSpectator;
    Compound fields;
    fields.emplace_back("invulnerable", Value{Tag::i8, static_cast<std::int8_t>(creative || spectator)});
    fields.emplace_back("flying", Value{Tag::i8, static_cast<std::int8_t>(spectator)});
    fields.emplace_back("mayfly", Value{Tag::i8, static_cast<std::int8_t>(creative || spectator)});
    fields.emplace_back("instabuild", Value{Tag::i8, static_cast<std::int8_t>(creative)});
    fields.emplace_back("mayBuild", Value{Tag::i8, static_cast<std::int8_t>(game_mode != proto::game_mode::kSpectator)});
    fields.emplace_back("flySpeed", Value{Tag::f32, 0.05f});
    fields.emplace_back("walkSpeed", Value{Tag::f32, 0.1f});
    return nbt::make_compound(std::move(fields));
}

// 原版 playerdata 字段集（依据 oracle 实测：根 compound 直接包含字段，无 "Data" 包装）
[[nodiscard]] Value build_vanilla_nbt(const PlayerData& data) {
    Compound root;
    root.emplace_back("DataVersion", Value{Tag::i32, kDataVersion1343});
    root.emplace_back("Pos", make_double_list({data.x, data.y, data.z}));
    root.emplace_back("Motion", make_double_list({0.0, 0.0, 0.0}));
    root.emplace_back("Rotation", make_float_list({data.yaw, data.pitch}));
    root.emplace_back("OnGround", Value{Tag::i8, static_cast<std::int8_t>(1)});
    root.emplace_back("Air", Value{Tag::i16, static_cast<std::int16_t>(300)});
    root.emplace_back("Fire", Value{Tag::i16, static_cast<std::int16_t>(-20)});
    root.emplace_back("FallDistance", Value{Tag::f32, 0.0f});
    root.emplace_back("Invulnerable", Value{Tag::i8, static_cast<std::int8_t>(0)});
    root.emplace_back("PortalCooldown", Value{Tag::i32, 0});
    root.emplace_back("Dimension", Value{Tag::i32, 0});
    root.emplace_back("Health", Value{Tag::f32, data.health});
    root.emplace_back("foodLevel", Value{Tag::i32, 20});
    root.emplace_back("foodTickTimer", Value{Tag::i32, 0});
    root.emplace_back("foodSaturationLevel", Value{Tag::f32, 5.0f});
    root.emplace_back("foodExhaustionLevel", Value{Tag::f32, 0.0f});
    root.emplace_back("XpP", Value{Tag::f32, 0.0f});
    root.emplace_back("XpLevel", Value{Tag::i32, 0});
    root.emplace_back("XpTotal", Value{Tag::i32, 0});
    root.emplace_back("Score", Value{Tag::i32, 0});
    root.emplace_back("playerGameType", Value{Tag::i32, static_cast<std::int32_t>(data.game_mode)});
    root.emplace_back("SelectedItemSlot", Value{Tag::i8, static_cast<std::int8_t>(data.selected_slot)});
    root.emplace_back("abilities", make_abilities(data.game_mode));

    List inventory;
    for (std::uint8_t window = 0; window < data.inventory.size(); ++window) {
        const auto& stack = data.inventory[window];
        if (stack.empty()) {
            continue;
        }
        const auto nbt_slot = window_slot_to_nbt(window);
        if (!nbt_slot) {
            continue;
        }
        Compound entry;
        entry.emplace_back("Slot", Value{Tag::i8, static_cast<std::int8_t>(*nbt_slot)});
        entry.emplace_back("id", Value{Tag::i16, stack.id});
        entry.emplace_back("Damage", Value{Tag::i16, stack.damage});
        entry.emplace_back("Count", Value{Tag::i8, static_cast<std::int8_t>(stack.count)});
        inventory.push_back(nbt::make_compound(std::move(entry)));
    }
    root.emplace_back("Inventory", nbt::make_list(std::move(inventory)));
    root.emplace_back("EnderItems", nbt::make_list(List{}));

    // UUIDLeast/UUIDMost（1.12.2 也写；读方以文件名 uuid 为准）
    const auto uuid = Uuid::parse(data.uuid_with_dashes);
    const auto& bytes = uuid.bytes();
    std::uint64_t most = 0, least = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        most = (most << 8) | bytes[i];
        least = (least << 8) | bytes[8 + i];
    }
    root.emplace_back("UUIDMost", Value{Tag::i64, static_cast<std::int64_t>(most)});
    root.emplace_back("UUIDLeast", Value{Tag::i64, static_cast<std::int64_t>(least)});

    return nbt::make_compound(std::move(root));
}

// ---- 遗留 JSON 迁移（0.1.x 早期格式，见 git 历史）----

[[nodiscard]] std::string legacy_trim(const std::string& s) {
    std::size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) {
        ++a;
    }
    std::string out = s.substr(a);
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t' || out.back() == ',')) {
        out.pop_back();
    }
    return out;
}

[[nodiscard]] std::optional<PlayerData> parse_legacy_json(std::string_view text) {
    PlayerData data;
    bool in_inventory = false;
    std::size_t cursor = 0;
    auto line_of = [&](std::size_t pos) {
        const auto nl = text.find('\n', pos);
        return text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
    };
    while (cursor <= text.size()) {
        const auto raw = line_of(cursor);
        const auto newline = text.find('\n', cursor);
        cursor = newline == std::string_view::npos ? text.size() + 1 : newline + 1;
        const auto line = legacy_trim(std::string{raw});
        if (line.empty() || line == "{" || line == "}") {
            continue;
        }
        if (line == "\"inventory\": [") {
            in_inventory = true;
            continue;
        }
        if (in_inventory && line == "]") {
            in_inventory = false;
            continue;
        }
        if (in_inventory) {
            auto read_int = [&](std::string_view key) -> std::optional<long> {
                const auto key_pos = line.find(std::string{"\""} + std::string{key} + "\"");
                if (key_pos == std::string::npos) {
                    return std::nullopt;
                }
                const auto colon = line.find(':', key_pos);
                if (colon == std::string::npos) {
                    return std::nullopt;
                }
                const auto end = line.find_first_of(",}]", colon);
                const auto value = legacy_trim(line.substr(colon + 1, end - colon - 1));
                try {
                    return std::stol(value);
                } catch (...) {
                    return std::nullopt;
                }
            };
            const auto slot = read_int("slot");
            const auto id = read_int("id");
            if (slot && id && *slot >= 0 &&
                static_cast<std::size_t>(*slot) < data.inventory.size()) {
                item::ItemStack stack{static_cast<std::int16_t>(*id),
                                       static_cast<std::uint8_t>(read_int("count").value_or(0)),
                                       static_cast<std::int16_t>(read_int("damage").value_or(0))};
                if (!stack.empty()) {
                    data.inventory[static_cast<std::size_t>(*slot)] = stack;
                }
            }
            continue;
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        std::string key = legacy_trim(line.substr(0, colon));
        if (key.size() >= 2 && key.front() == '"' && key.back() == '"') {
            key = key.substr(1, key.size() - 2);
        }
        const auto value = legacy_trim(line.substr(colon + 1));
        try {
            if (key == "x") {
                data.x = std::stod(value);
            } else if (key == "y") {
                data.y = std::stod(value);
            } else if (key == "z") {
                data.z = std::stod(value);
            } else if (key == "yaw") {
                data.yaw = std::stof(value);
            } else if (key == "pitch") {
                data.pitch = std::stof(value);
            } else if (key == "game_mode") {
                data.game_mode = static_cast<std::uint8_t>(std::stoi(value));
            } else if (key == "health") {
                data.health = std::stof(value);
            }
        } catch (...) {
        }
    }
    return data;
}

} // namespace

PlayerData PlayerDataStore::load_or_default(std::string_view uuid_with_dashes,
                                            std::string_view username,
                                            std::uint8_t default_game_mode) const {
    PlayerData data;
    data.uuid_with_dashes = std::string{uuid_with_dashes};
    data.username = std::string{username};
    data.game_mode = default_game_mode;  // 新玩家用服务器配置，存档存在时其值会覆盖

    std::lock_guard<std::mutex> lock{mutex_};
    const auto dat_path =
        std::filesystem::path{dir_} / (std::string{uuid_with_dashes} + ".dat");
    const auto json_path =
        std::filesystem::path{dir_} / (std::string{uuid_with_dashes} + ".json");

    if (std::ifstream in{dat_path, std::ios::binary}; in) {
        std::ostringstream buffer;
        buffer << in.rdbuf();
        const std::string raw = buffer.str();
        auto root = nbt::parse_compressed(ByteSpan{reinterpret_cast<const std::byte*>(raw.data()), raw.size()});
        if (!root) {
            return data;  // 损坏档：按新玩家处理
        }
        if (const auto* pos = root->find("Pos"); pos != nullptr) {
            data.x = list_double_get(*pos, 0, data.x);
            data.y = list_double_get(*pos, 1, data.y);
            data.z = list_double_get(*pos, 2, data.z);
        }
        if (const auto* rot = root->find("Rotation"); rot != nullptr) {
            data.yaw = list_float_get(*rot, 0, data.yaw);
            data.pitch = list_float_get(*rot, 1, data.pitch);
        }
        if (const auto* v = root->find("Health"); v != nullptr) {
            if (const auto* h = v->get_if<float>(); h != nullptr) {
                data.health = *h;
            }
        }
        if (const auto* v = root->find("playerGameType"); v != nullptr) {
            if (const auto g = v->scalar(); g && *g >= 0 && *g <= 3) {
                data.game_mode = static_cast<std::uint8_t>(*g);
            }
        }
        if (const auto* v = root->find("SelectedItemSlot"); v != nullptr) {
            if (const auto s = v->scalar(); s && *s >= 0 && *s <= 8) {
                data.selected_slot = static_cast<std::uint8_t>(*s);
            }
        }
        if (const auto* inv = root->find("Inventory"); inv != nullptr) {
            if (const auto* items = inv->get_if<List>(); items != nullptr) {
                for (const auto& entry : *items) {
                    const auto slot = entry.find("Slot") ? entry.find("Slot")->scalar() : std::nullopt;
                    const auto id = entry.find("id") ? entry.find("id")->scalar() : std::nullopt;
                    if (!slot || !id) {
                        continue;
                    }
                    const auto window = nbt_slot_to_window(static_cast<std::int32_t>(*slot));
                    if (!window || *window >= data.inventory.size()) {
                        continue;
                    }
                    item::ItemStack stack;
                    stack.id = static_cast<std::int16_t>(*id);
                    stack.count = static_cast<std::uint8_t>(
                        entry.find("Count") && entry.find("Count")->scalar()
                            ? *entry.find("Count")->scalar()
                            : 1);
                    stack.damage = static_cast<std::int16_t>(
                        entry.find("Damage") && entry.find("Damage")->scalar()
                            ? *entry.find("Damage")->scalar()
                            : 0);
                    data.inventory[*window] = stack;
                }
            }
        }
        return data;
    }

    // 遗留 JSON：迁移为 .dat 后删除
    if (std::ifstream in{json_path}; in) {
        std::ostringstream buffer;
        buffer << in.rdbuf();
        if (auto legacy = parse_legacy_json(buffer.str())) {
            legacy->uuid_with_dashes = data.uuid_with_dashes;
            legacy->username = data.username;
            if (const auto saved = save_locked(*legacy); saved) {
                std::error_code ec;
                std::filesystem::remove(json_path, ec);
            }
            return *legacy;
        }
    }
    return data;
}

Result<void> PlayerDataStore::save(const PlayerData& data) const {
    std::lock_guard<std::mutex> lock{mutex_};
    return save_locked(data);
}

// 假定 mutex_ 已被调用方持有（load_or_default 迁移分支复用）
Result<void> PlayerDataStore::save_locked(const PlayerData& data) const {
    auto nbt_bytes = nbt::serialize("", build_vanilla_nbt(data));
    if (!nbt_bytes) {
        return std::unexpected{std::move(nbt_bytes.error())};
    }
    auto gz = proto::deflate_gzip(ByteSpan{*nbt_bytes}, proto::kDefaultCompressionLevel);
    if (!gz) {
        return std::unexpected{std::move(gz.error())};
    }

    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    const auto path = std::filesystem::path{dir_} / (data.uuid_with_dashes + ".dat");
    const auto tmp = path.string() + ".tmp";
    {
        std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
        if (!out) {
            return make_error(ErrorCode::io, "cannot open player dat: " + tmp);
        }
        out.write(reinterpret_cast<const char*>(gz->data()),
                  static_cast<std::streamsize>(gz->size()));
        if (!out) {
            return make_error(ErrorCode::io, "cannot write player dat: " + tmp);
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        return make_error(ErrorCode::io, "cannot replace player dat: " + ec.message());
    }
    return {};
}

void PlayerDataStore::remove(std::string_view uuid_with_dashes) {
    std::lock_guard<std::mutex> lock{mutex_};
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path{dir_} / (std::string{uuid_with_dashes} + ".dat"), ec);
    std::filesystem::remove(std::filesystem::path{dir_} / (std::string{uuid_with_dashes} + ".json"), ec);
}

std::vector<std::string> PlayerDataStore::list_players() const {
    std::lock_guard<std::mutex> lock{mutex_};
    std::vector<std::string> out;
    std::error_code ec;
    if (!std::filesystem::exists(dir_, ec)) {
        return out;
    }
    for (const auto& entry : std::filesystem::directory_iterator{dir_, ec}) {
        if (entry.is_regular_file() &&
            (entry.path().extension() == ".dat" || entry.path().extension() == ".json")) {
            out.push_back(entry.path().stem().string());
        }
    }
    return out;
}

void PlayerDataStore::set_dir(std::string_view dir) {
    std::lock_guard<std::mutex> lock{mutex_};
    dir_ = std::string{dir};
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
}

} // namespace cyane::game