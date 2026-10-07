# 里程碑与当前状态

## 里程碑进度

- [x] **M0 工程骨架**（构建、日志、配置、线程池、无锁队列、测试框架）
- [x] **M1 协议与连接**（Handshake/Status/Ping、RSA/AES 加密登录、zlib 帧压缩）
- [x] **M2 世界与移动**（ChunkData 编码、玩家移动同步、多人可见、聊天、KeepAlive、动态区块流式加载）
- [x] **M3 玩法基础**（方块交互、46 格背包与窗口同步、箱子/工作台/熔炉、数据驱动配方、命令与 OP）
- [x] **M4 存档与世界兼容**（玩家 .dat 双向互通、**完整区块存储**、**原版真实世界加载**、`level.dat` 读写、**无损保存打补丁**、**掉落物与生物落盘**、小容器发射/投掷/漏斗）
- [ ] M5 世界生成（噪声地形、生物群系过渡、洞穴）
- [x] **M6 玩法进阶（核心大部完成）**（敌对生物 AI 与 A* 寻路、苦力怕引爆/骷髅举弓走位射箭、自然刷怪、完整击退与物理重力、经验系统与 XP 球、死亡掉落与动画）
- [ ] M7 插件基座（嵌入式 JVM、Bukkit API 核心子集、类加载器隔离、事件总线）
- [ ] M8 Bukkit API 覆盖扩展
- [ ] M9 NMS shim
- [ ] M10 性能与加固

---

## 已实现功能（按 Cuberite 功能域对照）

Cuberite（`/home/cycy/code/cuberite-master/src`）为架构与功能广度对标物。**✅**=已实现/对齐原版，**🟡**=部分实现，**⬜**=未开始。

| 功能域 | 实现位置 | 状态 | 详细说明 |
|---|---|---|---|
| **Protocol** 网络与握手 | `net/net_service` `net/connection_login` `proto/frame` `crypto/` | ✅ | SO_REUSEPORT 多 Reactor、AES-CFB8、RSA-1024、zlib 压缩 |
| **Protocol** Play 状态包通信 | `net/connection_play` `net/connection_world` `packet_writers` | ✅ | 覆盖 40+ 核心数据包，支持 Tab 补全、动画、视距过渡 |
| **WorldStorage** 区块存储 | `world/world` `world/region` `world/anvil` | ✅ | **完整区块存储**（全量 Section 数组）、Anvil `.mca` 无损读写 |
| **WorldStorage** 存档持久化 | `game/world_persistence` `world/level_dat` `game/player_data` | ✅ | 原版世界目录无损加载、`level.dat` 出生点对接、玩家 `.dat` 双向互通 |
| **Blocks** 破坏/放置/交互 | `net/connection_world` `world/blocks` | ✅ | 生存挖掘裂纹、方块更新广播、附着方块破损级联、门双半块同步 |
| **BlockEntities** 容器与工坊 | `net/container_store` `net/furnace_store` `net/crafting_table_store` | ✅ | 27 槽箱子、反应式熔炉状态机（燃料/冶炼进度条）、3×3 工作台 |
| **BlockEntities** 小容器 | `net/container_store` | ✅ | 发射器、投掷器、漏斗（9 槽窗口同步与落盘，无管道流转） |
| **Items** 背包与交互 | `item/player_inventory` `net/connection_inventory` | ✅ | 46 槽完整背包、游标拿放合并/拆分、Shift 快速转移 |
| **Items** 掉落物物理与拾取 | `net/item_drop` `net/connection_drops` | ✅ | 原版 EntityItem 物理（重力/摩擦/合并/熔岩弹起）、拾取延迟、Q 键丢弃 |
| **Items** 配方系统 | `item/crafting` + `config/recipes.toml` | ✅ | 数据驱动配方（有序 Shaped / 无序 Shapeless）、燃料燃烧时长映射 |
| **Mobs** 生物体系与 AI | `net/mob_manager` `world/mob_types` | ✅ | 僵尸近战追击、苦力怕引信膨胀自爆、骷髅举弓走位射箭、动物受击恐慌 |
| **Mobs** 寻路系统 | `world/pathfinding` | ✅ | **原版 A\* 寻路**（`WalkNodeProcessor` 节点分类、尺寸门控、防跳崖保证） |
| **Mobs** 生成与管理 | `net/mob_manager` `net/connection_mobs` | ✅ | 自然刷怪环（24~128 格）、刷怪蛋、繁殖/剪毛/挤奶交互、视距过渡补发 |
| **Combat & Damage** 战斗系统 | `net/connection_combat` `net/projectile_manager` | ✅ | 伤害抗性无敌窗、护甲吸伤、原版击退公式、箭矢物理碰撞、死亡掉落 |
| **Experience** 经验系统 | `net/xp_orb` `net/connection_experience` | ✅ | 经验球物理/合并/拾取、原版等级经验公式、`SetExperience` 同步 |
| **Commands & Permission** | `game/op_manager` `net/connection_play` | ✅ | 控制台/玩家共享命令（`/gamemode /tp /kill /op /deop /say /tps /help`） |
| **Generating** 世界生成 | `world/world`（超平坦 baseline） | ⬜ | M5 规划：多层噪声地形、生物群系、矿物生成 |
| **Plugin / JNI / Bukkit** | — | ⬜ | M7–M9 规划：内嵌 JVM、Bukkit API 核心实现、NMS shim |

