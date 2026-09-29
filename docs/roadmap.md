# 里程碑与当前状态

## 里程碑进度

- [x] 环境与依赖确认
- [x] 离线物化并校验 Spigot 1.12.2 jar（哈希比对通过，`tools/reproduce_jars.sh` 幂等）
- [x] 表面积与命名构成实测（见 `compat.md`）
- [x] ABI 编译链路验证（`javac -cp spigot.jar` 编译含 NMS 的类通过）
- [x] **M0 工程骨架**
- [x] **M1 协议与连接**（Handshake/Status/Ping/加密登录）
- [x] **M2 世界与移动**（超平坦区块、移动同步、多人可见、聊天、KeepAlive、动态区块加载）
- [x] **M3 玩法基础**（方块交互/物品栏/容器/合成/熔炉/命令/被动生物 AI）
- [~] **M4 存档与世界兼容**（玩家 .dat 双向互通 ✅、**完整区块存储** ✅、**原版世界加载** ✅、`level.dat` ✅（读 + 首启建档）、**保存无损化** ✅、**掉落物实体落盘** ✅；缺更多方块实体）
- [ ] M5 世界生成
- [ ] M6 玩法进阶
- [ ] M7 插件基座
- [ ] M8 Bukkit API 覆盖扩展
- [ ] M9 NMS shim
- [ ] M10 性能与加固

## 里程碑定义

### M0 — 工程骨架
CMake/构建、日志、配置、线程池、错误类型、测试框架、`data/` 生成管线、`jars/` 复现脚本。
**验收**：一条命令出可执行文件；空 tick 循环与单元测试通过。

### M1 — 协议与连接
Handshake/Status/Ping、Login（加密+压缩）、KeepAlive、Disconnect 全流程；reactor；抓包对照工具。
**验收**：原版 1.12.2 客户端连上看到 MOTD/玩家数，进入登录流程后正常断开。

### M2 — 世界与移动
Anvil 读取、Chunk Data 发送、玩家实体、移动同步、聊天、Tab、区块动态加载。
**验收**：客户端进世界自由移动，看到地形与他人聊天；100 假人稳 20 TPS。

### M3 — 玩法基础 ✅
方块破坏/放置、物品栏与窗口同步、容器、合成、熔炉、掉落物、生物生成与基础 AI、伤害与重生、命令与 OP。
**验收**：正常生存游玩 30 分钟无致命 bug。（已达成）

### M4 — 存档与世界兼容
玩家数据 `.dat`（原版 gzip NBT，双向互通）、区块 Anvil 读写；**完整区块存储**（全量状态数组替代超平坦+差量模型）、原版世界目录加载（真实地形）、`level.dat` 读写（种子/出生点/时间/gamerules）、保存无损化（未建模字段透传，消除有损重写）、更多方块实体与实体落盘。
**验收**：cyane 与原版服务器对同一 `world/` 目录交替运行，地形/方块/容器/玩家数据在两侧往返均完整保留。

### M5 — 世界生成
多层噪声地形 + 生物群系 + 洞穴 + 基础结构，基于 M4 完整区块模型生成并按原版格式落盘。
**验收**：新世界地形多样、群系过渡自然；生成的 `.mca` 原版客户端可直接游玩。

### M6 — 玩法进阶
敌对生物生成与战斗 AI（目标选择/寻路，参考 `Mobs/Monster.cpp`）、物理与重力（AABB 重叠分离、实体推动）、经验/附魔/药水。
**验收**：生存模式夜间可玩，战斗-掉落-拾取链路完整。

### M7 — 插件基座
JVM 嵌入、`cyane-bukkit.jar` 核心子集、第三方类路径、每插件类加载器、事件总线、调度器、命令、权限、`plugin.yml`、配置；**插件扫描器**与 **`tools/apidiff`**。
**验收**：自写 Hello 插件（`PlayerJoinEvent` + `/hello` + 周期任务 + `config.yml`）零改动运行；无监听器时事件开销 ≈ 0；apidiff 对 T0 类全绿。

