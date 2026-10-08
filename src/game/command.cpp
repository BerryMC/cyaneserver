#include "cyane/game/command.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "cyane/core/bytes.hpp"
#include "cyane/game/server.hpp"
#include "cyane/proto/packet_ids.hpp"

namespace cyane::game {

namespace {

[[nodiscard]] std::string_view trim_ws(std::string_view s) noexcept {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

[[nodiscard]] std::pair<std::string_view, std::vector<std::string_view>> parse_args(std::string_view text) {
    text = trim_ws(text);
    if (!text.empty() && text.front() == '/') {
        text.remove_prefix(1);
    }
    const auto space = text.find(' ');
    if (space == std::string_view::npos) {
        return {text, {}};
    }
    const std::string_view cmd = text.substr(0, space);
    std::string_view rest = trim_ws(text.substr(space + 1));
    std::vector<std::string_view> args;
    while (!rest.empty()) {
        const auto next_sp = rest.find(' ');
        if (next_sp == std::string_view::npos) {
            args.push_back(rest);
            break;
        }
        args.push_back(rest.substr(0, next_sp));
        rest = trim_ws(rest.substr(next_sp + 1));
    }
    return {cmd, args};
}

[[nodiscard]] std::optional<std::uint8_t> parse_mode(std::string_view s) noexcept {
    if (s == "survival" || s == "s" || s == "0") return proto::game_mode::kSurvival;
    if (s == "creative" || s == "c" || s == "1") return proto::game_mode::kCreative;
    if (s == "adventure" || s == "a" || s == "2") return proto::game_mode::kAdventure;
    if (s == "spectator" || s == "sp" || s == "3") return proto::game_mode::kSpectator;
    return std::nullopt;
}

// gameMode.survival = "Survival Mode" 等（原版语言文件 gameMode.<name>）
[[nodiscard]] std::string_view mode_to_string(std::uint8_t mode) noexcept {
    switch (mode) {
        case proto::game_mode::kSurvival: return "Survival Mode";
        case proto::game_mode::kCreative: return "Creative Mode";
        case proto::game_mode::kAdventure: return "Adventure Mode";
        case proto::game_mode::kSpectator: return "Spectator Mode";
        default: return "Unknown";
    }
}

[[nodiscard]] std::optional<double> parse_double(std::string_view s) noexcept {
    try {
        size_t idx = 0;
        const double val = std::stod(std::string(s), &idx);
        if (idx == s.size()) return val;
    } catch (...) {
        return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::int64_t> parse_int64(std::string_view s) noexcept {
    std::int64_t val = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), val);
    if (ec == std::errc{} && ptr == s.data() + s.size()) return val;
    return std::nullopt;
}

[[nodiscard]] std::optional<std::int16_t> resolve_item_id(std::string_view name_or_id) noexcept {
    if (const auto num = parse_int64(name_or_id)) {
        if (*num >= 0 && *num <= 32767) return static_cast<std::int16_t>(*num);
    }
    // 原版常用物品名映射
    static const std::unordered_map<std::string_view, std::int16_t> kItems = {
        {"stone", 1}, {"grass", 2}, {"dirt", 3}, {"cobblestone", 4}, {"planks", 5},
        {"sapling", 6}, {"bedrock", 7}, {"sand", 12}, {"gravel", 13}, {"gold_ore", 14},
        {"iron_ore", 15}, {"coal_ore", 16}, {"log", 17}, {"wood", 17}, {"leaves", 18},
        {"glass", 20}, {"lapis_ore", 21}, {"sandstone", 24}, {"wool", 35}, {"gold_block", 41},
        {"iron_block", 42}, {"brick_block", 45}, {"tnt", 46}, {"bookshelf", 47},
        {"mossy_cobblestone", 48}, {"obsidian", 49}, {"torch", 50}, {"fire", 51},
        {"chest", 54}, {"crafting_table", 58}, {"furnace", 61}, {"ladder", 65},
        {"snow", 78}, {"ice", 79}, {"clay", 82}, {"fence", 85}, {"glowstone", 89},
        {"iron_door", 330}, {"wooden_door", 324},
        {"apple", 260}, {"bow", 261}, {"arrow", 262}, {"coal", 263}, {"diamond", 264},
        {"iron_ingot", 265}, {"gold_ingot", 266}, {"iron_sword", 267}, {"wooden_sword", 268},
        {"wooden_shovel", 269}, {"wooden_pickaxe", 270}, {"wooden_axe", 271}, {"stone_sword", 272},
        {"diamond_sword", 276}, {"diamond_shovel", 277}, {"diamond_pickaxe", 278},
        {"diamond_axe", 279}, {"stick", 280}, {"bowl", 281}, {"string", 287},
        {"feather", 288}, {"gunpowder", 289}, {"wheat", 296}, {"bread", 297},
        {"flint", 318}, {"water_bucket", 326}, {"lava_bucket", 327}, {"bucket", 325},
        {"milk_bucket", 335}, {"saddle", 329}, {"redstone", 331}, {"snowball", 332},
        {"boat", 333}, {"leather", 334}, {"brick", 336}, {"clay_ball", 337},
        {"reeds", 338}, {"paper", 339}, {"book", 340}, {"slime_ball", 341},
        {"egg", 344}, {"compass", 345}, {"fishing_rod", 346}, {"clock", 347},
        {"glowstone_dust", 348}, {"bone", 352}, {"sugar", 353}, {"cake", 354},
        {"bed", 355}, {"cookie", 357}, {"shears", 359}, {"melon", 360},
        {"beef", 363}, {"cooked_beef", 364}, {"chicken", 365}, {"cooked_chicken", 366},
        {"ender_pearl", 368}, {"blaze_rod", 369}, {"ghast_tear", 370}, {"gold_nugget", 371},
        {"glass_bottle", 374}, {"spider_eye", 375}, {"fermented_spider_eye", 376},
        {"blaze_powder", 377}, {"magma_cream", 378}, {"brewing_stand", 379},
        {"emerald", 388}, {"item_frame", 389}, {"flower_pot", 390}, {"carrot", 391},
        {"potato", 392}, {"baked_potato", 393}, {"poisonous_potato", 394},
        {"golden_carrot", 396}, {"skull", 397}, {"nether_star", 399}, {"pumpkin_pie", 400},
        {"firework", 401}, {"enchanted_book", 403}, {"quartz", 406}, {"prismarine_shard", 409},
        {"prismarine_crystals", 410}, {"rabbit", 411}, {"cooked_rabbit", 412},
        {"mutton", 423}, {"cooked_mutton", 424}, {"armor_stand", 416}, {"iron_horse_armor", 417},
        {"golden_horse_armor", 418}, {"diamond_horse_armor", 419}, {"lead", 420},
        {"name_tag", 421}, {"shield", 442}, {"elytra", 443}, {"totem_of_undying", 449},
        {"shulker_shell", 450}, {"iron_nugget", 452}
    };
    std::string key{name_or_id};
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
    if (key.starts_with("minecraft:")) {
        key.erase(0, 10);
    }
    if (const auto it = kItems.find(key); it != kItems.end()) {
        return it->second;
    }
    return std::nullopt;
}

}  // namespace

bool CommandDispatcher::execute(CommandSender& sender, Server& server, std::string_view text) {
    const auto [cmd, args] = parse_args(text);
    if (cmd.empty()) {
        return true;
    }

    const std::uint8_t op = sender.op_level();

    // 1. help / ?
    if (cmd == "help" || cmd == "?") {
        sender.send_feedback("--- Showing help page 1 of 1 (/help <page>) ---");
        sender.send_feedback("/help [page|command name]");
        sender.send_feedback("/list");
        sender.send_feedback("/tps");
        if (op >= 2) {
            sender.send_feedback("/gamemode <mode> [player]");
            sender.send_feedback("/defaultgamemode <mode>");
            sender.send_feedback("/tp [target player] <destination player|x y z>");
            sender.send_feedback("/teleport <entity> <x y z>");
            sender.send_feedback("/kill [player|entity]");
            sender.send_feedback("/time <set|add|query> <value>");
            sender.send_feedback("/weather <clear|rain|thunder> [duration]");
            sender.send_feedback("/difficulty <new difficulty>");
            sender.send_feedback("/seed");
            sender.send_feedback("/give <player> <item> [amount] [data]");
            sender.send_feedback("/clear [player]");
            sender.send_feedback("/say <message ...>");
        }
        if (op >= 4) {
            sender.send_feedback("/op <player>");
            sender.send_feedback("/deop <player>");
            sender.send_feedback("/save-all [flush]");
            sender.send_feedback("/stop");
        }
        sender.send_feedback("Tip: Use the <tab> key while typing a command to auto-complete");
        return true;
    }

    // 2. tps（非原版命令，使用原版相近格式）
    if (cmd == "tps") {
        sender.send_feedback(std::format("TPS: {:.1f} | Online: {}", server.current_tps(), server.online_players()));
        return true;
    }

    // 3. list
    if (cmd == "list") {
        const auto names = server.player_names();
        sender.send_feedback(std::format("There are {}/{} players online:", names.size(), server.config().max_players));
        std::string list_str;
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (i > 0) list_str += ", ";
            list_str += names[i];
        }
        if (!list_str.empty()) {
            sender.send_feedback(list_str);
        }
        return true;
    }

