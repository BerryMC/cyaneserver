# CyaneServer

纯 C++23 实现的 Minecraft Java Edition **1.12.2**（协议 340）服务端。

> 详细文档见 `docs/`：架构、兼容契约、里程碑、协议、参考日志。
> 原版对齐进度：`vanilla/` 2185 文件燃尽清单，已核销 310（14.2%），`tools/vanilla_progress.py` 输出。

## 快速开始

```bash
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
./build/cyane
```

单元测试（207 项）：`cmake --build build -j2 && ./build/cyane_tests`

## 目标

| 项 | 指标 |
|---|---|
| 协议 | Java Edition 1.12.2，protocol 340 |
| 存档 | 读写原生 Anvil (`.mca`) |
| 性能 | 单 tick 内 1000 并发玩家 |
| 部署 | 单可执行文件 + `plugins/` + `world/` |

## 参考与使用的边界

- **可以**：反编译阅读原版/Spigot，理解语义；把 jar 当测试基准与行为预言机。
- **不可以**：复制其代码进本项目；链接其 class/jar；随包分发其产物。

## 目录

```
cyane/
├── include/cyane/    # 内核头文件
├── src/
│   ├── core/         # 日志、配置、线程池、错误、时间
│   ├── net/          # reactor、连接、加密、压缩、限流
│   ├── proto/        # 340 包定义、读写器、NBT
│   ├── world/        # 注册表、chunk、光照、anvil、voxelshape
│   ├── item/         # 物品、附魔、容器、合成、熔炉
│   ├── game/         # tick、调度、命令、权限、事件总线
│   └── plugin/       # JVM 宿主、JNI 桥、插件加载
├── config/           # server.toml / recipes.toml
├── data/             # 数据驱动内容（registry 等）
├── world/            # 运行时存档（level.dat / region / playerdata）
├── vanilla/          # 原版 1.12.2 燃尽占位文件（2185，逐份实现后 git rm）
├── java/             # bukkit-api / nms-shim / craft-shim
├── jars/             # 参考 jar（不入库）
├── tools/            # 探针、签名提取、燃尽统计、基准
├── tests/            # 单元测试
└── docs/             # 架构、兼容、里程碑、协议、参考日志
```

## 技术栈

C++23（clang）· CMake 3.25+ · lld · zlib-ng · OpenSSL · liburing · 内嵌 JVM（8/11/17/21）

## 里程碑

| 阶段 | 内容 | 状态 |
|---|---|---|
| M0 | 工程骨架 | ✅ |
| M1 | 协议与连接（握手/状态/登录/加密） | ✅ |
| M2 | 世界与移动（区块/移动同步/多人可见/聊天） | ✅ |
| M3 | 玩法基础（方块/物品栏/容器/合成/熔炉/命令/生物） | ✅ |
| M4 | 存档与世界兼容（玩家 .dat 互通 / Anvil 编辑区块 / 完整区块模型） | ✅ |
| M5 | 世界生成（噪声地形/生物群系/洞穴/矿脉/植被装饰/结构） | ✅ 核心 |
| M6 | 玩法进阶（敌对生物 AI/寻路/战斗/经验/掉落） | ✅ 核心 |
| M7 | 插件基座（JVM/类加载器/事件/调度） | ⬜ |
| M8 | Bukkit API 覆盖扩展 | ⬜ |
| M9 | NMS shim | ⬜ |
| M10 | 性能与加固 | ⬜ |

## 当前实现