### M8 — Bukkit API 覆盖扩展
补齐 `Inventory/ItemMeta/Enchantment/PotionEffect/Scoreboard/BossBar/Title/BlockData/Metadata/WorldEdit 所需 API`。
**验收**：20 个纯 Bukkit 真实插件跑通清单记入 `docs/plugin-compat.md`。

### M9 — NMS shim（T1 → 部分 T2）
按扫描器实测的需求频次排序实现：`Packet*` → `NBT*` → 聊天组件 → 枚举 → `Craft*` 句柄 + 镜像字段同步。
**验收**：shim 覆盖扫描器统计的 top-N 高频符号；取真实 NMS 插件实测通过。

### M10 — 性能与加固
光照引擎优化、序列化零拷贝化、区块生成吞吐（≥ 500 chunks/s / 4 核）；压测 1000 玩家、内存/GC 调优、崩溃恢复、安全（握手限流、封包校验、压缩炸弹防护）。
**验收**：性能目标达标，`docs/benchmarks.md` 有可复现数据。

## 已实现功能（按 Cuberite 功能域对照）

Cuberite（`/home/cycy/code/cuberite-master/src`）是功能广度的对标物。**✅**=可用，**🟡**=部分/有缺口，**⬜**=未开始。

| Cuberite 功能域 | 我们的实现 | 状态 |
|---|---|---|
| **Protocol** 握手/状态/登录/压缩/加密 | `net/connection_login` `proto/frame` `crypto` | ✅ |
| **Protocol** Play 包收发、移动、Tab、命令 | `net/connection_play` `connection_inventory` | ✅ |
| **WorldStorage** 区块读写 | 超平坦 `make_flat_chunk` + 内存编辑表 + **Anvil 编辑区块存档**（`world/region` `world/anvil`，停机/自动/手动落盘） | 🟡 编辑持久化完成，无原生地形 |
| **Blocks** 破坏/放置/碰撞/回滚 | `net/connection_world` `world/blocks` `world/world` | ✅ |
| **BlockEntities** 箱子 | `net/container_store` `connection_container` | ✅ |
| **BlockEntities** 工作台 | `net/crafting_table_store` `connection_table` | ✅ |
| **BlockEntities** 熔炉 | `net/furnace_store` `connection_furnace` | ✅ |
| **BlockEntities** 附魔台/铁砧/酿造台/漏斗/发射器/告示牌/唱片机 | — | ⬜ |
| **Items** 物品堆/背包/掉落物拾取 | `item/item_stack` `item/player_inventory` `net/item_drop` | ✅ |
| **Items** 合成（数据驱动） | `item/crafting` + `config/recipes.toml` | ✅ |
| **Items** 熔炼与燃料表 | `item/crafting::load_furnace_config` | ✅ |
| **Items** 附魔/药水/NBT 物品 | — | ⬜ |
| **Mobs** 被动生物与漫游 AI | `net/mob_manager` `connection_mobs` | 🟡 AI 简单 |
| **Mobs** 敌对生物/战斗/掉落表/生成权重 | `net/connection_combat`（仅伤害与死亡） | 🟡 |
| **Physics** 碰撞/重力/推动 | 仅放置时 AABB 检测 | 🟡 无重力 |
| **Generating** 地形/生物群系/结构/洞穴 | 仅超平坦 | ⬜ |
| **Registries** 配方/方块/物品/实体 | `item/crafting` `world/blocks` `mob_manager` | 🟡 部分 |
| **UI/Window** 窗口与槽区抽象 | 分散在各 `connection_*` | 🟡 无统一抽象 |
| **Commands** 控制台/权限 | `game/op_manager` `connection_play` | ✅ |
| **插件加载/JNI/Bukkit API** | — | ⬜ M7–M9 |

## 近期优先项

按里程碑顺序（M4 → M5 → M6），"收益/成本"在同级内排序：