    // 4. say
    if (cmd == "say") {
        if (args.empty()) {
            sender.send_feedback("/say <message ...>", true);
            return true;
        }
        std::string raw_msg;
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (i > 0) raw_msg += " ";
            raw_msg += args[i];
        }
        // chat.type.announcement = [%s] %s
        const std::string broadcast_msg = std::format("[{}] {}", sender.name(), raw_msg);
        server.broadcast_system_message(broadcast_msg);
        return true;
    }

    // 5. gamemode / gm
    if (cmd == "gamemode" || cmd == "gm") {
        if (args.empty()) {
            sender.send_feedback("/gamemode <mode> [player]", true);
            return true;
        }
        const auto mode = parse_mode(args[0]);
        if (!mode) {
            sender.send_feedback(std::format("Unknown game mode: '{}'", args[0]), true);
            return true;
        }
        if (args.size() == 1) {
            if (!sender.is_player()) {
                sender.send_feedback("Console must specify a player: /gamemode <mode> <player>", true);
                return true;
            }
            if (op < 2) {
                sender.send_feedback("You do not have permission to use this command.", true);
                return true;
            }
            if (server.set_player_gamemode(sender.name(), args[0])) {
                // commands.gamemode.success.self = Set own game mode to %s
                sender.send_feedback(std::format("Set own game mode to {}", mode_to_string(*mode)));
            }
            return true;
        }
        if (op < 2) {
            sender.send_feedback("You do not have permission to use this command.", true);
            return true;
        }
        const std::string_view target = args[1];
        if (server.set_player_gamemode(target, args[0])) {
            // commands.gamemode.success.other = Set %s's game mode to %s
            sender.send_feedback(std::format("Set {}'s game mode to {}", target, mode_to_string(*mode)));
        } else {
            sender.send_feedback(std::format("Player '{}' not found", target), true);
        }
        return true;
    }