- **协议与连接**：握手/状态/登录/加密/压缩完整流程，与原版逐字节一致。
- **世界与移动**：自然噪声地形区块、动态加载/卸载、多人可见、聊天、KeepAlive。
- **Anvil 存档**：完整区块存储，可**加载原版世界**（真实地形 + `level.dat` 出生点），保存为无损打补丁（光照/群系/实体透传）；方块修改与箱子/熔炉内容跨重启保留；停机保存 + 自动保存 + 控制台 `save`。
- **存档互操作（Spigot 双向实测）**：
  - `session.lock` 用 Java `writeLong` 的大端 8 字节；被其他进程接管后拒绝写盘（`The save is being accessed from another location, aborting`），关服不删除锁文件。
  - `level.dat` 走原版 `level.dat_new → level.dat_old → level.dat` 轮换，读取失败回退 `level.dat_old`，每次保存强制 `saveVersion=19133`。
  - `WorldInfo` 逐字段对齐原版：`Version/DataVersion/RandomSeed/generatorName/generatorVersion/MapFeatures/GameType/Difficulty/hardcore/allowCommands/initialized/Time/DayTime/SizeOnDisk/LastPlayed/天气/GameRules/Player/DimensionData`，以及位于 **Data 根**的 `BorderCenterX/BorderCenterZ/BorderSize/BorderSizeLerpTime/BorderSizeLerpTarget/BorderSafeZone/BorderDamagePerBlock/BorderWarningBlocks/BorderWarningTime`。
  - 保存只覆盖已建模字段，Bukkit/Spigot/Forge 追加的未知键原样保留；`SaveFormatOld` 提供 `World1..World5` 枚举、`renameWorld`（只改 `LevelName`）、`deleteWorldDirectory`（5 次重试）等存档目录管理。
- **自然地形生成（M5 核心）**：逐行转写原版 `ChunkGeneratorOverworld`——
  - 5×5×33 噪声高度图（depth/main/minLimit/maxLimit 四组八度 Perlin + 群系 5×5 权重卷积）→ 三线性插值填石头/水；
  - **30 种原版生物群系**（大陆度宏观分带 + 温湿度气候区，陆海间沙滩缓冲，冷/温带/热区不交错）；
  - 洞穴雕刻（`MapGenCaves`）、8 类矿脉（煤/铁/金/红石/钻石/青金石/绿宝石 + 花岗岩/闪长岩/安山岩）、地表替换与基岩；
  - 装饰植被：橡树/白桦/云杉/金合欢/沼泽树/深色橡树、高草、花朵、甘蔗、仙人掌、野生南瓜、睡莲、枯灌木、双层植物、水底黏土与沙盘；
  - 湖与熔岩湖（原版空腔密封校验，不撕裂海岸）、岩壁水/熔岩泉眼、沙漠水井、地下地牢（苔石室 + 刷怪笼 + 宝箱）、寒带自然覆雪结冰。
