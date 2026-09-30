#include "cyane/world/anvil.hpp"

#include <algorithm>
#include <cstring>

#include "cyane/world/blocks.hpp"
#include "cyane/world/nbt.hpp"

namespace cyane::world {

namespace {

using nbt::Compound;
using nbt::List;
using nbt::Tag;
using nbt::Value;

// 1.12.2 的 DataVersion
constexpr std::int32_t kDataVersion1343 = 1343;

constexpr std::string_view kChestEntityId = "minecraft:chest";
constexpr std::string_view kFurnaceEntityId = "minecraft:furnace";
constexpr std::string_view kItemEntityId = "minecraft:item";
constexpr std::string_view kDispenserEntityId = "minecraft:dispenser";
constexpr std::string_view kDropperEntityId = "minecraft:dropper";
constexpr std::string_view kHopperEntityId = "minecraft:hopper";
// 被动生物实体 id ↔ 1.12.2 SpawnMob 类型（90 猪 91 羊 92 牛 93 鸡）
constexpr std::string_view kMobEntityIds[] = {"minecraft:pig", "minecraft:sheep",
                                              "minecraft:cow", "minecraft:chicken"};
constexpr std::int32_t kMobSpawnTypes[] = {90, 91, 92, 93};

void compound_set(Compound& fields, std::string name, Value value) {
    for (auto& [key, existing] : fields) {
        if (key == name) {
            existing = std::move(value);
            return;
        }
    }
    fields.emplace_back(std::move(name), std::move(value));
}

Value item_list(std::span<const item::ItemStack> slots) {
    List items;
    for (std::size_t index = 0; index < slots.size(); ++index) {
        const auto& stack = slots[index];
        if (stack.empty()) {
            continue;
        }
        Compound entry;
        compound_set(entry, "Slot", nbt::make_i8(static_cast<std::int8_t>(index)));
        compound_set(entry, "id", nbt::make_i16(stack.id));
        compound_set(entry, "Damage", nbt::make_i16(stack.damage));
        compound_set(entry, "Count", nbt::make_i8(static_cast<std::int8_t>(stack.count)));
        items.push_back(nbt::make_compound(std::move(entry)));
    }
    return nbt::make_list(std::move(items));
}

// 从 Items 列表恢复槽位（未知 Slot 越界忽略）
template <std::size_t N>
void read_items(const Value& items, std::array<item::ItemStack, N>& slots) {
    const auto* list = items.get_if<List>();
    if (list == nullptr) {
        return;
    }
    for (const auto& entry : *list) {
        const auto slot = entry.find("Slot") ? entry.find("Slot")->scalar() : std::nullopt;
        const auto id = entry.find("id") ? entry.find("id")->scalar() : std::nullopt;
        if (!slot || !id || *slot < 0 || static_cast<std::size_t>(*slot) >= N) {
            continue;
        }
        item::ItemStack stack;
        stack.id = static_cast<std::int16_t>(*id);
        stack.count = static_cast<std::uint8_t>(
            entry.find("Count") && entry.find("Count")->scalar()
                ? *entry.find("Count")->scalar()
                : 1);
        stack.damage = static_cast<std::int16_t>(
            entry.find("Damage") && entry.find("Damage")->scalar() ? *entry.find("Damage")->scalar() : 0);
        slots[static_cast<std::size_t>(*slot)] = stack;
    }
}

Value tile_entity(std::int64_t key, std::string_view id, std::span<const item::ItemStack> slots,
                  const StoredFurnace* furnace) {
    const auto [bx, by, bz] = unpack_block_pos(key);
    Compound fields;
    compound_set(fields, "id", nbt::make_string(std::string{id}));
    compound_set(fields, "x", nbt::make_i32(bx));
    compound_set(fields, "y", nbt::make_i32(by));
    compound_set(fields, "z", nbt::make_i32(bz));
    compound_set(fields, "Items", item_list(slots));
    if (furnace != nullptr) {
        compound_set(fields, "BurnTime", nbt::make_i16(static_cast<std::int16_t>(furnace->burn_left)));
        compound_set(fields, "CookTime", nbt::make_i16(static_cast<std::int16_t>(furnace->cook_time)));
        compound_set(fields, "CookTimeTotal",
                     nbt::make_i16(static_cast<std::int16_t>(furnace->burn_total)));
    }
    return nbt::make_compound(std::move(fields));
}

// 掉落物品实体：Entity{id:"minecraft:item", Pos:List<Double>×3, Motion, Item{Count,id,Damage}}
// 字段名与方块实体不同：实体用小写 id + Pos 列表（原版 1.12.2 实测，R-012）
Value item_entity(const StoredEntity& entity) {
    Compound item;
    compound_set(item, "id", nbt::make_i16(entity.stack.id));
    compound_set(item, "Count", nbt::make_i8(static_cast<std::int8_t>(entity.stack.count)));
    compound_set(item, "Damage", nbt::make_i16(entity.stack.damage));
    Compound fields;
    compound_set(fields, "id", nbt::make_string(std::string{kItemEntityId}));
    compound_set(fields, "Pos", nbt::make_list(List{nbt::make_f64(entity.x),
                                                    nbt::make_f64(entity.y),
                                                    nbt::make_f64(entity.z)}));
    compound_set(fields, "Motion", nbt::make_list(List{nbt::make_f64(0.0),
                                                       nbt::make_f64(0.0),
                                                       nbt::make_f64(0.0)}));
    compound_set(fields, "Health", nbt::make_f32(5.0F));
    compound_set(fields, "Age", nbt::make_i16(0));
    compound_set(fields, "Item", nbt::make_compound(std::move(item)));
    return nbt::make_compound(std::move(fields));
}

// 生物实体：Entity{id:"minecraft:pig"…, Pos, Motion, Rotation, Health}
// 最小集即可与原版互通：缺失字段（Age/Sheared/Attributes 等）由原版取默认值
Value mob_entity(const StoredMob& mob) {
    Compound fields;
    compound_set(fields, "id", nbt::make_string(std::string{kMobEntityIds
                                                 [mob.type >= 90 && mob.type <= 93
                                                      ? static_cast<std::size_t>(mob.type) - 90
                                                      : 0]}));
    compound_set(fields, "Pos", nbt::make_list(List{nbt::make_f64(mob.x),
                                                    nbt::make_f64(mob.y),
                                                    nbt::make_f64(mob.z)}));
    compound_set(fields, "Motion", nbt::make_list(List{nbt::make_f64(0.0),
                                                       nbt::make_f64(0.0),
                                                       nbt::make_f64(0.0)}));
    compound_set(fields, "Rotation",
                 nbt::make_list(List{nbt::make_f32(mob.yaw), nbt::make_f32(mob.pitch)}));
    compound_set(fields, "Health", nbt::make_f32(20.0F));
    return nbt::make_compound(std::move(fields));
}

// 解析一个 Entities 条目：仅还原 minecraft:item（Pos + Item 堆叠），其余实体忽略
bool read_item_entity(const Value& entry, StoredEntity& out) {
    const auto id = entry.find("id") ? entry.find("id")->text() : std::nullopt;
    if (!id || *id != kItemEntityId) {
        return false;
    }
    const auto* pos = entry.find("Pos");
    const auto* pos_list = pos != nullptr ? pos->get_if<List>() : nullptr;
    if (pos_list == nullptr || pos_list->size() != 3) {
        return false;
    }
    const auto axis = [&](std::size_t index) {
        if (const auto* d = (*pos_list)[index].get_if<double>()) {
            return *d;
        }
        if (const auto* f = (*pos_list)[index].get_if<float>()) {
            return static_cast<double>(*f);
        }
        return 0.0;
    };
    out.x = axis(0);
    out.y = axis(1);
    out.z = axis(2);
    if (const Value* item = entry.find("Item"); item != nullptr) {
        const auto count = item->find("Count") ? item->find("Count")->scalar() : std::nullopt;
        const auto item_id = item->find("id") ? item->find("id")->scalar() : std::nullopt;
        const auto damage = item->find("Damage") ? item->find("Damage")->scalar() : std::nullopt;
        if (count && item_id && *count > 0) {
            const auto damage_value =
                damage ? static_cast<std::int16_t>(*damage) : std::int16_t{0};
            out.stack = item::ItemStack{static_cast<std::int16_t>(*item_id),
                                        static_cast<std::uint8_t>(*count), damage_value};
        }
    }
    return true;
}

// 判断实体类型是否由我们建模（载入/保存两侧统一管理）：item + 4 种被动生物
[[nodiscard]] bool is_modeled_entity_id(std::string_view id) noexcept {
    if (id == kItemEntityId) {
        return true;
    }
    for (const auto mob_id : kMobEntityIds) {
        if (id == mob_id) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::optional<std::int32_t> mob_spawn_type(std::string_view id) noexcept {
    for (std::size_t i = 0; i < std::size(kMobEntityIds); ++i) {
        if (id == kMobEntityIds[i]) {
            return kMobSpawnTypes[i];
        }
    }
    return std::nullopt;
}

// 解析一个生物实体条目（Pos + Rotation；Health 等运行时字段不持久化）
bool read_mob_entity(const Value& entry, StoredMob& out) {
    const auto id = entry.find("id") ? entry.find("id")->text() : std::nullopt;
    if (!id) {
        return false;
    }
    const auto type = mob_spawn_type(*id);
    if (!type) {
        return false;
    }
    out.type = static_cast<std::uint8_t>(*type);
    const auto* pos = entry.find("Pos");
    const auto* pos_list = pos != nullptr ? pos->get_if<List>() : nullptr;
    if (pos_list == nullptr || pos_list->size() != 3) {
        return false;
    }
    const auto axis = [&](std::size_t index) {
        if (const auto* d = (*pos_list)[index].get_if<double>()) {
            return *d;
        }
        if (const auto* f = (*pos_list)[index].get_if<float>()) {
            return static_cast<double>(*f);
        }
        return 0.0;
    };
    out.x = axis(0);
    out.y = axis(1);
    out.z = axis(2);
    out.yaw = 0.0f;
    out.pitch = 0.0f;
    if (const Value* rot = entry.find("Rotation"); rot != nullptr) {
        if (const auto* rot_list = rot->get_if<List>(); rot_list != nullptr && rot_list->size() == 2) {
            if (const auto* yaw = (*rot_list)[0].get_if<float>()) {
                out.yaw = *yaw;
            }
            if (const auto* pitch = (*rot_list)[1].get_if<float>()) {
                out.pitch = *pitch;
            }
        }
    }
    return true;
}

// 解析一个小容器 TileEntity（发射器/投掷器/漏斗）槽位
bool read_small_container(std::string_view id, const Value& entry, StoredSmallContainer& out) {
    if (id == kDispenserEntityId) {
        out.kind = kSmallKindDispenser;
    } else if (id == kDropperEntityId) {
        out.kind = kSmallKindDropper;
    } else if (id == kHopperEntityId) {
        out.kind = kSmallKindHopper;
    } else {
        return false;
    }
    if (const Value* items = entry.find("Items"); items != nullptr) {
        std::array<item::ItemStack, kSmallPersistSlots> slots{};
        read_items(*items, slots);
        out.slots = slots;
    }
    return true;
}

} // namespace


namespace {

// 由 Chunk 的某 section 生成 Blocks/Data[/Add] 三个标签
struct SectionArrays {
    Bytes blocks;
    Bytes data;
    Bytes add;
    Bytes block_light;
    Bytes sky_light;
    bool needs_add{false};
};

[[nodiscard]] SectionArrays build_section_arrays(const Section& section) {
    SectionArrays out;
    out.blocks.assign(kSectionBlockCount, std::byte{0});
    out.data.assign(kSectionBlockCount / 2, std::byte{0});
    out.add.assign(kSectionBlockCount / 2, std::byte{0});
    out.block_light.assign(kLightArrayBytes, std::byte{0});
    out.sky_light.assign(kLightArrayBytes, std::byte{0});
    for (std::size_t index = 0; index < kSectionBlockCount; ++index) {
        const auto state = section.state(index);
        const auto id = block_id(state);
        const auto meta = state_meta(state);
        out.blocks[index] = static_cast<std::byte>(id & 0xFF);
        out.data[index / 2] |= static_cast<std::byte>(
            index % 2 == 0 ? meta & 0x0F : (meta & 0x0F) << 4);
        if (id > 0xFF) {
            const auto high = static_cast<std::uint8_t>((id >> 8) & 0x0F);
            out.add[index / 2] |= static_cast<std::byte>(index % 2 == 0 ? high : high << 4);
            out.needs_add = true;
        }
    }
    return out;
}

[[nodiscard]] bool section_all_air(const Section& section) {
    for (const auto state : section.states) {
        if (state != kStateAir) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] List build_tile_entities(const ChunkEntities& entities) {
    List out;
    for (const auto& [key, chest] : entities.chests) {
        out.push_back(tile_entity(key, kChestEntityId, chest, nullptr));
    }
    for (const auto& [key, furnace] : entities.furnaces) {
        out.push_back(tile_entity(key, kFurnaceEntityId, std::span{&furnace.input, 3}, &furnace));
    }
    for (const auto& [key, small] : entities.small_containers) {
        const std::string_view id = small.kind == kSmallKindHopper     ? kHopperEntityId
                                    : small.kind == kSmallKindDropper  ? kDropperEntityId
                                                                       : kDispenserEntityId;
        out.push_back(tile_entity(key, id, small.slots, nullptr));
    }
    return out;
}

} // namespace

Result<Bytes> encode_chunk(ChunkPos pos, const Chunk& chunk, const ChunkEntities& entities) {
    List section_list;
    for (std::size_t sy = 0; sy < chunk.sections().size(); ++sy) {
        const auto& section = chunk.sections()[sy];
        if (section.empty()) {
            continue;
        }
        bool all_air = true;
        for (const auto state : section.states) {
            if (state != kStateAir) {
                all_air = false;
                break;
            }
        }
        if (all_air) {
            continue;
        }
        Compound fields;
        compound_set(fields, "Y", nbt::make_i8(static_cast<std::int8_t>(sy)));
        Bytes blocks(kSectionBlockCount);
        Bytes data(kSectionBlockCount / 2);
        bool needs_add = false;
        Bytes add(kSectionBlockCount / 2);
        // 原版读取端对每个 section 无条件构造光照 NibbleArray（必须 2048 字节）；
        // 全 0 占位 + LightPopulated 缺省 0，原版加载后自行重算天光
        Bytes light(kLightArrayBytes, std::byte{0});
        for (std::size_t index = 0; index < kSectionBlockCount; ++index) {
            const auto state = section.states[index];
            const auto id = block_id(state);
            const auto meta = state_meta(state);
            blocks[index] = static_cast<std::byte>(id & 0xFF);
            data[index / 2] |= static_cast<std::byte>(index % 2 == 0 ? meta & 0x0F
                                                                      : (meta & 0x0F) << 4);
            if (id > 0xFF) {
                const auto high = static_cast<std::uint8_t>((id >> 8) & 0x0F);
                add[index / 2] |= static_cast<std::byte>(index % 2 == 0 ? high : high << 4);
                needs_add = true;
            }
        }
        compound_set(fields, "Blocks", nbt::make_byte_array(std::move(blocks)));
        compound_set(fields, "Data", nbt::make_byte_array(std::move(data)));
        if (needs_add) {
            compound_set(fields, "Add", nbt::make_byte_array(std::move(add)));
        }
        compound_set(fields, "BlockLight", nbt::make_byte_array(light));
        compound_set(fields, "SkyLight", nbt::make_byte_array(light));
        section_list.push_back(nbt::make_compound(std::move(fields)));
    }

    List tile_entities;
    for (const auto& [key, chest] : entities.chests) {
        tile_entities.push_back(tile_entity(key, kChestEntityId, chest, nullptr));
    }
    for (const auto& [key, furnace] : entities.furnaces) {
        tile_entities.push_back(tile_entity(key, kFurnaceEntityId,
                                            std::span{&furnace.input, 3}, &furnace));
    }
    for (const auto& [key, small] : entities.small_containers) {
        const std::string_view id = small.kind == kSmallKindHopper     ? kHopperEntityId
                                    : small.kind == kSmallKindDropper  ? kDropperEntityId
                                                                       : kDispenserEntityId;
        tile_entities.push_back(tile_entity(key, id, small.slots, nullptr));
    }

    // 掉落物品实体（原版 Entities 列表，ID=minecraft:item）
    List entity_list;
    for (const auto& item : entities.items) {
        entity_list.push_back(item_entity(item));
    }
    for (const auto& mob : entities.mobs) {
        entity_list.push_back(mob_entity(mob));
    }

    Compound level;
    compound_set(level, "xPos", nbt::make_i32(pos.x));
    compound_set(level, "zPos", nbt::make_i32(pos.z));
    compound_set(level, "LastUpdate", nbt::make_i64(0));
    compound_set(level, "Sections", nbt::make_list(std::move(section_list)));
    compound_set(level, "TileEntities", nbt::make_list(std::move(tile_entities)));
    compound_set(level, "Entities", nbt::make_list(std::move(entity_list)));

    Compound root;
    compound_set(root, "DataVersion", nbt::make_i32(kDataVersion1343));
    compound_set(root, "Level", nbt::make_compound(std::move(level)));

    return nbt::serialize("", nbt::make_compound(std::move(root)));
}


namespace {

// 保留原版区块中除已建模方块实体外的 TileEntity（告示牌等未建模实体透传）
[[nodiscard]] bool is_modeled_tile_entity(const Value& entry) {
    const auto id = entry.find("id") ? entry.find("id")->text() : std::nullopt;
    if (!id) {
        return false;
    }
    return *id == kChestEntityId || *id == kFurnaceEntityId || *id == kDispenserEntityId ||
           *id == kDropperEntityId || *id == kHopperEntityId;
}

} // namespace

Result<Bytes> encode_chunk_merged(ChunkPos pos, const Chunk& chunk, const ChunkEntities& entities,
                                  ByteSpan source_nbt) {
    if (source_nbt.empty()) {
        return encode_chunk(pos, chunk, entities);
    }
    auto root = nbt::parse(source_nbt);
    if (!root) {
        return encode_chunk(pos, chunk, entities);  // 坏源档：退化为整体重编码
    }
    auto* level = root->find_mut("Level");
    if (level == nullptr) {
        return encode_chunk(pos, chunk, entities);
    }

    // 逐 section 覆盖 Blocks/Data/Add，保留光照等字段
    if (auto* sections = level->find_mut("Sections"); sections != nullptr) {
        if (auto* list = std::get_if<List>(&sections->data); list != nullptr) {
            std::vector<bool> patched(chunk.sections().size(), false);
            for (auto& section : *list) {
                const auto y = section.find("Y") ? section.find("Y")->scalar() : std::nullopt;
                if (!y || *y < 0 || static_cast<std::size_t>(*y) >= chunk.sections().size()) {
                    continue;
                }
                const auto* model = chunk.section(static_cast<std::size_t>(*y));
                if (model == nullptr || model->states.size() != kSectionBlockCount) {
                    continue;
                }
                auto arrays = build_section_arrays(*model);
                section.set("Blocks", nbt::make_byte_array(std::move(arrays.blocks)));
                section.set("Data", nbt::make_byte_array(std::move(arrays.data)));
                if (arrays.needs_add) {
                    section.set("Add", nbt::make_byte_array(std::move(arrays.add)));
                } else if (section.find("Add") != nullptr) {
                    section.set("Add", nbt::make_byte_array(
                                           Bytes(kSectionBlockCount / 2, std::byte{0})));
                }
                patched[static_cast<std::size_t>(*y)] = true;
            }
            // 模型有而源档缺失的非空 section：追加
            for (std::size_t sy = 0; sy < chunk.sections().size(); ++sy) {
                const auto* model = chunk.section(sy);
                if (patched[sy] || model == nullptr || model->states.size() != kSectionBlockCount ||
                    section_all_air(*model)) {
                    continue;
                }
                auto arrays = build_section_arrays(*model);
                Compound fields;
                compound_set(fields, "Y", nbt::make_i8(static_cast<std::int8_t>(sy)));
                compound_set(fields, "Blocks", nbt::make_byte_array(std::move(arrays.blocks)));
                compound_set(fields, "Data", nbt::make_byte_array(std::move(arrays.data)));
                if (arrays.needs_add) {
                    compound_set(fields, "Add", nbt::make_byte_array(std::move(arrays.add)));
                }
                // 原版读取端无条件构造光照 NibbleArray：新增 section 必须带全 0 占位
                compound_set(fields, "BlockLight", nbt::make_byte_array(std::move(arrays.block_light)));
                compound_set(fields, "SkyLight", nbt::make_byte_array(std::move(arrays.sky_light)));
                list->push_back(nbt::make_compound(std::move(fields)));
            }
        }
    }

    // TileEntities：保留未建模实体，替换/追加箱子与熔炉
    List kept;
    if (auto* tile_entities = level->find_mut("TileEntities"); tile_entities != nullptr) {
        if (const auto* list = tile_entities->get_if<List>(); list != nullptr) {
            for (const auto& entry : *list) {
                if (!is_modeled_tile_entity(entry)) {
                    kept.push_back(entry);
                }
            }
        }
    }
    for (auto& entry : build_tile_entities(entities)) {
        kept.push_back(std::move(entry));
    }
    level->set("TileEntities", nbt::make_list(std::move(kept)));

    // Entities：保留原版未建模实体（敌对生物等），物品/被动生物由内存态重写
    {
        List kept_entities;
        if (auto* entities_field = level->find_mut("Entities"); entities_field != nullptr) {
            if (const auto* list = entities_field->get_if<List>(); list != nullptr) {
                for (const auto& entry : *list) {
                    const auto id = entry.find("id") ? entry.find("id")->text() : std::nullopt;
                    if (!id || !is_modeled_entity_id(*id)) {
                        kept_entities.push_back(entry);  // 未建模实体原样透传
                    }
                }
            }
        }
        for (const auto& item : entities.items) {
            kept_entities.push_back(item_entity(item));
        }
        for (const auto& mob : entities.mobs) {
            kept_entities.push_back(mob_entity(mob));
        }
        level->set("Entities", nbt::make_list(std::move(kept_entities)));
    }

    return nbt::serialize("", *root);
}

Result<DecodedChunk> decode_chunk(ByteSpan nbt_bytes) {
    auto root = nbt::parse(nbt_bytes);
    if (!root) {
        return std::unexpected{std::move(root.error())};
    }
    const Value* level = root->find("Level");
    if (level == nullptr) {
        return make_error(ErrorCode::world, "anvil chunk missing Level");
    }

    const auto pos_x = level->find("xPos") ? level->find("xPos")->scalar() : std::nullopt;
    const auto pos_z = level->find("zPos") ? level->find("zPos")->scalar() : std::nullopt;
    if (!pos_x || !pos_z) {
        return make_error(ErrorCode::world, "anvil chunk missing xPos/zPos");
    }
    DecodedChunk out;
    out.chunk = Chunk{ChunkPos{static_cast<std::int32_t>(*pos_x), static_cast<std::int32_t>(*pos_z)}};

    if (const Value* sections = level->find("Sections"); sections != nullptr) {
        if (const auto* list = sections->get_if<List>(); list != nullptr) {
            for (const auto& section : *list) {
                const auto y = section.find("Y") ? section.find("Y")->scalar() : std::nullopt;
                const Value* blocks = section.find("Blocks");
                const Value* data = section.find("Data");
                const Value* add = section.find("Add");
                if (!y || *y < 0 || *y >= kSectionCount || blocks == nullptr) {
                    continue;
                }
                const auto* block_bytes = blocks->get_if<Bytes>();
                if (block_bytes == nullptr || block_bytes->size() < kSectionBlockCount) {
                    continue;
                }
                const auto* data_bytes = data != nullptr ? data->get_if<Bytes>() : nullptr;
                const auto* add_bytes = add != nullptr ? add->get_if<Bytes>() : nullptr;
                Section loaded;
                loaded.states.resize(kSectionBlockCount, kStateAir);
                for (std::size_t index = 0; index < kSectionBlockCount; ++index) {
                    auto id = static_cast<std::uint16_t>(
                        std::to_integer<std::uint8_t>((*block_bytes)[index]));
                    if (add_bytes != nullptr && add_bytes->size() == kSectionBlockCount / 2) {
                        const auto nibble =
                            std::to_integer<std::uint8_t>((*add_bytes)[index / 2]);
                        id |= static_cast<std::uint16_t>(
                            (index % 2 == 0 ? (nibble & 0x0F) : (nibble >> 4)) << 8);
                    }
                    std::uint16_t meta = 0;
                    if (data_bytes != nullptr && data_bytes->size() == kSectionBlockCount / 2) {
                        const auto nibble =
                            std::to_integer<std::uint8_t>((*data_bytes)[index / 2]);
                        meta = index % 2 == 0 ? (nibble & 0x0F)
                                              : static_cast<std::uint16_t>(nibble >> 4);
                    }
                    loaded.states[index] = static_cast<std::uint16_t>((id << 4) | meta);
                }
                out.chunk.set_section(static_cast<std::size_t>(*y), std::move(loaded));
            }
        }
    }

    if (const Value* entities = level->find("TileEntities"); entities != nullptr) {
        if (const auto* list = entities->get_if<List>(); list != nullptr) {
            for (const auto& entity : *list) {
                const auto id = entity.find("id") ? entity.find("id")->text() : std::nullopt;
                const auto x = entity.find("x") ? entity.find("x")->scalar() : std::nullopt;
                const auto y = entity.find("y") ? entity.find("y")->scalar() : std::nullopt;
                const auto z = entity.find("z") ? entity.find("z")->scalar() : std::nullopt;
                if (!id || !x || !y || !z) {
                    continue;
                }
                const std::int64_t key = pack_block_pos(static_cast<std::int32_t>(*x),
                                                        static_cast<std::int32_t>(*y),
                                                        static_cast<std::int32_t>(*z));
                if (*id == kChestEntityId) {
                    StoredChest chest{};
                    if (const Value* items = entity.find("Items"); items != nullptr) {
                        read_items(*items, chest);
                    }
                    out.entities.chests.emplace_back(key, std::move(chest));
                } else if (*id == kFurnaceEntityId) {
                    StoredFurnace furnace;
                    std::array<item::ItemStack, 3> slots{};
                    if (const Value* items = entity.find("Items"); items != nullptr) {
                        read_items(*items, slots);
                    }
                    furnace.input = slots[0];
                    furnace.fuel = slots[1];
                    furnace.output = slots[2];
                    const auto burn = entity.find("BurnTime")
                                          ? entity.find("BurnTime")->scalar()
                                          : std::nullopt;
                    const auto cook = entity.find("CookTime")
                                          ? entity.find("CookTime")->scalar()
                                          : std::nullopt;
                    const auto total = entity.find("CookTimeTotal")
                                           ? entity.find("CookTimeTotal")->scalar()
                                           : std::nullopt;
                    furnace.burn_left = burn ? static_cast<std::int32_t>(*burn) : 0;
                    furnace.cook_time = cook ? static_cast<std::int32_t>(*cook) : 0;
                    furnace.burn_total = total ? static_cast<std::int32_t>(*total) : 0;
                    out.entities.furnaces.emplace_back(key, std::move(furnace));
                } else {
                    StoredSmallContainer small;
                    if (read_small_container(*id, entity, small)) {
                        out.entities.small_containers.emplace_back(key, std::move(small));
                    }
                }
            }
        }
    }

    if (const Value* entities = level->find("Entities"); entities != nullptr) {
        if (const auto* list = entities->get_if<List>(); list != nullptr) {
            for (const auto& entry : *list) {
                StoredEntity item;
                if (read_item_entity(entry, item)) {
                    out.entities.items.push_back(std::move(item));
                    continue;
                }
                StoredMob mob;
                if (read_mob_entity(entry, mob)) {
                    out.entities.mobs.push_back(std::move(mob));
                }
            }
        }
    }
    return out;
}

} // namespace cyane::world