    // 6. defaultgamemode
    if (cmd == "defaultgamemode") {
        if (op < 2) {
            sender.send_feedback("You do not have permission to use this command.", true);
            return true;
        }
        if (args.empty()) {
            sender.send_feedback("/defaultgamemode <mode>", true);
            return true;
        }
        const auto mode = parse_mode(args[0]);
        if (!mode) {
            sender.send_feedback(std::format("Unknown game mode: '{}'", args[0]), true);
            return true;
        }
        // commands.defaultgamemode.success = The world's default game mode is now %s
        sender.send_feedback(std::format("The world's default game mode is now {}", mode_to_string(*mode)));
        return true;
    }

    // 7. tp / teleport
    if (cmd == "tp" || cmd == "teleport") {
        if (op < 2) {
            sender.send_feedback("You do not have permission to use this command.", true);
            return true;
        }
        if (args.empty()) {
            sender.send_feedback("/tp [target player] <destination player> OR /tp [target player] <x> <y> <z> [<yaw> <pitch>]", true);
            return true;
        }
        // /tp <x> <y> <z>
        if (args.size() == 3 && sender.is_player()) {
            const auto x = parse_double(args[0]);
            const auto y = parse_double(args[1]);
            const auto z = parse_double(args[2]);
            if (!x || !y || !z) {
                sender.send_feedback("Invalid coordinates", true);
                return true;
            }
            if (server.teleport_player(sender.name(), *x, *y, *z)) {
                // commands.tp.success.coordinates = Teleported %s to %s, %s, %s
                sender.send_feedback(std::format("Teleported {} to {:.1f}, {:.1f}, {:.1f}", sender.name(), *x, *y, *z));
            }
            return true;
        }
        // /tp <target_player>
        if (args.size() == 1) {
            if (!sender.is_player()) {
                sender.send_feedback("Console must specify source and target: /tp <player> <target>", true);
                return true;
            }
            if (auto* hub = server.hub()) {
                const auto others = hub->others(0);
                for (const auto& o : others) {
                    if (o.name == args[0]) {
                        if (server.teleport_player(sender.name(), o.x, o.y, o.z, o.yaw, o.pitch)) {
                            // commands.tp.success = Teleported %s to %s
                            sender.send_feedback(std::format("Teleported {} to {}", sender.name(), args[0]));
                        }
                        return true;
                    }
                }
            }
            sender.send_feedback(std::format("Player '{}' not found", args[0]), true);
            return true;
        }
        // /tp <player> <x> <y> <z>
        if (args.size() == 4) {
            const auto x = parse_double(args[1]);
            const auto y = parse_double(args[2]);
            const auto z = parse_double(args[3]);
            if (!x || !y || !z) {
                sender.send_feedback("Invalid coordinates", true);
                return true;
            }
            if (server.teleport_player(args[0], *x, *y, *z)) {
                sender.send_feedback(std::format("Teleported {} to {:.1f}, {:.1f}, {:.1f}", args[0], *x, *y, *z));
            } else {
                sender.send_feedback(std::format("Player '{}' not found", args[0]), true);
            }
            return true;
        }
        // /tp <player> <target_player>
        if (args.size() == 2) {
            if (auto* hub = server.hub()) {
                const auto others = hub->others(0);
                const net::PlayerSnapshot* target = nullptr;
                for (const auto& o : others) {
                    if (o.name == args[1]) {
                        target = &o;
                        break;
                    }
                }
                if (target != nullptr && server.teleport_player(args[0], target->x, target->y, target->z, target->yaw, target->pitch)) {
                    sender.send_feedback(std::format("Teleported {} to {}", args[0], args[1]));
                    return true;
                }
            }
            sender.send_feedback(std::format("Target player '{}' not found", args[1]), true);
            return true;
        }
        sender.send_feedback("/tp [target player] <destination player> OR /tp [target player] <x> <y> <z> [<yaw> <pitch>]", true);
        return true;
    }