1. **完整区块存储**（M4 核心）— World 从"超平坦基线+差量表"升级为全量状态模型，是地形加载与生成的共同地基。
2. **原版世界目录加载**（M4）— 读取原版 `world/`（真实地形 + `level.dat`），cyane 可接入既有存档。
3. **保存无损化**（M4）— 玩家/区块未建模字段透传，消除有损重写。
4. **世界生成**（M5）— 噪声地形、生物群系、洞穴；生成的 `.mca` 原版客户端可直接游玩。
5. **敌对生物与物理**（M6）— 目标选择/寻路、AABB 重力；经验/附魔随后。

## 交付记录

### M0 交付

| 模块 | 内容 |
|---|---|
| 构建 | CMake 4.4 + clang 22 + C++23，thin LTO，lld；自动探测 zlib-ng/OpenSSL/liburing |
| `core/error` | `std::expected` 错误传播，8 类 `ErrorCode` |
| `core/mpmc_queue` | Vyukov 有界 MPMC，缓存行对齐，零锁 |
| `core/log` | 无锁队列 + 后台 flusher；同时写控制台与文件 |
| `core/config` | TOML 子集解析，带行号错误，类型严格校验 |
| `core/time` | `Ticker` 固定节拍、`TickStats` 窗口统计 |
| `core/thread_pool` | 信号量唤醒 + MPMC 队列，任务异常隔离 |
| `game/server` | 20Hz tick 循环、优雅停机、控制台命令 |
| 代码生成 | `data/registry.toml` → `generated/registry_meta.hpp` |

已验证：40 tick @20Hz 精确 2.00s、0 过载；配置错误在启动期报错并退出码 1。

### M1 交付

| 模块 | 内容 |
|---|---|
| `core/bytes` | VarInt/VarLong/字符串/位置/大端编解码 + UTF-8 校验 |
| `proto/frame` | 帧编解码（压缩阈值 256），长度/解压上限防攻击 |
| `crypto` | AES-128-CFB8、RSA-1024、SHA-1、离线 UUID 派生 |
| `session` | `SessionService` + `MojangSessionService`（HTTPS 会话验证） |
| `net/reactor` | epoll ET + `data.ptr` 直分发 |
| `net/connection` | 状态机、半包重组、写入背压；**加密登录完整流程** |
| `net/net_service` | SO_REUSEPORT 多 reactor，EMFILE 退避 |
| `game/status` | 状态 JSON，键序与原版一致 |

**验证结论**：状态响应与原版服务端**逐字节一致**；协议不匹配给出原版同款 `Outdated client!`。

### M2 交付

| 模块 | 内容 |
|---|---|
| `world/chunk_codec` | Chunk Data(0x20) 线格式，per-section 调色板 + 打包 long[] + 光照 + 生物群系 |
| `net/player_hub` | 线程安全多人广播中心，每玩家带锁 mailbox |
| 移动同步 | Position/PositionLook/Look/Flying → EntityTeleport + EntityHeadLook |
| 可见性 | 登录互发 PlayerInfo+SpawnPlayer、退出广播 DestroyEntities+PlayerInfo(remove) |
| 聊天 / KeepAlive / 动态区块 | 广播、10s 心跳 30s 超时、按视距加载卸载区块 |
| 控制台 | `help`/`tps`/`say`/`stop` |

**验证结论**：两客户端可互见并聊天；出生点按实体 id 错开成网格。

### M3 交付

| 模块 | 内容 |
|---|---|
| `world/world` | 共享可编辑方块存储（mutex 保护） |
| `world/blocks` | 物品→方块状态映射，六向 face 增量 |
| `item/item_stack` | `ItemStack{id,count,damage}` 与 1.12.2 网络 slot 编解码 |
| `item/player_inventory` | 46 槽背包与热区栏→窗口槽映射 |
| 方块交互 | 破坏置空气、放置按手持物品映射、碰撞检测拒绝并回滚 |
| 窗口同步 | 登入 WindowItems、创造改物品、ClickWindow 事务回执 + 权威重同步 |
| `item/crafting` | **数据驱动配方**（shaped/shapeless），全部来自 `config/recipes.toml` |
| `net/furnace_store` | 线程安全熔炉存储：点火/燃烧/冶炼/产出/阻塞全状态机，shift-click 转入 |
| `net/container_store` | 箱子容器持久化 |
| `net/crafting_table_store` | 工作台 3×3 格与结果持久化 |
| `net/mob_manager` | 生物登记表 + 漫游 AI（停留↔行走状态机、边界回拉） |
| `game/op_manager` | OP 权限管理器，`config/ops.json` 持久化 |
| 命令系统 | `/gamemode /tp /kill /say /op /deop /tps /help`，玩家与控制台共享 |
| Tab 补全 | serverbound TabComplete（text + assumeCommand + lookedAtBlock） |
| 测试 | 98 个用例全绿 |

