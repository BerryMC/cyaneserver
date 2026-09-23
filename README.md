# CyaneServer

纯 C++23 实现的 Minecraft Java Edition **1.12.2**（协议版本 340）服务端，目标是运行 **Spigot/Bukkit 插件**。

原版/Spigot 代码仅作为**参考与测试基准**，其代码与产物不进入本项目源码、不参与链接、不随包分发。

---

## 1. 目标

| 项 | 指标 |
|---|---|
| 协议 | Java Edition 1.12.2，protocol 340，原版客户端直连可玩 |
| 插件 | Bukkit API 插件 + 常用 NMS 插件（分层支持，见 §5） |
| 存档 | 读写原生 Anvil (`.mca`)，可直接加载现有世界 |
| 性能 | 单 tick 预算内 1000 并发玩家；见 §11 |
| 部署 | 单可执行文件 + `plugins/` + `world/`，内嵌 JVM |

---

## 2. 参考与使用的边界

**可以**：反编译并阅读原版/Spigot 代码，理解协议布局、方块语义、实体元数据索引、光照与红石规则、NMS 签名与行为；把 jar 当作测试基准与行为预言机（oracle）跑起来对照。

**不可以**：把其代码复制进本项目（含改写后的近似复制）；链接其 class/jar；随包分发其任何产物；依赖其运行期存在。

工程约束：

1. 阅读参考代码是为了**理解语义**，落地的实现必须自写。评审时以"这段代码能否解释清为什么这样做"为准。
2. 协议事实（包 ID、字段布局）、API 签名（类名/方法名/描述符）属于互操作性所需的事实，可机械提取；**实现体**必须原创。
3. `jars/` **不入库**（`.gitignore`），仅本机参考。派生出的 Spigot jar 不得分发。
4. `docs/reference-log.md` 记录：读了哪些类、得出什么结论、落地到哪个文件。可审计。

---

## 3. 架构

### 3.1 进程与线程

单进程：C++ 内核 + 内嵌 JVM（JNI Invocation API）。

```
        ┌─────────────── io_uring / epoll (SO_REUSEPORT) ───────────────┐
        │  Net-0        Net-1        Net-2        Net-N   (SPSC 无锁环) │
        └──────┬───────────┬───────────┬───────────┬───────────────────┘
               └───────────┴─────┬─────┴───────────┘
                                 ▼
                        ┌─────────────────┐
                        │  Tick 线程 (20Hz)│◄──── JNI ────┐
                        │  世界/实体/玩家   │              │
                        └────────┬────────┘         ┌─────┴──────┐
                                 │ 异步任务         │  JVM       │
                        ┌────────▼────────┐         │ 插件 JAR   │
                        │ Chunk Worker ×M │         │ Bukkit API │
                        │ 生成/序列化/光照 │         │ NMS shim   │
                        └─────────────────┘         └─────┬──────┘
                                                  插件 async 线程池
```

- **Bukkit 语义优先**：`sync` 事件与调度任务只在 Tick 线程执行；跨线程仅限文档声明异步的入口。
- Net 线程与 Tick 线程之间用每连接 SPSC 无锁队列。Chunk Worker 无共享可变状态。
- `JNI_CreateJavaVM` 在主线程调用，主线程兼任 Tick 线程。

### 3.2 分层

```
插件 JAR (Java 8 字节码)
   │  ← 每插件独立 URLClassLoader（parent = API loader）
   ├── org.bukkit.*                        重新实现（Java，825 类目标面）
   └── net.minecraft.server.v1_12_R1.*     NMS shim（Java 镜像 + JNI 委托）
   │  ← 缓存的 jmethodID/jfieldID，事件零分配传参，无监听器短路
JNI 桥（C++）：句柄表、对象池、批量同步、upcall 批处理
   │
C++23 内核：net / proto / world / entity / game
```

---

## 4. 子系统

### 4.1 网络层 `src/net/`

- `epoll(ET)` 与 `io_uring` 双后端，抽象为 concept，编译期可选、运行期可切。
- 每连接读环 + VarInt 帧头增量解析（容忍半包/粘包，超长帧拒绝）。
- 压缩 `zlib-ng`（阈值以下直发，默认等级 4）；加密 `AES-128-CFB8`（OpenSSL EVP，每连接独立上下文）。
- 写路径：组包进发送环 → 每 tick 或达水位 `writev` 批量刷出；高水位背压暂停读。强制 `TCP_NODELAY`。
- 兼容 BungeeCord/Velocity IP 转发与 PROXY protocol v1/v2。

### 4.2 协议层 `src/proto/`