    // 8. kill（支持原版实体选择器 @a @p @r @e @s）
    if (cmd == "kill") {
        if (args.empty()) {
            if (!sender.is_player()) {
                sender.send_feedback("/kill [player|entity]", true);
                return true;
            }
            (void)server.kill_player_by_name(sender.name());
            sender.send_feedback(std::format("Killed {}", sender.name()));
            return true;
        }
        if (op < 2) {
            sender.send_feedback("You do not have permission to use this command.", true);
            return true;
        }
        const std::string_view target = args[0];
        if (target == "@s") {
            if (!sender.is_player()) {
                sender.send_feedback("Console has no self entity to kill", true);
                return true;
            }
            (void)server.kill_player_by_name(sender.name());
            sender.send_feedback(std::format("Killed {}", sender.name()));
        } else if (target == "@a") {
            const auto killed = server.kill_all_players();
            if (killed.empty()) {
                sender.send_feedback("No players online");
            } else {
                for (const auto& name : killed) {
                    sender.send_feedback(std::format("Killed {}", name));
                }
            }
        } else if (target == "@p") {
            const auto* pos = sender.player_position();
            if (pos == nullptr) {
                sender.send_feedback("Console has no position to select nearest player", true);
                return true;
            }
            const auto name = server.kill_nearest_player(pos->x, pos->y, pos->z);
            if (!name.empty()) {
                sender.send_feedback(std::format("Killed {}", name));
            } else {
                sender.send_feedback("No player nearby", true);
            }
        } else if (target == "@r") {
            const auto name = server.kill_random_player();
            if (!name.empty()) {
                sender.send_feedback(std::format("Killed {}", name));
            } else {
                sender.send_feedback("No players online", true);
            }
        } else if (target == "@e") {
            const auto killed = server.kill_all_entities();
            if (killed.empty()) {
                sender.send_feedback("No entities to kill");
            } else {
                for (const auto& name : killed) {
                    sender.send_feedback(std::format("Killed {}", name));
                }
            }
        } else {
            if (server.kill_player_by_name(target)) {
                sender.send_feedback(std::format("Killed {}", target));
            } else {
                sender.send_feedback(std::format("Player '{}' not found", target), true);
            }
        }
        return true;
    }

