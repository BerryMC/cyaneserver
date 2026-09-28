#include "cyane/world/nbt.hpp"
#include "test_framework.hpp"

using namespace cyane;
namespace nbt = cyane::world::nbt;

CYANE_TEST(nbt_scalar_round_trip) {
    nbt::Compound root;
    root.emplace_back("int", nbt::make_i32(-123456));
    root.emplace_back("short", nbt::make_i16(-300));
    root.emplace_back("byte", nbt::make_i8(-5));
    root.emplace_back("long", nbt::make_i64(-9007199254740993ll));
    root.emplace_back("text", nbt::make_string("minecraft:chest"));
    auto bytes = nbt::serialize("", nbt::make_compound(std::move(root)));
    CYANE_CHECK(bytes.has_value());

    auto parsed = nbt::parse(ByteSpan{*bytes});
    CYANE_CHECK(parsed.has_value());
    CYANE_CHECK_EQ(parsed->find("int")->scalar().value_or(0), -123456);
    CYANE_CHECK_EQ(parsed->find("short")->scalar().value_or(0), -300);
    CYANE_CHECK_EQ(parsed->find("byte")->scalar().value_or(0), -5);
    CYANE_CHECK_EQ(parsed->find("long")->scalar().value_or(0), -9007199254740993ll);
    CYANE_CHECK_EQ(parsed->find("text")->text().value_or(""), "minecraft:chest");
}

CYANE_TEST(nbt_nested_compound_and_list) {
    nbt::Compound inner;
    inner.emplace_back("x", nbt::make_i32(7));
    nbt::List list;
    list.push_back(nbt::make_i16(1));
    list.push_back(nbt::make_i16(-2));
    nbt::Compound root;
    root.emplace_back("inner", nbt::make_compound(std::move(inner)));
    root.emplace_back("list", nbt::make_list(std::move(list)));
    Bytes payload{std::byte{0xAB}, std::byte{0xCD}, std::byte{0x00}};
    root.emplace_back("bytes", nbt::make_byte_array(std::move(payload)));

    auto bytes = nbt::serialize("root", nbt::make_compound(std::move(root)));
    CYANE_CHECK(bytes.has_value());
    auto parsed = nbt::parse(ByteSpan{*bytes});
    CYANE_CHECK(parsed.has_value());
    const auto* inner_value = parsed->find("inner");
    CYANE_CHECK(inner_value != nullptr);
    CYANE_CHECK_EQ(inner_value->find("x")->scalar().value_or(0), 7);
    const auto* list_value = parsed->find("list");
    CYANE_CHECK(list_value != nullptr);
    const auto* items = list_value->get_if<nbt::List>();
    CYANE_CHECK(items != nullptr && items->size() == 2);
    CYANE_CHECK_EQ((*items)[1].scalar().value_or(0), -2);
    const auto* raw = parsed->find("bytes");
    const auto* raw_bytes = raw != nullptr ? raw->get_if<Bytes>() : nullptr;
    CYANE_CHECK(raw_bytes != nullptr && raw_bytes->size() == 3);
}

CYANE_TEST(nbt_truncated_input_rejected) {
    const Bytes garbage{std::byte{0x0A}, std::byte{0x00}};
    auto parsed = nbt::parse(ByteSpan{garbage});
    CYANE_CHECK(!parsed.has_value());
}