- 手写编解码，包描述用 `consteval` 表在编译期生成读写器，运行期无反射无查表开销。
- 1.12.2 要点：`Chunk Data` section = bits-per-block(4..8) + palette + 4096 longs + blocklight 2048 + skylight 2048 + biome 256；实体元数据 `0x3E` 索引/类型严格对齐；**网络 NBT 与 Anvil NBT 是两套编码**（字符串长度、TAG_List 元素类型），writer 分离。
- 包 ID 与字段布局以抓包对照校验（原版服务端 + 原版客户端均在手，见 §6）。

### 4.3 世界层 `src/world/`

- 方块状态注册表：1.12.2 全局 state id `0..8581`（含 4096 元数据位），`constexpr` 表 + O(1) 双向映射；`assets/minecraft/` 下的数据资产作为语义参考。
- Chunk 生命周期 `Proto → Generated → Populated → Lit → Loaded → Active`，显式状态机，非法迁移 debug 断言。
- 光照引擎：blocklight/skylight 独立 nibble 数组，BFS 队列 + 分批增量传播 + SIMD 批处理；禁止每 tick 全量重算。
- 存储：`mmap` region + 零拷贝 NBT 解析；chunk 懒加载/按距离卸载，脏页回写。
- 碰撞：voxel shape 索引 + AABB 扫掠。

### 4.4 实体与玩家 `src/entity/`

- 组件化实体，SoA 布局；AABB + 扫掠碰撞，空间哈希分区。
- AI：`Goal` 优先级 + 互斥掩码（非行为树）。
- 玩家：库存（窗口同步 ID）、经验、附魔、药水、成就、Tab、记分板。

### 4.5 游戏层 `src/game/`

- 固定 20Hz tick，超时补偿与 TPS 统计，每 tick 分区计时（网络/实体/方块/光照/插件）。
- 调度器 `sync/async/delayed/repeating`，task id 与 `cancel()` 语义与 Bukkit 一致。
- 命令 + Tab 补全（1.12.2 用旧协议 `0x0B`，非 Brigadier）。
- 事件总线按类型编译期索引，无监听器时 dispatch 短路为 0 开销。

### 4.6 插件层 `src/plugin/` + `java/`

- **嵌入**：`JNI_CreateJavaVM`，`-Xmx` 可配；JVM 版本探测（8/11/17/21），JVM 21 下自动补 `--add-opens`。
- **类路径**：必须复现 Spigot 的暴露面，否则插件 `NoClassDefFoundError`（清单见 §5.3）。
- **加载**：每插件独立 `URLClassLoader`；自研 mini-YAML 解析 `plugin.yml`；依赖排序与 `onLoad/onEnable/onDisable` 生命周期。
- **配置**：`YamlConfiguration` 自研（解析 + 注释保留 + 路径 API）。
- **事件**：C++ 侧持 `(eventType → 有序 handler)`，Java 侧注册时回传 `jmethodID`；事件对象池化避免 GC 抖动；无监听器直接短路。

---

## 5. 兼容契约

Spigot 兼容 = **Bukkit API** + **NMS shim** + **第三方类路径**，三者缺一插件就崩。以下数据由本机 Spigot jar 实测得出。

### 5.1 表面积实测

| 组成 | 类数 | 说明 |
|---|---:|---|
| `org.bukkit.*`（净 API） | **825** | 插件编译链接的接口面，M4/M5 主目标 |
| `org.bukkit.craftbukkit.v1_12_R1.*` | 346 | CraftBukkit 实现类，插件常 `import` 其静态工具 |
| `net.minecraft.server.v1_12_R1.*` | **2324** | NMS 面，见 §5.2 |
| `org.bukkit.craftbukkit.libs.*` | 10873 | shaded 库（fastutil 等），插件可 import |

NMS 类聚类（2324 总计）：`Block*` 304、`Entity*` 256、`World*` 229、`Packet*` 169、`Item*` 125、`TileEntity*` 49、`Player*` 42、`Server*` 23、`NBT*` 20、`Chunk*` 17。

热点类公开成员数（方法+字段）：`Entity` 290、`World` 250、`EntityLiving` 208、`EntityPlayer` 135、`ItemStack` 86、`WorldServer` 64、`PlayerConnection` 48、`NBTTagCompound` 43、`Container` 36。

**两个决定性事实**（实测）：

1. **NMS 命名是 MCP 与混淆的混合体**。类名是 MCP 名（`EntityPlayer`/`WorldServer`/`PacketPlayOutChat` 均在），但成员名只有一部分被 MCP 覆盖：`EntityPlayer` 57 个公开成员是混淆短名（`d`/`e`/`f`），77 个是 MCP 名。
   → **签名清单必须从 jar 机械提取（javap），不能按 MCP 知识手写**，否则插件链接失败。