    // 9. time
    if (cmd == "time") {
        if (op < 2) {
            sender.send_feedback("You do not have permission to use this command.", true);
            return true;
        }
        if (args.empty()) {
            sender.send_feedback("/time <set|add|query> <value>", true);
            return true;
        }
        if (args[0] == "set" && args.size() >= 2) {
            std::int64_t target_time = 0;
            if (args[1] == "day") target_time = 1000;
            else if (args[1] == "noon") target_time = 6000;
            else if (args[1] == "night") target_time = 13000;
            else if (args[1] == "midnight") target_time = 18000;
            else if (auto parsed = parse_int64(args[1])) target_time = *parsed;
            else {
                sender.send_feedback(std::format("Unknown time: '{}'", args[1]), true);
                return true;
            }
            server.set_time_of_day(target_time);
            // commands.time.set = Set the time to %s
            sender.send_feedback(std::format("Set the time to {}", target_time));
            return true;
        }
        if (args[0] == "add" && args.size() >= 2) {
            if (auto parsed = parse_int64(args[1])) {
                server.add_time(*parsed);
                // commands.time.added = Added %s to the time
                sender.send_feedback(std::format("Added {} to the time", *parsed));
            } else {
                sender.send_feedback(std::format("Invalid value: '{}'", args[1]), true);
            }
            return true;
        }
        if (args[0] == "query" && args.size() >= 2) {
            if (args[1] == "daytime") {
                // commands.time.query = Time is %s
                sender.send_feedback(std::format("Time is {}", server.time_of_day()));
            } else if (args[1] == "gametime") {
                sender.send_feedback(std::format("Time is {}", server.world_age()));
            } else if (args[1] == "day") {
                sender.send_feedback(std::format("Day is {}", server.world_age() / 24000));
            }
            return true;
        }
        sender.send_feedback("/time <set|add|query> <value>", true);
        return true;
    }

    // 10. weather
    if (cmd == "weather") {
        if (op < 2) {
            sender.send_feedback("You do not have permission to use this command.", true);
            return true;
        }
        if (args.empty()) {
            sender.send_feedback("/weather <clear|rain|thunder> [duration in seconds]", true);
            return true;
        }
        if (args[0] == "clear") {
            // commands.weather.clear = Changing to clear weather
            sender.send_feedback("Changing to clear weather");
        } else if (args[0] == "rain") {
            // commands.weather.rain = Changing to rainy weather
            sender.send_feedback("Changing to rainy weather");
        } else if (args[0] == "thunder") {
            // commands.weather.thunder = Changing to rain and thunder
            sender.send_feedback("Changing to rain and thunder");
        } else {
            sender.send_feedback(std::format("Unknown weather: '{}'", args[0]), true);
        }
        return true;
    }

