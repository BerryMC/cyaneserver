#include "cyane/game/command.hpp"

#include <string>
#include <string_view>
#include <vector>

#include "cyane/game/server.hpp"

namespace cyane::game {

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
    // 解析参数序号（第几个空格 = 第几个参数，0-based）
    const auto first_space = text.find(' ');
    const std::string_view cmd = text.substr(0, first_space);
    const std::string_view prefix = text.substr(last_space + 1);
    // 计算当前是第几个参数（空格数 = 参数索引）
    int arg_index = 0;
    for (std::size_t i = 0; i < last_space; ++i) {
        if (text[i] == ' ') {
            ++arg_index;
            // 跳过连续空格
            while (i + 1 < last_space && text[i + 1] == ' ') ++i;
        }
    }
    const auto names = server.player_names();

    // 对齐原版各命令的 getTabCompletions 行为
    if (cmd == "gamemode" || cmd == "gm") {
        // args[0] = 模式名, args[1] = 玩家名
        if (arg_index == 0) {
            for (const auto m : {"survival", "creative", "adventure", "spectator"}) {
                if (std::string_view(m).starts_with(prefix)) {
                    matches.push_back(m);
                }
            }
        } else if (arg_index == 1) {
            for (const auto& n : names) {
                if (n.starts_with(prefix)) {
                    matches.push_back(n);
                }
            }
        }
    } else if (cmd == "defaultgamemode") {
        // args[0] = 模式名
        if (arg_index == 0) {
            for (const auto m : {"survival", "creative", "adventure", "spectator"}) {
                if (std::string_view(m).starts_with(prefix)) {
                    matches.push_back(m);
                }
            }
        }
    } else if (cmd == "tp") {
        // args[0] = 目标玩家 / 待传送玩家, args[1] = 目标玩家 / x 坐标
        if (arg_index == 0 || arg_index == 1) {
            for (const auto& n : names) {
                if (n.starts_with(prefix)) {
                    matches.push_back(n);
                }
            }
            for (const auto s : {"@p", "@a", "@r", "@e", "@s"}) {
                if (std::string_view(s).starts_with(prefix)) {
                    matches.push_back(s);
                }
            }
        }
    } else if (cmd == "teleport") {
        // teleport 命令在原版优先补全选择器与坐标
        if (arg_index == 0) {
            for (const auto& n : names) {
                if (n.starts_with(prefix)) {
                    matches.push_back(n);
                }
            }
            for (const auto s : {"@p", "@a", "@r", "@e", "@s"}) {
                if (std::string_view(s).starts_with(prefix)) {
                    matches.push_back(s);
                }
            }
        }
    } else if (cmd == "kill") {
        // args[0] = 实体名或选择器
        if (arg_index == 0) {
            for (const auto& n : names) {
                if (n.starts_with(prefix)) {
                    matches.push_back(n);
                }
            }
            for (const auto s : {"@a", "@p", "@r", "@e", "@s"}) {
                if (std::string_view(s).starts_with(prefix)) {
                    matches.push_back(s);
                }
            }
        }
    } else if (cmd == "give") {
        // args[0] = 玩家名, args[1] = 物品 id / 名称
        if (arg_index == 0) {
            for (const auto& n : names) {
                if (n.starts_with(prefix)) {
                    matches.push_back(n);
                }
            }
            for (const auto s : {"@p", "@a", "@r", "@e", "@s"}) {
                if (std::string_view(s).starts_with(prefix)) {
                    matches.push_back(s);
                }
            }
        } else if (arg_index == 1) {
            // 原版常用基础物品名简短补全候选
            static constexpr std::string_view kCommonItems[] = {
                "stone", "grass", "dirt", "cobblestone", "planks", "bedrock",
                "sand", "gravel", "gold_ore", "iron_ore", "coal_ore", "wood",
                "diamond_ore", "crafting_table", "furnace", "chest", "torch",
                "iron_sword", "diamond_sword", "apple", "bread", "water_bucket"
            };
            for (const auto it : kCommonItems) {
                if (it.starts_with(prefix)) {
                    matches.push_back(std::string(it));
                }
            }
        }
    } else if (cmd == "clear") {
        // args[0] = 玩家名
        if (arg_index == 0) {
            for (const auto& n : names) {
                if (n.starts_with(prefix)) {
                    matches.push_back(n);
                }
            }
        }
    } else if (cmd == "op") {
        // args[0] = 非 OP 玩家名（原版补全全部玩家）
        if (arg_index == 0) {
            for (const auto& n : names) {
                if (n.starts_with(prefix)) {
                    matches.push_back(n);
                }
            }
        }
    } else if (cmd == "deop") {
        // args[0] = OP 玩家名
        if (arg_index == 0) {
            for (const auto& n : names) {
                if (n.starts_with(prefix)) {
                    matches.push_back(n);
                }
            }
        }
    } else if (cmd == "time") {
        // args[0] = set/add/query, args[1] = day/night/数字
        if (arg_index == 0) {
            for (const auto a : {"set", "add", "query"}) {
                if (std::string_view(a).starts_with(prefix)) {
                    matches.push_back(a);
                }
            }
        } else if (arg_index == 1) {
            // 检查 args[0] 是否为 set
            const auto second_space = text.find(' ', first_space + 1);
            const auto sub = (second_space == std::string_view::npos)
                ? text.substr(first_space + 1)
                : text.substr(first_space + 1, second_space - first_space - 1);
            if (sub == "set") {
                for (const auto t : {"day", "night", "noon", "midnight", "0", "1000", "6000", "13000", "18000"}) {
                    if (std::string_view(t).starts_with(prefix)) {
                        matches.push_back(t);
                    }
                }
            } else if (sub == "query") {
                for (const auto q : {"daytime", "gametime", "day"}) {
                    if (std::string_view(q).starts_with(prefix)) {
                        matches.push_back(q);
                    }
                }
            }
        }
    } else if (cmd == "weather") {
        // args[0] = clear/rain/thunder
        if (arg_index == 0) {
            for (const auto w : {"clear", "rain", "thunder"}) {
                if (std::string_view(w).starts_with(prefix)) {
                    matches.push_back(w);
                }
            }
        }
    } else if (cmd == "difficulty") {
        // args[0] = peaceful/easy/normal/hard 或 0/1/2/3
        if (arg_index == 0) {
            for (const auto d : {"peaceful", "easy", "normal", "hard", "0", "1", "2", "3", "p", "e", "n", "h"}) {
                if (std::string_view(d).starts_with(prefix)) {
                    matches.push_back(d);
                }
            }
        }
    } else if (cmd == "save-all" || cmd == "save") {
        if (arg_index == 0) {
            if (std::string_view("flush").starts_with(prefix)) {
                matches.push_back("flush");
            }
        }
    }

    return matches;
}

}  // namespace cyane::game