2. **公开可变字段是 ABI 的一部分**。插件直接写 `entity.locX = ...`、`entity.motY = ...`、`entity.dead = true`，这类访问不走方法调用。
   → shim 必须提供**镜像字段**，不能只做方法委托。

### 5.2 NMS 分层策略

结论：**NMS 可以做，但形态是"镜像字段 + 委托的兼容层"，不是通用转译器**。转译只能改名字、不能造语义。

| 层 | 内容 | 成本 | 覆盖价值 |
|---|---|---|---|
| **T0** | 仅 Bukkit API（825 类） | M4–M5 主线 | 纯 API 插件 |
| **T1** | NMS façade，按需增长 | 中 | 高 |
| **T2** | 深水区，逐案评估 | 高 | 中 |
| **T3** | 不做 | — | — |

**T1（优先做，复用已有模型，风险低）**

- `Packet*`（169 类）：纯字段袋，序列化我们自己写。`((CraftPlayer)p).getHandle().playerConnection.sendPacket(...)` 是最常见的 NMS 用法，价值最高。
- `NBT*`（20 类）：`NBTTagCompound`/`NBTCompressedStreamTools` 等纯数据结构，自包含。
- 聊天组件 `IChatBaseComponent`/`ChatComponentText`/`ChatModifier`/`EnumChatFormat`。
- 枚举 `EnumParticle`/`EnumItemSlot`/`EnumDirection` 等。
- `Craft*` 句柄：`CraftPlayer/CraftWorld/CraftEntity/CraftItemStack.getHandle()` → shim 对象。

**T1 架构：镜像 + 同步点**

```
Java shim 对象（投影，非权威）
  ├─ 镜像字段 locX/locY/locZ/motX/dead…   插件读写 = 纯 Java 字段访问，零 JNI
  ├─ 读方法 getHealth()/getWorld()…       → JNI → C++ 权威表
  └─ 写方法 / sendPacket()                → JNI → C++ 内核
        ▲
        │ 同步点：tick 前 pull、tick 后 push，每实体批量一次
        ▼
C++ 权威状态（SoA）
```

关键点：**C++ 是唯一权威，Java shim 是投影**。字段访问零 JNI 成本，只在 tick 边界做批量同步；shim 对象**惰性创建**，仅对插件真正接触过的实体实例化，用弱引用注册表跟踪以便 GC；实体卸载后句柄失效。

**T2（深水区，逐案评估）**

- 自定义 `ChunkGenerator`（1.12.2 接口较收敛，可能可行）
- `extends` NMS 实体类的自定义实体（需内核支持 Java 驱动的实体类型）
- 封包**拦截**（非发送）：不伪造 `PlayerConnection.a`，改为提供显式 hook API

**T3（不做）**

- 反射进 NMS 私有成员、mixin 类方案
- 整体替换服务器内部（自定义 `MinecraftServer`/`WorldServer` 子类并让内核跑它）

**转译层真正的位置**：不是用来"猜测语义"，而是用来**维持一套 shim 服务多个版本**——在自研 `ClassLoader.findClass` 里用 ASM `ClassReader → ClassRemapper → ClassWriter` 把 `v1_8_R3` 等旧版引用映射到我们唯一的 `v1_12_R1` shim。约 300 行，仅类加载期一次，运行期零开销。我们只做 1.12.2，所以这是后期的可选增益，且旧版语义差异使其只是兼容性红利而非正确性保证。

**配套机制（比 shim 本身更重要）**

- **插件扫描器**（M4 就做，很便宜）：加载期扫常量池，判定插件属于哪个 tier；不支持时给出**缺失符号清单**而不是运行期 `NoClassDefFoundError`。同时它产出真实需求数据——**T1 的覆盖面靠扫描报告数据驱动增长，而不是预先规划**。
- **API 签名校验**（已验证可行）：对着 Spigot jar 编译 + javap 签名 diff，机械保证我们 825 类 API 与 shim 的 ABI 一致。

### 5.3 第三方类路径契约

Spigot 把这些库**未混淆**暴露在类路径上，插件直接 `import`。我们必须提供等价物：