    // 11. difficulty
    if (cmd == "difficulty") {
        if (op < 2) {
            sender.send_feedback("You do not have permission to use this command.", true);
            return true;
        }
        if (args.empty()) {
            sender.send_feedback("/difficulty <new difficulty>", true);
            return true;
        }
        // commands.difficulty.success = Set game difficulty to %s
        sender.send_feedback(std::format("Set game difficulty to {}", args[0]));
        return true;
    }

    // 12. seed
    if (cmd == "seed") {
        if (op < 2) {
            sender.send_feedback("You do not have permission to use this command.", true);
            return true;
        }
        // commands.seed.success = Seed: %s
        sender.send_feedback("Seed: [0]");
        return true;
    }

    // 13. give
    if (cmd == "give") {
        if (op < 2) {
            sender.send_feedback("You do not have permission to use this command.", true);
            return true;
        }
        if (args.size() < 2) {
            sender.send_feedback("/give <player> <item> [amount] [data] [dataTag]", true);
            return true;
        }
        const auto item_id = resolve_item_id(args[1]);
        if (!item_id) {
            // commands.give.item.notFound = There is no such item with name %s
            sender.send_feedback(std::format("There is no such item with name {}", args[1]), true);
            return true;
        }
        std::uint8_t count = 1;
        if (args.size() >= 3) {
            if (auto p = parse_int64(args[2])) count = static_cast<std::uint8_t>(std::clamp(*p, std::int64_t{1}, std::int64_t{64}));
        }
        std::int16_t damage = 0;
        if (args.size() >= 4) {
            if (auto p = parse_int64(args[3])) damage = static_cast<std::int16_t>(*p);
        }
        if (server.give_player_item(args[0], *item_id, count, damage)) {
            // commands.give.success = Given %s * %s to %s
            sender.send_feedback(std::format("Given {} * {} to {}", args[1], count, args[0]));
        } else {
            sender.send_feedback(std::format("Player '{}' not found", args[0]), true);
        }
        return true;
    }

    // 14. clear
    if (cmd == "clear") {
        if (args.empty()) {
            if (!sender.is_player()) {
                sender.send_feedback("/clear [player]", true);
                return true;
            }
            if (op < 2) {
                sender.send_feedback("You do not have permission to use this command.", true);
                return true;
            }
            (void)server.clear_player_inventory(sender.name());
            // commands.clear.success = Cleared the inventory of %s, removing %s items
            sender.send_feedback(std::format("Cleared the inventory of {}, removing all items", sender.name()));
            return true;
        }
        if (op < 2) {
            sender.send_feedback("You do not have permission to use this command.", true);
            return true;
        }
        if (server.clear_player_inventory(args[0])) {
            sender.send_feedback(std::format("Cleared the inventory of {}, removing all items", args[0]));
        } else {
            // commands.clear.failure = Could not clear the inventory of %s, no items to remove
            sender.send_feedback(std::format("Could not clear the inventory of {}, no items to remove", args[0]), true);
        }
        return true;
    }

    // 15. op (等级 4)
    if (cmd == "op") {
        if (op < 4) {
            sender.send_feedback("You do not have permission to use this command.", true);
            return true;
        }
        if (args.empty()) {
            sender.send_feedback("/op <player>", true);
            return true;
        }
        if (server.op_player(args[0])) {
            // commands.op.success = Opped %s
            sender.send_feedback(std::format("Opped {}", args[0]));
        } else {
            // commands.op.failed = Could not op %s
            sender.send_feedback(std::format("Could not op {}", args[0]), true);
        }
        return true;
    }

    // 16. deop
    if (cmd == "deop") {
        if (op < 4) {
            sender.send_feedback("You do not have permission to use this command.", true);
            return true;
        }
        if (args.empty()) {
            sender.send_feedback("/deop <player>", true);
            return true;
        }
        if (server.deop_player(args[0])) {
            // commands.deop.success = De-opped %s
            sender.send_feedback(std::format("De-opped {}", args[0]));
        } else {
            // commands.deop.failed = Could not deop %s
            sender.send_feedback(std::format("Could not deop {}", args[0]), true);
        }
        return true;
    }

