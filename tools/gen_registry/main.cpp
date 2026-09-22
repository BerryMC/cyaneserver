#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <system_error>

#include "cyane/core/config.hpp"

namespace {

struct Options {
    std::filesystem::path data;
    std::filesystem::path out;
};

[[nodiscard]] std::string render(const cyane::Config& config) {
    const std::int64_t protocol = config.get_or<std::int64_t>("protocol.version", 0);
    const std::string minecraft = config.get_or<std::string>("protocol.minecraft", "unknown");
    const std::string vanilla = config.get_or<std::string>("reference.vanilla_server", "");
    const std::string spigot = config.get_or<std::string>("reference.spigot_server", "");

    return std::format(
        "// 由 tools/gen_registry 从 data/registry.toml 生成，请勿手工编辑\n"
        "#pragma once\n"
        "\n"
        "#include <cstdint>\n"
        "#include <string_view>\n"
        "\n"
        "namespace cyane::generated {{\n"
        "\n"
        "inline constexpr int kProtocolVersion = {};\n"
        "inline constexpr std::string_view kMinecraftVersion = \"{}\";\n"
        "\n"
        "// 参考 jar 的 sha256，用于校验本机参考源与签名清单一致\n"
        "inline constexpr std::string_view kVanillaServerHash = \"{}\";\n"
        "inline constexpr std::string_view kSpigotServerHash = \"{}\";\n"
        "\n"
        "}}\n",
        protocol,
        minecraft,
        vanilla,
        spigot);
}

}

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--data" && i + 1 < argc) {
            options.data = argv[++i];
            continue;
        }
        if (arg == "--out" && i + 1 < argc) {
            options.out = argv[++i];
            continue;
        }
        std::print(stderr, "gen_registry: unknown argument '{}'\n", arg);
        return 2;
    }
    if (options.data.empty() || options.out.empty()) {
        std::print(stderr, "usage: gen_registry --data <registry.toml> --out <registry_meta.hpp>\n");
        return 2;
    }

    const auto config = cyane::Config::load_file(options.data);
    if (!config) {
        std::print(stderr,
                   "gen_registry: {}: {}\n",
                   cyane::to_string(config.error().code),
                   config.error().message);
        return 1;
    }

    std::error_code error;
    std::filesystem::create_directories(options.out.parent_path(), error);
    if (error) {
        std::print(stderr, "gen_registry: cannot create {}: {}\n", options.out.parent_path().string(), error.message());
        return 1;
    }

    const std::string header = render(*config);
    if (std::ifstream existing{options.out, std::ios::binary}; existing) {
        std::string current{std::istreambuf_iterator<char>{existing}, std::istreambuf_iterator<char>{}};
        if (current == header) {
            return 0;
        }
    }

    std::ofstream out{options.out, std::ios::binary | std::ios::trunc};
    if (!out) {
        std::print(stderr, "gen_registry: cannot write {}\n", options.out.string());
        return 1;
    }
    out << header;
    if (!out) {
        std::print(stderr, "gen_registry: failed writing {}\n", options.out.string());
        return 1;
    }
    std::print("gen_registry: wrote {} (protocol {})\n", options.out.string(), config->get_or<std::int64_t>("protocol.version", 0));
    return 0;
}
