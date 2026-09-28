# 架构

## 进程与线程

单进程：C++ 内核 + 内嵌 JVM（JNI Invocation API）。

```
        ┌─────────────── epoll (SO_REUSEPORT) ───────────────┐
        │  Net-0        Net-1        Net-2        Net-N      │
        └──────┬───────────┬───────────┬───────────┬─────────┘
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

## 分层

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

## 子系统

### 网络层 `src/net/`

- `epoll(ET)` 抽象，留 io_uring 后端接口。
- 每连接读环 + VarInt 帧头增量解析（容忍半包/粘包，超长帧拒绝）。
- 压缩 `zlib-ng`（阈值以下直发，默认等级 4）；加密 `AES-128-CFB8`（OpenSSL EVP，每连接独立上下文）。
- 写路径：组包进发送环 → 每 tick 或达水位 `writev` 批量刷出；高水位背压暂停读。强制 `TCP_NODELAY`。
- 兼容 BungeeCord/Velocity IP 转发与 PROXY protocol v1/v2（未实现）。

### 协议层 `src/proto/`

- 手写编解码，包描述用 `consteval` 表在编译期生成读写器，运行期无反射无查表开销。
- 1.12.2 要点：`Chunk Data` section = bits-per-block(4..8) + palette + 4096 longs + blocklight 2048 + skylight 2048 + biome 256；实体元数据 `0x3E` 索引/类型严格对齐；**网络 NBT 与 Anvil NBT 是两套编码**，writer 分离。
- 包 ID 与字段布局以抓包对照校验。

### 世界层 `src/world/`

- 方块状态注册表：`constexpr` 表 + O(1) 双向映射。
- Chunk 生命周期 `Proto → Generated → Populated → Lit → Loaded → Active`。
- 光照引擎：blocklight/skylight 独立 nibble 数组；禁止每 tick 全量重算。
- 存储：当前为内存编辑表；Anvil `.mca` 待实现。
- 碰撞：voxel shape 索引 + AABB 扫掠。
- 存储：超平坦 baseline + 内存编辑表；Anvil `.mca` 待实现。玩家与容器数据另有独立落盘通道。

### 实体与玩家 `src/entity/`

- 玩家：库存（窗口同步 ID）、经验、附魔、药水、成就、Tab、记分板（经验/药水/成就/记分板未实现）。
- 玩家持久化 `game/player_data.hpp`：按 UUID 落盘位置/朝向/游戏模式/血量/46 格背包（`server.player_data_dir`，登录恢复、断开保存）。

### 游戏层 `src/game/`

- 固定 20Hz tick，超时补偿与 TPS 统计。
- 调度器 `sync/async/delayed/repeating`（未实现）。
- 命令 + Tab 补全（1.12.2 旧协议 `0x0B`）。
- 事件总线（未实现）。

### 插件层 `src/plugin/` + `java/`（未实现）

- **嵌入**：`JNI_CreateJavaVM`，`-Xmx` 可配；JVM 版本探测（8/11/17/21）。
- **类路径**：必须复现 Spigot 的暴露面。
- **加载**：每插件独立 `URLClassLoader`；自研 mini-YAML 解析 `plugin.yml`。
- **事件**：C++ 侧持 `(eventType → 有序 handler)`，Java 侧注册时回传 `jmethodID`。

## 签名清单驱动

`data/nms-shim-v1_12_R1.toml` 由 `tools/nms_manifest` 从 Spigot jar 机械生成，声明我们**实际支持**的类与成员（含混淆短名）。Java 骨架与 C++ 绑定表均由它生成，做到可审计、可重生成、可数据驱动增长——手写必然写错混淆名。