---

## 交付记录

### M0 交付：工程骨架
- CMake + Clang 构建链路，`-Wall -Wextra -Wpedantic -Wconversion -Werror` 零告警标准。
- `core/error`（`std::expected` 错误传递）、`core/mpmc_queue`（Vyukov 有界无锁队列）、`core/log`（异步日志）、`core/config`（TOML 解析）、`core/time`（固定节拍定时器）、`core/thread_pool`。
- 代码生成：`data/registry.toml` → `generated/registry_meta.hpp`。

### M1 交付：协议与连接
- 协议底层：`core/bytes`（VarInt/VarLong/String/Position/BigEndian 流式读写）、`proto/frame`（zlib 压缩与解压防护）。
- 加密子系统：AES-128-CFB8 流加密、RSA-1024 密钥交换、SHA-1 验签。
- 网络驱动：`net/reactor`（epoll ET 驱动）、`net/connection`（每连接状态机与半包重组缓冲）、`net/net_service`（多 Reactor 工作池）。
- 状态响应：Handshake、Status（逐字节黄金向量对照）、Ping/Pong。

### M2 交付：世界与移动
- 区块传输：`world/chunk_codec`，1.12.2 Per-section 调色板与位压缩打包、光照数组与生物群系。
- 多人同步：`net/player_hub` 广播中心（邮箱机制），`SpawnPlayer`、`DestroyEntities`、`PlayerInfo` Tab 列表。
- 移动物理与可见性：Position/Look 双向流、视距范围广播与按区块动态加载。

### M3 交付：玩法基础
- 方块系统：`world/world` 共享世界状态、方块破坏/放置/朝向元数据、生存挖掘裂纹。
- 物品与容器：46 槽玩家背包、箱子（`ContainerStore`）、工作台（3×3 合成）、熔炉（燃烧与冶炼状态机）。
- 配方驱动：TOML 声明式配方解析、有序与无序匹配算法。
- 基础生物：被动生物漫游状态机。
- 命令与权限：控制台与玩家命令解释器、OP 权限表持久化。

### M4 交付：存档与世界兼容
- 玩家数据：`world/playerdata/<uuid>.dat`（原版 gzip NBT 格式，与原版双向互通）。
- 完整区块存储：全量 16-section 数组，内存脏标记管理，干净区块按需释放与重载。
- Anvil `.mca` 引擎：扇区动态分配、原版区块记录格式、`encode_chunk_merged` 无损增量覆写打补丁。
- 原版世界接轨：加载真实原版地图目录（实测 1500+ 区块零错误加载）、`level.dat` 出生点与种子读取。
- 实体与小容器落盘：掉落物与生物落盘为原版 `Entities` 列表，小容器落盘为 `TileEntities`。

### M6 交付（已落地）：玩法进阶与战斗
- 原版 A\* 寻路：`world/pathfinding.hpp`，`WalkNodeProcessor` 节点代价评估、包围盒尺寸门控、防跳崖保证。
- 敌对生物 AI：
  - 僵尸：近战索敌追击、破招反击、护甲减伤。
  - 苦力怕：视线接近膨胀引信（Swell metadata 12）、30 tick 倒计时引爆、范围方块与生物伤害。
  - 骷髅：举弓蓄力动画（SWINGING_ARMS）、退步与侧移走位（Kiting）、投掷物箭矢飞行物理。
  - 动物生态：喂食繁殖、剪羊毛、挤牛奶。
- 伤害与战斗物理：
  - 无敌窗机制（Hurt resistance window 20 tick）、护甲吸收公式。
  - 原版 Travel 空中/地面加速度分流、打击方向击退抖动。
  - 掉落物物理：`net/item_drop` 抛掷初速、地面滑动摩擦、流体浮力、合并吸附。
  - 经验系统：`net/xp_orb` 经验球掉落/拾取，原版升级公式与等级进度同步。
  - 视距与重生加固：跨区块实体视距过渡补发、重生内层区块瞬时突发下发、生存模式死亡掉落全背包。

---

## 性能与架构指标

| 指标项 | 目标基线 | 实测现状 |
|---|---|---|
| 空载 TPS | 20.0 (Tick 耗时 < 5ms) | 20.0 (空载 < 0.2ms) |
| 单包编解码延时 | < 500ns | < 200ns |
| 实体追踪视距过渡 | 零冗余发包 | 切比雪夫视距增量精确推演 |
| 区块流式下发 | 突发缓冲可控 | 初始 49 区块即时送达，后续 32 区块/tick |
| 单元测试覆盖 | 核心路径全覆盖 | 177 项自动化测试 100% 通过 |
