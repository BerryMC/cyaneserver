#pragma once

#include <string_view>

namespace cyane {

// 首次启动（配置文件不存在）时写出的默认配置。
// 内容必须与 ServerConfig 的默认值保持一致，改字段时两处一起改。
inline constexpr std::string_view kDefaultServerToml = R"toml(# CyaneServer 配置（首次启动自动生成）
# 修改后重启生效。字段缺失时使用内置默认值。
# 键名以 ServerConfig::known_keys() 为准，写错的键会被静默忽略。

[network]
bind = "0.0.0.0"
port = 25565
motd = "CyaneServer 1.12.2"
# 正版验证。离线模式（false）允许任意用户名登录
online_mode = false
# 小于该字节数的包不压缩；-1 关闭压缩（默认 256）
# compression_threshold = 256
# IO 线程数（默认 1）
# io_threads = 1

[server]
max_players = 20
view_distance = 8
tick_rate = 20
# survival | creative | spectator
game_mode = "creative"
# 0 = 物理核数 - 1
worker_threads = 0
world_dir = "world"
# 世界存档自动保存间隔（秒；0 = 关闭；停机时总是保存）
autosave_interval = 300
player_data_dir = "world/playerdata"
op_file = "config/ops.json"
recipe_file = "config/recipes.toml"
# 世界种子（留空为随机，支持数字或文本；已存在的 level.dat 优先使用其内部的 RandomSeed）
seed = ""

[log]
level = "info"
file = "logs/latest.log"
)toml";

}  // namespace cyane
