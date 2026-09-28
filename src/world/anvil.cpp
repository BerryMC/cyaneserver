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

Result<Bytes> encode_chunk(ChunkPos pos,
                           std::span<const std::pair<std::uint32_t, std::uint16_t>> edits,
                           const ChunkEntities& entities) {
    if (edits.empty() && entities.empty()) {
        return make_error(ErrorCode::world, "refusing to encode an unedited chunk");
    }

    // 展开编辑到 per-section 缓冲（堆分配，避免 ~130KB 栈占用）：非 baseline 状态才算内容
    struct SectionBuffer {
        std::array<std::uint8_t, kSectionBlockCount> blocks{};
        std::array<std::uint8_t, kSectionBlockCount / 2> data{};
        std::array<std::uint8_t, kSectionBlockCount / 2> add{};
        std::array<bool, kSectionBlockCount> edited{};
        bool used{false};
        bool needs_add{false};
    };
    std::vector<SectionBuffer> sections(kSectionCount);

    for (const auto& [local, state] : edits) {
        const std::int32_t wy = static_cast<std::int32_t>(local >> 8);
        if (wy < 0 || wy >= kChunkSizeY || state == flat_baseline(wy)) {
            continue;
        }
        auto& section = sections[static_cast<std::size_t>(wy) / 16];
        const std::size_t index = local & 0xFFF;
        const auto id = block_id(state);
        const auto meta = state_meta(state);
        section.blocks[index] = static_cast<std::uint8_t>(id & 0xFF);
        section.data[index / 2] |= static_cast<std::uint8_t>(
            index % 2 == 0 ? meta & 0x0F : (meta & 0x0F) << 4);
        section.edited[index] = true;
        if (id > 0xFF) {
            const auto high = static_cast<std::uint8_t>((id >> 8) & 0x0F);
            section.add[index / 2] |= static_cast<std::uint8_t>(
                index % 2 == 0 ? high : high << 4);
            section.needs_add = true;
        }
        section.used = true;
    }

    List section_list;
    for (std::size_t sy = 0; sy < sections.size(); ++sy) {
        auto& section = sections[sy];
        if (!section.used) {
            continue;
        }
        // 未编辑位置填充超平坦基线：解码按 baseline 过滤即还原为纯编辑集，
        // 文件本身也是完整地形（原版工具可读）
        const std::int32_t base_y = static_cast<std::int32_t>(sy) * 16;
        for (std::size_t index = 0; index < kSectionBlockCount; ++index) {
            if (section.edited[index]) {
                continue;
            }
            const auto base = flat_baseline(base_y + static_cast<std::int32_t>(index >> 8));
            const auto base_id = block_id(base);
            const auto base_meta = state_meta(base);
            section.blocks[index] = static_cast<std::uint8_t>(base_id & 0xFF);
            if (base_id > 0xFF) {
                const auto high = static_cast<std::uint8_t>((base_id >> 8) & 0x0F);
                section.add[index / 2] |= static_cast<std::uint8_t>(
                    index % 2 == 0 ? high : high << 4);
                section.needs_add = true;
            }
            if (base_meta != 0) {
                section.data[index / 2] |= static_cast<std::uint8_t>(
                    index % 2 == 0 ? base_meta & 0x0F : (base_meta & 0x0F) << 4);
            }
        }
        Compound fields;
        compound_set(fields, "Y", nbt::make_i8(static_cast<std::int8_t>(sy)));
        Bytes blocks(kSectionBlockCount);
        std::transform(section.blocks.begin(), section.blocks.end(), blocks.begin(),
                       [](std::uint8_t b) { return static_cast<std::byte>(b); });
        compound_set(fields, "Blocks", nbt::make_byte_array(std::move(blocks)));
        Bytes data(kSectionBlockCount / 2);
        std::transform(section.data.begin(), section.data.end(), data.begin(),
                       [](std::uint8_t b) { return static_cast<std::byte>(b); });
        compound_set(fields, "Data", nbt::make_byte_array(std::move(data)));
        if (section.needs_add) {
            Bytes add(kSectionBlockCount / 2);
            std::transform(section.add.begin(), section.add.end(), add.begin(),
                           [](std::uint8_t b) { return static_cast<std::byte>(b); });
            compound_set(fields, "Add", nbt::make_byte_array(std::move(add)));
        }
        section_list.push_back(make_compound(std::move(fields)));
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
    compound_set(root, "Level", make_compound(std::move(level)));

    return nbt::serialize("", make_compound(std::move(root)));
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

    DecodedChunk out;
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
                const std::int32_t base_y = static_cast<std::int32_t>(*y) * 16;
                for (std::size_t index = 0; index < kSectionBlockCount; ++index) {
                    const auto low = std::to_integer<std::uint8_t>((*block_bytes)[index]);
                    std::uint16_t id = low;
                    if (add_bytes != nullptr && add_bytes->size() == kSectionBlockCount / 2) {
                        const auto nibble = std::to_integer<std::uint8_t>((*add_bytes)[index / 2]);
                        id |= static_cast<std::uint16_t>((index % 2 == 0 ? (nibble & 0x0F)
                                                                          : (nibble >> 4))
                                                          << 8);
                    }
                    std::uint16_t meta = 0;
                    if (data_bytes != nullptr && data_bytes->size() == kSectionBlockCount / 2) {
                        const auto nibble = std::to_integer<std::uint8_t>((*data_bytes)[index / 2]);
                        meta = index % 2 == 0 ? (nibble & 0x0F) : static_cast<std::uint16_t>(nibble >> 4);
                    }
                    const auto state = static_cast<std::uint16_t>((id << 4) | meta);
                    const std::int32_t wy = base_y + static_cast<std::int32_t>(index >> 8);
                    if (state != flat_baseline(wy)) {
                        const std::uint32_t local = (static_cast<std::uint32_t>(wy) << 8) |
                                                    static_cast<std::uint32_t>(index & 0xFF);
                        out.edits.emplace_back(local, state);
                    }
                }
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
                // 未知方块实体：忽略（vanilla 同样容忍）
            }
        }
    }
    return out;
}

} // namespace cyane::world