**验证结论**：熔炉合成全链路（煤炭+铁矿 → 铁锭）、容器 shift-click 转移、工作台 3×3、命令与 Tab 补全均经真实 1.12.2 客户端验证。

### M4 交付（第一部分）：存档与世界兼容

| 模块 | 内容 |
|---|---|
| `game/player_data` | 按 UUID 落盘玩家位置/朝向/游戏模式/血量/46 格背包（`world/playerdata/<uuid>.dat`，**原版 gzip NBT 格式**），登录恢复、断开保存；遗留 JSON 自动迁移 |
| `proto/frame` | `inflate_dynamic`：输出未知大小的流式解压（zlib/gzip，上限防压缩炸弹） |
| `world/nbt` | Anvil NBT 读写器（大端、命名标签、13 类标签、保序 compound） |
| `world/region` | `.mca` 读写重写：扇区分配/复用、位置表+时间戳、tmp+rename 原子落盘；修除原悬垂指针缺陷 |
| `world/anvil` | 区块 ↔ 1.12.2 NBT（Blocks/Data/Add per-section 基线填充 + TileEntities：箱子 Items、熔炉 Items/BurnTime/CookTime/CookTimeTotal） |
| `game/world_persistence` | 编排：编辑区块 ∪ 实体区块 → region；载入合并编辑并恢复箱子/熔炉存储 |
| Server 接线 | 启动载入、停机保存、`server.autosave_interval` 自动保存、控制台 `save` 命令 |
| `proto/deflate_gzip` | gzip 容器写出（zlib deflateInit2 15+16）；`nbt::parse_compressed` 嗅探 gzip/zlib/raw |
| 测试 | +4 用例（预言机 fixture 解析、.dat 往返与槽位映射、读原版文件、遗留 JSON 迁移），109 全绿 |

**验证结论**：真实客户端挖方块 → SIGTERM 停机落盘 `world/region/r.0.0.mca` → 重启日志 `loaded 1 chunks` → 再次停机 `saved 1 chunks`；区块级还原由 `persistence_world_round_trip` 覆盖（跨 region 负坐标、baseline 剔除、箱子/熔炉内容与进度）。

**玩家 .dat 互操作**（R-010 预言机实测）：我们的服务器写的 `.dat` 由原版 1.12.2 服务器加载——玩家按我们保存的游戏模式（创造，server.properties 默认为生存，故该字段必出自我们的文件）与坐标 (0.5, 4, 0.5) 进入世界；反向由 fixture `tests/fixtures/vanilla_player_oracle.dat` 锁定。

| 模块 | 内容 |
|---|---|
| `world/anvil` | 掉落物实体 ↔ region `Entities` 列表（`id=minecraft:item`，`Pos` List<f64>×3 + `Item{id,Count,Damage}`——实体字段名与方块实体不同，见 R-012）；`decode_chunk` 还原；`encode_chunk` 写出；`encode_chunk_merged` 保留非物品实体（生物等）原样透传、重写物品实体为内存态 |
| `net/item_drop` | `ItemDropManager` 增加 `restore(span<DroppedItemState>)` / `all_drops()`：存档载入回填、保存取全量（`DroppedItemState` 仅位置+堆叠，去实体 id/出生时刻） |
| `game/world_persistence` | 载入时把各区块掉落物回填 `ItemDropManager`；保存时按 `floor` 坐标把 `all_drops()` 归入所在区块的 `Entities` |
| `world/level_dat` | `ensure_level_dat(dir, game_type)`：首启缺失时写最小集（`Time/Version{Id=1101,Name=1.12.2}/GameType/generatorName=flat/Spawn*`，gzip NBT）；已存在不覆盖 |
| 测试 | +2 用例（掉落物往返 + merged 保留生物实体；level.dat 首启建档），117 全绿 |