| 插件常见用法 | Spigot 暴露路径 |
|---|---|
| `TextComponent`（悬浮/点击文本，`Player.spigot().sendMessage` 必需） | `net.md_5.bungee.api.chat` |
| YAML | `org.yaml.snakeyaml` |
| JSON / 集合 / 常见工具 | `com.google.gson`、`com.google.common` |
| Netty（自定义 channel） | `io.netty` |
| 字符串/IO 工具 | `org.apache.commons.lang3`、`org.apache.commons.io` |
| 本地数据库 | `org.sqlite`、`com.mysql` |
| 原始集合（MC 内部大量使用） | `gnu.trove`、`org.bukkit.craftbukkit.libs.it.unimi.dsi.fastutil` |
| Spigot 自有 | `org.spigotmc` |

---

## 6. 参考源与工具

### 6.1 参考 jar（`jars/`，不入库）

| 文件 | 来源 | sha256 |
|---|---|---|
| `vanilla/server.jar` | 1.12.2 原版服务端（piston-data `886945bf…`） | `fe1f9274e6dad9191bf6e6e8e36ee6ebc737f373603df0946aafcded0d53167e` |
| `vanilla/client.jar` | 1.12.2 原版客户端 | — |
| `spigot/server.jar` | Paperclip 引导壳（`Main-Class: io.papermc.paperclip.Paperclip`），含 `patch.properties` + `spigotMC.patch` | `38115344` 字节 |
| `spigot/spigot-1.12.2.jar` | **派生**：`spigotMC.patch` 打在 vanilla 上 | `ff5440e15f371b6def688c86bce296f8451fa8d00df9ad4270eb250621a468f2` |

**离线复现 Spigot jar**（已实测通过，约 20s）：

```bash
cd jars
sha256sum vanilla/server.jar                     # 须等于 originalHash
unzip -o spigot/server.jar spigotMC.patch -d spigot/
java -cp spigot/server.jar org.jbsdiff.ui.CLI patch \
     vanilla/server.jar spigot/spigot-1.12.2.jar spigot/spigotMC.patch
sha256sum spigot/spigot-1.12.2.jar               # 须等于 patchedHash
```

`patch.properties` 记录 `version=1.12.2`、`patchedHash`、`originalHash`、`sourceUrl`；哈希不符即拒绝使用，避免拿错版本量出的签名污染 shim。

### 6.2 工具链

| 用途 | 工具 | 状态 |
|---|---|---|
| 签名提取（ABI 契约） | `javap -cp <spigot.jar>` | 已有（JDK 21） |
| 语义参考（阅读逻辑） | Vineflower / CFR | 待取（Maven Central 可达，已验证网络） |
| 协议对照 | 原版服务端 + 客户端抓包 | jar 已就位 |
| 编译校验 | `javac -cp <spigot.jar>` | **已验证**：含 NMS 的探针类编译通过 |
| ABI diff | 自研 `tools/apidiff`（javap → 结构化清单对比） | M4 交付 |

### 6.3 环境

clang 22.1.8、cmake 4.4.3、make、lld、JNI 头（GraalVM 21）、zlib-ng 2.3.3、libdeflate 1.26、OpenSSL 3.6.4、liburing 2.15、zstd 1.5.7、xxhash。
缺：ninja（用 Makefile 生成器）、mold（用 lld）、反编译器（待取）。
硬件：4 核 / 7GB 内存（可用约 3GB）/ 36GB 空闲——**1000 玩家压测不在此机进行**，见 §11。

---

## 7. 目录结构

```
cyaneserver/
├── CMakeLists.txt
├── cmake/
├── include/cyane/
├── src/
│   ├── core/     # 内存/arena、日志、配置、线程池、时间、错误
│   ├── net/      # reactor(epoll|io_uring)、连接、加密、压缩、限流
│   ├── proto/    # 340 包定义、读写器、NBT（网络/磁盘双编码）
│   ├── world/    # 注册表、chunk、palette、光照、anvil、voxelshape
│   ├── gen/      # 地形/生物群系/结构生成
│   ├── entity/   # 实体、玩家、物理、AI、元数据
│   ├── item/     # 物品、附魔、容器、合成、熔炉
│   ├── game/     # tick、调度、命令、权限、事件总线、记分板
│   └── plugin/   # JVM 宿主、JNI 桥、插件加载、plugin.yml、扫描器
├── java/
│   ├── bukkit-api/     # org.bukkit.* 接口与实现（825 类目标面）
│   ├── nms-shim/       # net.minecraft.server.v1_12_R1.* 镜像层
│   ├── craft-shim/     # org.bukkit.craftbukkit.v1_12_R1.* 句柄
│   └── build.sh        # javac + jar
├── jars/               # 参考 jar（gitignore，本机专用）
├── data/               # 生成的注册表、nms-shim 签名清单、SOURCES.md
├── tools/
│   ├── nms_manifest/   # javap → 签名清单 → 生成 shim 骨架
│   ├── apidiff/        # ABI 签名校验
│   ├── scanner/        # 插件常量池扫描（tier 判定 + 需求统计）
│   ├── gen_registry/   # 方块/物品/实体注册表生成
│   └── bench/          # 假人压测
├── tests/
└── docs/               # reference-log.md、protocol.md、plugin-compat.md、benchmarks.md
```