- **种子系统**：`config/server.toml` 的 `seed`（空=随机、数字=直取、文本=Java `String.hashCode`），持久化到 `level.dat` 的 `RandomSeed`，`/seed` 动态读取。
- **旁观者模式与动作同步**：`PlayerInfo` 动作序号对齐原版枚举（更新游戏模式=action 1，含广播给自己）；旁观者无敌/禁交互，切换时对他人销毁/重建自身实体；潜行/疾跑经 `EntityMetadata`（index 0，serializer 1）广播；破坏方块发射 `WorldEvent 2001` 粒子，挖掘裂纹动画按原版计时广播 `BlockBreakAnimation`。
- **方块交互**：破坏/放置/碰撞检测/回滚，创造/生存模式。
- **容器**：箱子、工作台、熔炉（含持久化存储与 shift-click 转移）。
- **物品栏**：46 槽背包、点击事务、创造物品修改。
- **玩家持久化**：按 UUID 落盘位置/朝向/游戏模式/血量/46 格背包（`world/playerdata/<uuid>.dat`，**原版 gzip NBT 格式**，与原版服务器双向互通），登录恢复、断开保存。
- **命令**：`/gamemode /tp /kill /say /op /deop /seed /tps /help`，OP 权限，Tab 补全。
- **合成**：数据驱动配方（shaped/shapeless），结果槽实时刷新。
- **生物（M6 核心）**：僵尸近战、苦力怕引信自爆、骷髅举弓走位射箭、动物繁殖/剪毛/挤奶；原版 A* 寻路（`WalkNodeProcessor`）、无敌窗与护甲减伤、经验球与升级公式、死亡掉落。

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCYANE_LTO=OFF
cmake --build build -j2
./build/cyane_tests
```

构建速度默认已开三样（都能用选项关掉）：

| 机制 | 说明 | 关闭方式 |
|---|---|---|
| ccache | 编译结果缓存，切换分支/重复编译几乎全命中 | `CYANE_NO_CCACHE=1` 环境变量 |
| lld | 链接器，比 BFD ld 快数倍 | `-DCYANE_FAST_LINK=OFF` |
| `-g1` | 只保留行号调试信息，编译与链接都更快 | `-DCYANE_FULL_DEBUG=ON` |

另外建议装 `ninja` 后改用 `-G Ninja`，比 Make 并行调度更高效。

**发布构建**：`-DCMAKE_BUILD_TYPE=Release -DCYANE_LTO=ON`。LTO 在开发期关闭，
因为它会显著拖慢每次链接。

## 首次启动

空目录直接运行即可，缺失的配置会自动生成，不需要手工准备：

| 文件 | 内容 |
|---|---|
| `config/server.toml` | 服务端配置（内置默认值，带注释） |
| `config/recipes.toml` | 合成配方表（构建期从仓库那份嵌入，单一来源） |
| `config/ops.json` | OP 列表（空数组） |
| `world/` | `level.dat`、`session.lock` 等，首次启动按种子生成 |

已存在的文件**绝不会被覆盖**；写入先落 `.tmp` 再改名，中途失败不会留下半个配置。

## 原版燃尽跟踪

`vanilla/` 下按原版 1.12.2 源码目录保存 2185 个零字节占位文件，**每个机制先实现 C++ 等价物、加单元测试，再 `git rm` 核销对应占位文件**。

```bash
python3 tools/vanilla_progress.py          # 人读报告
python3 tools/vanilla_progress.py --json   # 机器可读
python3 tools/vanilla_progress.py --strict # 有 P0 待修时退出码 1（CI 用）
```

工具给出三个口径：

1. **服务端源码燃尽** —— 分母 1520（2185 中剔除 `client` 643 与 `realms` 22，这两个是纯客户端包，服务端永远不实现，计入会让进度卡在 15%）。已完成 310（20.4%）。
2. **缺口清单燃尽** —— 解析 [`docs/vanilla-gaps.md`](docs/vanilla-gaps.md)，给出 P0/P1/P2 的待修、已修、阻塞条目，**这是"还差什么"的权威数字**。
3. **分模块燃尽** —— 按顶层包给出 已实现/总数 与进度条。

基线由 [`tools/vanilla_baseline.txt`](tools/vanilla_baseline.txt)（2185 条，由 git 历史里出现过、现已删除的文件并上现存文件生成）提供；文件缺失时回退到用 git 现算。

> ⚠️ 文件计数口径有局限：cyane 的生物/方块/物品是数据驱动实现（`mob_types`/`blocks` 表），`entity`/`block`/`item` 等包的按文件计数会显著低估实际覆盖度。**服务端进度以缺口清单为准**，文件燃尽只反映结构对齐程度。

## 文档索引

| 文档 | 内容 |
|---|---|
| `docs/architecture.md` | 进程/线程、分层、各子系统设计 |
| `docs/compat.md` | Spigot 兼容契约、NMS 分层、第三方类路径 |
| `docs/roadmap.md` | 里程碑交付清单、当前状态、性能目标、风险 |
| `docs/protocol.md` | 协议 340 事实与验证状态 |
| `docs/reference-log.md` | 参考源审计日志 |
| `docs/vanilla-gaps.md` | **原版一致性缺口清单（待修复列表，按 P0/P1/P2 分级）** |
