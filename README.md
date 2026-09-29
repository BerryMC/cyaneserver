# CyaneServer

纯 C++23 实现的 Minecraft Java Edition **1.12.2**（协议 340）服务端。

> 详细文档见 `docs/`：架构、兼容契约、里程碑、协议、参考日志。

## 快速开始

```bash
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
./build/cyane
```

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
├── java/             # bukkit-api / nms-shim / craft-shim
├── jars/             # 参考 jar（不入库）
├── tools/            # 探针、签名提取、插件扫描器、基准
├── tests/            # 单元测试
└── docs/             # 架构、兼容、里程碑、协议、参考日志
```

## 技术栈

C++23（clang）· CMake 4.4 · lld · zlib-ng · OpenSSL · liburing · 内嵌 JVM（8/11/17/21）

## 里程碑

| 阶段 | 内容 | 状态 |
|---|---|---|
| M0 | 工程骨架 | ✅ |
| M1 | 协议与连接（握手/状态/登录/加密） | ✅ |
| M2 | 世界与移动（区块/移动同步/多人可见/聊天） | ✅ |
| M3 | 玩法基础（方块/物品栏/容器/合成/熔炉/命令/生物） | ✅ |
| M4 | 存档与世界兼容（玩家 .dat 互通 ✅ / Anvil 编辑区块 ✅ / 完整区块模型） | 🟡 |
| M5 | 世界生成（噪声地形/生物群系/洞穴） | ⬜ |
| M6 | 玩法进阶（敌对生物/物理/经验附魔） | ⬜ |
| M7 | 插件基座（JVM/类加载器/事件/调度） | ⬜ |
| M8 | Bukkit API 覆盖扩展 | ⬜ |
| M9 | NMS shim | ⬜ |
| M10 | 性能与加固 | ⬜ |

## 当前实现

- **协议与连接**：握手/状态/登录/加密/压缩完整流程，与原版逐字节一致。
- **世界与移动**：超平坦区块、动态加载/卸载、多人可见、聊天、KeepAlive。
- **Anvil 存档**：完整区块存储，可**加载原版世界**（真实地形 + `level.dat` 出生点），保存为无损打补丁（光照/群系/实体透传）；方块修改与箱子/熔炉内容跨重启保留；停机保存 + 自动保存 + 控制台 `save`。
- **方块交互**：破坏/放置/碰撞检测/回滚，创造/生存模式。
- **容器**：箱子、工作台、熔炉（含持久化存储与 shift-click 转移）。
- **物品栏**：46 槽背包、点击事务、创造物品修改。
- **玩家持久化**：按 UUID 落盘位置/朝向/游戏模式/血量/46 格背包（`world/playerdata/<uuid>.dat`，**原版 gzip NBT 格式**，与原版服务器双向互通），登录恢复、断开保存。
- **命令**：`/gamemode /tp /kill /say /op /deop /tps /help`，OP 权限，Tab 补全。
- **合成**：数据驱动配方（shaped/shapeless），结果槽实时刷新。
- **生物**：被动生物生成与漫游 AI。

## 文档索引

| 文档 | 内容 |
|---|---|
| `docs/architecture.md` | 进程/线程、分层、各子系统设计 |
| `docs/compat.md` | Spigot 兼容契约、NMS 分层、第三方类路径 |
| `docs/roadmap.md` | 里程碑交付清单、当前状态、性能目标、风险 |
| `docs/protocol.md` | 协议 340 事实与验证状态 |
| `docs/reference-log.md` | 参考源审计日志 |