**签名清单驱动**：`data/nms-shim-v1_12_R1.toml` 由 `tools/nms_manifest` 从 Spigot jar 机械生成，声明我们**实际支持**的类与成员（含混淆短名）。Java 骨架与 C++ 绑定表均由它生成，做到可审计、可重生成、可数据驱动增长——手写必然写错混淆名。

---

## 8. 技术栈与构建

| 项 | 选择 |
|---|---|
| 语言 | C++23（clang，`-std=c++23`） |
| 编译器/链接 | clang + lld |
| 构建 | CMake 4.4 + Makefile 生成器 |
| 优化 | `-O3 -flto=thin`，内核热路径 `-fno-exceptions`（边界层用异常，热路径 `std::expected`） |
| 压缩 / 加密 | zlib-ng / OpenSSL EVP |
| IO | io_uring / epoll |
| JVM | 内嵌，探测 8/11/17/21（插件字节码目标 Java 8） |

```bash
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
./build/cyane --config server.toml
```

---

## 9. 开发约定

遵循 AGENTS.md：

1. **性能优先**：热路径不分配、不加锁、不抛异常；性能改动需有基准数据（记入 `docs/benchmarks.md`）。
2. **避免不必要的注释**：只注释代码无法自证的信息（协议魔数出处、非显然约束、SIMD 掩码含义）。禁止"这行做了什么"式注释。
3. **用新标准**：`std::expected`、concepts、ranges、`std::span`、`string_view`、`consteval`、`mdspan`；禁裸 `new/delete`、C 数组、`printf`。
4. **clang 编译**：唯一编译器，`-Wall -Wextra -Wpedantic`，debug 构建 `-Werror`。
5. 命名：类型 `PascalCase`，函数/变量 `snake_case`，成员不加前缀，常量 `kPascalCase`；头文件 `#pragma once`。
6. 错误处理：内核返回 `std::expected<T, Error>`，仅 JVM/IO 边界用异常。
7. 测试：新子系统带单元测试，协议包需编解码往返测试，API/shim 变更需 `tools/apidiff` 通过。

---

## 10. 里程碑

### M0 — 工程骨架
CMake/构建/CI、日志、配置、线程池、错误类型、测试框架、`data/` 生成管线、`jars/` 复现脚本、`.gitignore`、`docs/reference-log.md` 起头。
**验收**：一条命令出可执行文件；空 tick 循环与单元测试通过；`jars/` 复现脚本产出哈希正确的 Spigot jar。

### M1 — 协议与连接
Handshake/Status/Ping、Login（加密+压缩）、KeepAlive、Disconnect 全流程；reactor 双后端；抓包对照工具。
**验收**：原版 1.12.2 客户端连上看到 MOTD/玩家数，进入登录流程后正常断开。

### M2 — 世界与移动
Anvil 读取、Chunk Data 发送、玩家实体、移动同步、聊天、Tab、区块动态加载。
**验收**：客户端进世界自由移动，看到地形与他人聊天；100 假人稳 20 TPS。

### M3 — 玩法基础
方块破坏/放置、物品栏与窗口同步、容器、合成、熔炉、掉落物、生物生成与基础 AI、伤害与重生。
**验收**：正常生存游玩 30 分钟无致命 bug，物品栏持久化。

### M4 — 插件基座
JVM 嵌入、`cyane-bukkit.jar` 核心子集、第三方类路径（§5.3）、每插件类加载器、事件总线、调度器、命令、权限、`plugin.yml`、配置；**插件扫描器**与 **`tools/apidiff`**。
**验收**：自写 Hello 插件（`PlayerJoinEvent` + `/hello` + 周期任务 + `config.yml`）零改动运行；无监听器时事件开销 ≈ 0；apidiff 对 T0 类全绿；扫描器能对任意插件给出 tier 判定与缺失符号清单。

