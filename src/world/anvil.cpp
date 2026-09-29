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

} // namespace


namespace {

// 由 Chunk 的某 section 生成 Blocks/Data[/Add] 三个标签
struct SectionArrays {
    Bytes blocks;
    Bytes data;
    Bytes add;
    bool needs_add{false};
};

[[nodiscard]] SectionArrays build_section_arrays(const Section& section) {
    SectionArrays out;
    out.blocks.assign(kSectionBlockCount, std::byte{0});
    out.data.assign(kSectionBlockCount / 2, std::byte{0});
    out.add.assign(kSectionBlockCount / 2, std::byte{0});
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

    Compound level;
    compound_set(level, "xPos", nbt::make_i32(pos.x));
    compound_set(level, "zPos", nbt::make_i32(pos.z));
    compound_set(level, "LastUpdate", nbt::make_i64(0));
    compound_set(level, "Sections", nbt::make_list(std::move(section_list)));
    compound_set(level, "TileEntities", nbt::make_list(std::move(tile_entities)));

    Compound root;
    compound_set(root, "DataVersion", nbt::make_i32(kDataVersion1343));
    compound_set(root, "Level", nbt::make_compound(std::move(level)));

    return nbt::serialize("", nbt::make_compound(std::move(root)));
}


namespace {

// 保留原版区块中除箱子/熔炉外的方块实体（告示牌等未建模实体透传）
[[nodiscard]] bool is_modeled_tile_entity(const Value& entry) {
    const auto id = entry.find("id") ? entry.find("id")->text() : std::nullopt;
    return id && (*id == kChestEntityId || *id == kFurnaceEntityId);
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
                }
            }
        }
    }
    return out;
}

} // namespace cyane::world