**验证结论**：cyane 载入含掉落物的区块后重新保存，物品实体按原版 `id`+`Pos` 格式写回 region `Entities`，非物品实体（生物等）经无损保存原样透传；首启无 level.dat 时建档最小集供原版读取出生点。真机预言机：cyane 载入原版世界（633 区块，83 生物 + 6 掉落物实体）→ 保存 → 原版重载 `Done (4.317s)` 零错误，掉落物字段往返完整（R-012）。

### M4 交付（第二部分）：完整区块模型与原版世界加载

| 模块 | 内容 |
|---|---|
| `world/world` | 重构为**完整区块存储**：每区块持全量 section，未物化区块回退超平坦 baseline；脏标记驱动落盘、干净区块按视距释放 |
| `world/region` | 修正区块记录格式为原版布局（4 字节大端长度含压缩字节 + 1 字节压缩类型）；支持 gzip/zlib/未压缩三种类型 |
| `world/anvil` | 编解码改为 Chunk 级（去 baseline 过滤）；新增 `encode_chunk_merged` **无损保存**：以磁盘原始 NBT 为底仅替换 Blocks/Data/Add 与箱子/熔炉，光照/生物群系/HeightMap/实体/未建模方块实体原样透传 |
| `world/chunk` | 修复 `ChunkPos::from_world` 地板除（此前 x≥0、z<0 象限 z 轴偏移一个区块） |
| `world/level_dat` | 读取 `<world>/level.dat` 的 SpawnX/Y/Z，接入出生点 |
| 测试 | +6 用例（完整/无损往返、原版区块记录 fixture、level.dat oracle、无损字段保留），115 全绿 |

**验证结论**：cyane 加载原版 1.12.2 服务器生成的真实世界（`loaded 1576 chunks`，出生点取自 level.dat `(247,4,1091)`）；改块保存后由原版服务器重新加载**零区块错误**；被重写的区块保留 SkyLight/BlockLight/HeightMap/Biomes。

## 性能目标（M8 验收基线）

| 场景 | 目标 |
|---|---|
| 空载 TPS | 20.0（tick p99 < 35ms） |
| 1000 玩家移动广播 | < 5ms/tick |
| 区块生成 | ≥ 500 chunks/s（4 核） |
| 1000 加载区块内存 | < 2GB |
| 启动 | < 2s（不含 JVM 初始化） |
| 事件 dispatch（无监听器） | 0（短路，不进 JNI） |
| 典型 Play 包编解码 | < 500ns |

本机 4 核 / 7GB 只能覆盖到百人级与单机基准；1000 玩家指标需另找机器。

## 风险与对策

| 风险 | 影响 | 对策 |
|---|---|---|
| NMS 需求面未知 | 做了没人用的符号 | M7 起用扫描器收集真实需求，T1 按频次数据驱动 |
| 混淆/MCP 混名写错 | 插件链接失败 | 签名清单从 jar 机械提取，禁手写 |
| 镜像字段与内核状态不一致 | 幽灵 bug | 单一权威 + 固定同步点；debug 构建校验双向一致 |
| 第三方类路径缺失 | 大量插件 `NoClassDefFoundError` | 类路径契约表 + 扫描器检测 import 并预检 |
| Bukkit API 表面积大 | 兼容永远"差一点" | 按真实插件调用频率分级，apidiff 机械保证已实现部分正确 |
| 世界生成与原生不一致 | 地形预期不符 | 文档声明；优先保证 Anvil 读写正确 |
| JNI 成瓶颈 | 插件多时 TPS 崩 | 事件短路、对象池、批量 upcall |
| 参考代码污染实现 | 法律风险 | 评审清单 + `reference-log.md` 审计 |
| 7GB 内存/4 核 | 无法本机验证规模指标 | 规模测试外借机器 |