    // 17. save-all / save
    if (cmd == "save-all" || cmd == "save") {
        if (op < 4) {
            sender.send_feedback("You do not have permission to use this command.", true);
            return true;
        }
        // commands.save.start = Saving...
        sender.send_feedback("Saving...");
        server.save_world_now();
        // commands.save.success = Saved the world
        sender.send_feedback("Saved the world");
        return true;
    }

    // 18. stop
    if (cmd == "stop") {
        if (op < 4) {
            sender.send_feedback("You do not have permission to use this command.", true);
            return true;
        }
        // commands.stop.start = Stopping the server
        sender.send_feedback("Stopping the server");
        server.request_stop();
        return false;
    }

    sender.send_feedback(std::format("Unknown command '{}'. Try /help for a list of commands.", cmd), true);
    return true;
}

std::vector<std::string> CommandDispatcher::tab_complete(const CommandSender& sender,
                                                        const Server& server,
                                                        std::string_view text) {
    (void)sender;
    std::vector<std::string> matches;
    // 只去前导斜杠，不去尾随空格（尾随空格标志着已进入参数补全）
    if (!text.empty() && text.front() == '/') {
        text.remove_prefix(1);
    }
    // 找最后一个空格位置，确定当前正在补全的词
    const auto last_space = text.rfind(' ');
    if (last_space == std::string_view::npos) {
        // 没有空格：补全命令名本身
        static constexpr std::string_view kAllCommands[] = {
            "help", "tps", "list", "gamemode", "defaultgamemode", "tp", "teleport",
            "kill", "time", "weather", "difficulty", "seed", "give", "clear",
            "say", "op", "deop", "save-all", "save", "stop"
        };
        for (const auto c : kAllCommands) {
            if (c.starts_with(text)) {
                matches.push_back("/" + std::string(c));
            }
        }
        return matches;
    }

    // 有空格：取命令名和当前词前缀
    const std::string_view cmd = text.substr(0, text.find(' '));
    const std::string_view prefix = text.substr(last_space + 1);
    const auto names = server.player_names();

    // 补全玩家名或实体选择器
    if (cmd == "tp" || cmd == "teleport" || cmd == "kill" || cmd == "op" || cmd == "deop" ||
        cmd == "give" || cmd == "clear" ||
        (cmd == "gamemode" && text.find(' ') != text.find(' ', text.find(' ') + 1))) {
        // 补全玩家名
        for (const auto& n : names) {
            if (n.starts_with(prefix)) {
                matches.push_back(n);
            }
        }
        // kill / tp 命令额外补全实体选择器
        if (cmd == "kill" || cmd == "tp" || cmd == "teleport") {
            static constexpr std::string_view kSelectors[] = {"@a", "@p", "@r", "@e", "@s"};
            for (const auto s : kSelectors) {
                if (s.starts_with(prefix)) {
                    matches.push_back(std::string(s));
                }
            }
        }
    } else if (cmd == "gamemode" || cmd == "defaultgamemode") {
        for (const auto m : {"survival", "creative", "adventure", "spectator"}) {
            if (std::string_view(m).starts_with(prefix)) {
                matches.push_back(m);
            }
        }
    } else if (cmd == "time") {
        // /time set <...> 或 /time <set|add|query>
        const auto first_space = text.find(' ');
        const std::string_view sub = text.substr(first_space + 1, last_space - first_space - 1);
        if (sub == "set") {
            for (const auto t : {"day", "noon", "night", "midnight"}) {
                if (std::string_view(t).starts_with(prefix)) {
                    matches.push_back(t);
                }
            }
        } else {
            for (const auto t : {"set", "add", "query"}) {
                if (std::string_view(t).starts_with(prefix)) {
                    matches.push_back(t);
                }
            }
        }
    } else if (cmd == "weather") {
        for (const auto w : {"clear", "rain", "thunder"}) {
            if (std::string_view(w).starts_with(prefix)) {
                matches.push_back(w);
            }
        }
    } else if (cmd == "difficulty") {
        for (const auto d : {"peaceful", "easy", "normal", "hard"}) {
            if (std::string_view(d).starts_with(prefix)) {
                matches.push_back(d);
            }
        }
    }
    return matches;
}

}  // namespace cyane::game