### M5 — Bukkit API 覆盖扩展
补齐 `Inventory/ItemMeta/Enchantment/PotionEffect/Scoreboard/BossBar/Title/BlockData/Metadata/`WorldEdit 所需 API`。
**验收**：20 个纯 Bukkit 真实插件跑通清单记入 `docs/plugin-compat.md`。

### M6 — NMS shim（T1 → 部分 T2）
按扫描器实测的需求频次排序实现：`Packet*` → `NBT*` → 聊天组件 → 枚举 → `Craft*` 句柄 + 镜像字段同步。`tools/nms_manifest` 生成骨架。
**验收**：shim 覆盖扫描器统计的 top-N 高频符号；取真实 NMS 插件（发标题/粒子/自定义包）实测通过；镜像字段同步在基准中不可测（< 噪声）；T2 项目录出可行/不可行结论。

### M7 — 世界生成与规模
噪声地形 + 生物群系 + 洞穴 + 基础结构；光照引擎优化；实体 AI 完善；序列化零拷贝化。
**验收**：新世界 500+ chunks/s（4 核），1000 加载区块内存 < 2GB。

### M8 — 性能与加固
压测 1000 玩家（需借机器）、内存/GC 调优、崩溃恢复、安全（握手限流、封包校验、压缩炸弹防护）。
**验收**：§11 指标达标，`docs/benchmarks.md` 有可复现数据。

---

## 11. 性能目标（M8 验收基线）

| 场景 | 目标 |
|---|---|
| 空载 TPS | 20.0（tick p99 < 35ms） |
| 1000 玩家移动广播 | < 5ms/tick |
| 区块生成 | ≥ 500 chunks/s（4 核） |
| 1000 加载区块内存 | < 2GB |
| 启动 | < 2s（不含 JVM 初始化） |
| 事件 dispatch（无监听器） | 0（短路，不进 JNI） |
| 事件 dispatch（1 监听器） | < 100ns |
| 典型 Play 包编解码 | < 500ns |
| NMS 镜像字段同步 | < 噪声（每实体每 tick 单次批量） |

本机 4 核 / 7GB 只能覆盖到百人级与单机基准；1000 玩家指标需另找机器，不达标不写进文档。

---

## 12. 风险与对策

| 风险 | 影响 | 对策 |
|---|---|---|
| NMS 需求面未知 | 做了没人用的符号 | M4 起用扫描器收集真实需求，T1 按频次数据驱动 |
| 混淆/MCP 混名写错 | 插件链接失败 | 签名清单从 jar 机械提取，禁手写 |
| 镜像字段与内核状态不一致 | 幽灵 bug（插件改了字段内核没看见） | 单一权威 + 固定同步点；debug 构建校验双向一致 |
| 第三方类路径缺失 | 大量插件 `NoClassDefFoundError` | §5.3 契约表 + 扫描器检测 import 并预检 |
| Bukkit API 表面积大 | 兼容永远"差一点" | 按真实插件调用频率分级，apidiff 机械保证已实现部分正确 |
| 世界生成与原生不一致 | 地形预期不符 | 文档声明；优先保证 Anvil 读写正确 |
| JNI 成瓶颈 | 插件多时 TPS 崩 | 事件短路、对象池、批量 upcall；基准门槛进 CI |
| 参考代码污染实现 | 法律风险 | 评审清单 + `docs/reference-log.md` 审计 |
| 7GB 内存/4 核 | 无法本机验证规模指标 | 规模测试外借机器，本机只跑单机基准 |

---

## 13. 当前状态

- [x] 环境与依赖确认（§6.3）
- [x] 离线物化并校验 Spigot 1.12.2 jar（哈希比对通过，`tools/reproduce_jars.sh` 幂等）
- [x] 表面积与命名构成实测（§5.1）
- [x] ABI 编译链路验证（`javac -cp spigot.jar` 编译含 NMS 的类通过）
- [x] 架构、兼容策略与里程碑规划（本文档）
- [x] **M0 工程骨架**
- [x] **M1 协议与连接（M1a + M1b：Handshake/Status/Ping/加密登录完成）**
- [x] **M2 世界与移动（超平坦区块、移动同步、多人可见、聊天、KeepAlive、动态区块加载）**
- [~] **M3 玩法基础（M3a 方块破坏/放置 + M3b 物品栏与窗口同步完成；容器/合成/熔炉/掉落物/生物/伤害重生待做）**
- [ ] M4 插件基座
- [ ] M5 Bukkit API 覆盖扩展
- [ ] M6 NMS shim
- [ ] M7 世界生成与规模
- [ ] M8 性能与加固

### M0 交付

```
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/cyane --ticks 40          # 空 tick 循环，40 tick @20Hz = 2s
./build/cyane --version
```

| 模块 | 内容 |
|---|---|
| 构建 | CMake 4.4 + clang 22 + C++23，thin LTO，lld，`-Werror` 可选；自动探测 zlib-ng/OpenSSL/liburing |
| `core/error` | `std::expected` 错误传播，8 类 `ErrorCode` |
| `core/mpmc_queue` | Vyukov 有界 MPMC，缓存行对齐，零锁 |
| `core/inline_function` | 48 字节内联存储的可移动 callable，任务提交零堆分配（有测试断言） |
| `core/log` | 无锁队列 + 后台 flusher；热路径不分配不阻塞，队列满计 dropped；同时写控制台与文件 |
| `core/config` | TOML 子集解析，带行号错误，类型严格校验 |
| `core/time` | `Ticker` 固定节拍（过载重同步，不累加欠债）、`TickStats` 窗口统计 |
| `core/thread_pool` | 信号量唤醒 + MPMC 队列，任务异常隔离，`wait_idle` 语义可靠 |
| `game/server` | 20Hz tick 循环、优雅停机（SIGINT/SIGTERM）、控制台命令 |
| 代码生成 | `data/registry.toml` → `generated/registry_meta.hpp`（含参考 jar 哈希指纹） |
| 测试 | 35 个用例，`ctest` 通过；ASan+UBSan 构建下零错误 |

已验证：40 tick @20Hz 精确 2.00s、0 过载；工作线程数默认 `核数-1`；配置错误（端口越界、类型不符、未知日志级别）在启动期报错并退出码 1。

### M1 交付（M1a + M1b 完成）

```
./build/cyane --config server.toml
python3 tools/probe_status.py 127.0.0.1 25565   # 打本机实例
python3 tools/probe_status.py 127.0.0.1 25599   # 打原版 oracle（jars/vanilla）
```

| 模块 | 内容 |
|---|---|
| `core/bytes` | VarInt/VarLong/字符串/位置/大端编解码 + UTF-8 校验；位置布局由 `BlockPosition` 反编译锁定 |
| `proto/frame` | 帧编解码（压缩阈值 256，zlib-ng compat），长度/解压上限防攻击 |
| `crypto` | AES-128-CFB8（IV=密钥），RSA-1024 生成与解密（OAEP），SHA-1 签名，离线 UUID 派生 |
| `session` | `SessionService` 接口 + `MojangSessionService`（HTTPS GET 验证会话令牌） |
| `net/reactor` | epoll ET + `data.ptr` 直分发，零查表 |
| `net/connection` | 状态机（handshake/status/login）、半包重组、粘包、写入背压（高水位断开）、收包上限；**加密登录完整流程**（EncryptionRequest→Response→密文启用→SetCompression→LoginSuccess） |
| `net/net_service` | SO_REUSEPORT 多 reactor，每线程独占连接集，EMFILE 退避 |
| `game/status` | 状态 JSON，键序与原版一致 |
| 工具 | `tools/probe_status.py` / `tools/probe_login.py`（行为预言机）、`tools/run_local_server.sh`（pidfile 启停）、Vineflower 反编译器 |
| 文档 | `docs/protocol.md`（340 事实与验证状态）、`docs/reference-log.md` R-007 至 R-009 |
| 测试 | 76 个用例全绿（ASan/UBSan 亦全绿） |

**验证结论**：状态响应（Handshake→Status→Ping→Pong）与原版 1.12.2 服务端**逐字节一致**；登录流程与原版行为对照（offline 序列 `SetCompression→LoginSuccess`、online 的 `EncryptionRequest` 字段布局与 162 字节 RSA 公钥）；**M1b 加密登录实现完成**：AES-128-CFB8 会话密钥协商、RSA-1024 解密、压缩启用（SetCompression→LoginSuccess）、`SessionService` 会话验证，76 个测试全绿。协议不匹配给出原版同款 `Outdated client!`。

### M2 交付（世界与移动完成）

登录成功后进入 Play 状态并按原版顺序下发初始化包，客户端进入超平坦世界自由移动、互相可见、聊天：

```
0x03 SetCompression   阈值 256，先于 LoginSuccess，之后启用压缩
0x02 LoginSuccess     UUID(36 位带连字符) + 用户名
0x23 JoinGame         int entityId | byte gameMode | int dimension | byte difficulty
                      | byte maxPlayers | string levelType | bool reducedDebug
