#include <cstddef>
#include <string_view>

#include "cyane/generated/registry_meta.hpp"
#include "test_framework.hpp"

CYANE_TEST(generated_metadata_matches_target_versions) {
    CYANE_CHECK_EQ(cyane::generated::kProtocolVersion, 340);
    CYANE_CHECK_EQ(cyane::generated::kMinecraftVersion, std::string_view{"1.12.2"});
}

CYANE_TEST(generated_metadata_carries_reference_hashes) {
    CYANE_CHECK_EQ(cyane::generated::kVanillaServerHash.size(), std::size_t{64});
    CYANE_CHECK_EQ(cyane::generated::kSpigotServerHash.size(), std::size_t{64});
    CYANE_CHECK(cyane::generated::kVanillaServerHash.starts_with("fe1f9274"));
    CYANE_CHECK(cyane::generated::kSpigotServerHash.starts_with("ff5440e1"));
}