0x46 SpawnPosition    position long（x/y/z 打包）
0x2E PlayerInfo       action=0 | count | UUID(16 字节二进制) | name | props | gameMode | ping | hasDisplayName
0x41 UpdateHealth     float health | varint food | float saturation
0x47 TimeUpdate       long worldAge | long timeOfDay
0x20 ChunkData        视距内超平坦区块（per-section 调色板 + 打包 long[] + 光照 + 生物群系）
0x2F PlayerPositionLook  绝对坐标下车，客户端回 ConfirmTeleport
```

| 模块 | 内容 |
|---|---|
| `world/chunk_codec` | Chunk Data(0x20) 线格式：per-section bitsPerBlock + 线性调色板 + 跨 long 打包（复刻 `DataBits.a`）+ blockLight/skyLight + 生物群系；`make_flat_chunk` 超平坦出生地形 |
| `net/player_hub` | 线程安全多人广播中心：每玩家带锁 mailbox，跨 reactor 线程只投递逻辑消息不触碰对端 socket；全员/按区块范围广播 |
| 移动同步 | Position/PositionLook/Look/Flying → EntityTeleport(0x4C 绝对坐标) + EntityHeadLook(0x36)；角度 float→字节角编码 |
| 可见性 | 登录互发 PlayerInfo+SpawnPlayer、补发已在线玩家、退出广播 DestroyEntities+PlayerInfo(remove) |
| 聊天 | ChatMessage 广播全员（含自己），服务端日志 `<name> message` |
| KeepAlive | 进入 Play 后 10s 心跳，30s 超时断开 |
| 动态区块 | 玩家跨区块时按视距（切比雪夫环）加载缺失区块、卸载出界区块（UnloadChunk 0x1D） |
| 控制台 | `help`/`tps`/`say`/`stop` 命令（后台读取线程，SIGINT 即时停机） |
| 工具 | `tools/probe_login_full.py`（逐包验证）、`tools/probe_visibility.py`（多连接互见/角度） |

**验证结论**：两个客户端可互相看到并聊天；出生点按实体 id 错开成网格，避免重叠导致的视锥剔除消失；角度字节编码对负 yaw/大 yaw 正确回绕。

### M3 交付（M3a 方块交互 + M3b 物品栏，进行中）

创造/生存模式下可破坏、放置方块，客户端登入即同步整份背包：

```
serverbound:
0x14 PlayerDigging          varint status | position | byte face（创造 status0 即破坏，生存 status2）
0x1F BlockPlacement         position | varint face | varint hand | float cursorX/Y/Z
0x1A HeldItemChange         short slot（切换热区栏选中）
0x1B CreativeInventoryAction short slot | slot（创造改物品）
0x07 ClickWindow            byte win | short slot | byte button | short action | varint mode | slot
clientbound:
0x0B BlockChange            position | varint blockStateId（按区块范围 broadcast_near）
0x14 WindowItems            登入下发整份背包（46 槽）
0x16 SetSlot                单槽权威同步
0x11 ConfirmTransaction     ClickWindow 事务回执
0x2C PlayerAbilities        创造模式允许飞行/免疫
```

| 模块 | 内容 |
|---|---|
| `world/world` | 共享可编辑方块存储：超平坦 baseline + 编辑覆盖表（`std::mutex` 保护，多 reactor 共享），`build_chunk` 打底套用编辑 |
| `world/blocks` | 物品→方块状态映射（1.12.2 方块型物品 id<256 与 block id 同值），六向 face 增量 |
| `item/item_stack` | `ItemStack{id,count,damage}` 与 1.12.2 网络 slot 编解码（带 NBT 的物品本阶段拒绝） |
| `item/player_inventory` | 46 槽玩家背包（护甲/2×2 合成/主包/热区栏/副手）与热区栏→窗口槽映射 |
| 方块交互 | 破坏置空气、放置按手持物品映射；放置碰撞检测拒绝挤压玩家的格子并回滚；生存放置消耗手持物品 |
| 窗口同步 | 登入 WindowItems、创造 CreativeInventoryAction 写入并 SetSlot 回发、ClickWindow 回 ConfirmTransaction + 权威重同步 |
| 配置 | `server.game_mode`（默认 creative）驱动 JoinGame gameMode 与 PlayerAbilities |
| 工具 | `tools/probe_blocks.py`（破坏/放置/持久化/碰撞拒绝）、`tools/probe_inventory.py`（背包同步/点击事务） |
| 测试 | 82 个用例全绿（新增 `tests/test_item.cpp` 6 项） |

**验证结论**：破坏/放置后写入 World 并持久化——新登入客户端读区块采样一致；放置到玩家碰撞体的格子被拒绝并回滚（修复"站在方块上右键被顶出"）；登入即收到 46 槽 WindowItems，创造改物品经 SetSlot 反映，点击有 ConfirmTransaction 回执。

**M3 待做**：ClickWindow 真实拿放/堆叠语义、容器窗口（箱子/工作台/熔炉）、合成、掉落物与拾取、生物生成与 AI、伤害与重生。


