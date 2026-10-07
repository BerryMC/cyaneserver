# 架构设计

## 1. 进程与线程模型

CyaneServer 采用基于现代 C++ 的高性能单进程并发架构：

```
                    ┌────────────────────────────┐
                    │    epoll (SO_REUSEPORT)    │
                    │  Net-0   Net-1   ... Net-N │ (I/O Reactor 线程池)
                    └──────┬───────────────┬─────┘
                           │ 邮箱 (Mailbox)│
                           ▼               ▼
                  ┌────────────────────────────────┐
                  │       PlayerHub 广播中心        │
                  └───────────────┬────────────────┘
                                  │ 20Hz 定拍驱动
                                  ▼
                  ┌────────────────────────────────┐
                  │       Server Tick (主线程)      │
                  │  世界物理 / 怪物AI / 掉落物 / 碰撞 │
                  └───────────────┬────────────────┘
                                  │ 异步落盘 / 任务分发
                                  ▼
                  ┌────────────────────────────────┐
                  │    ThreadPool (Worker 线程池)   │
                  │   Region 落盘 / 区块加载 / 保存   │
                  └────────────────────────────────┘
```

- **Tick 线程（主线程，20Hz）**：
  - 拥有世界与实体状态的权威变更权。驱动方块 Tick、生物 AI（漫游/追击/攻击/自爆/走位）、掉落物物理、箭矢飞行、经验球运算与世界保存编排。
- **Net 线程（I/O Reactor，`net/reactor.hpp`）**：
  - 基于 Linux `epoll` 边缘触发（Edge-Triggered）机制，运行每连接的网络帧解析、加解密（AES-128-CFB8）、压缩解压（zlib-ng）与写入缓冲管理。
- **线程通信解耦（Mailbox / Actor 模式）**：
  - `PlayerHub`（`net/player_hub.hpp`）为每个在线玩家维护独立的带锁消息邮箱（Mailbox）。
  - Tick 线程生成的广播与事件（如生物移动、音效、方块变更、视距过渡包）无阻塞地压入目标连接的邮箱，由各 Reactor 线程在连接 Tick 时批量排干（`drain_mailbox`）并异步写出到 Socket，杜绝主循环被网络 I/O 阻塞。

---

## 2. 核心子系统与模块划分

### 2.1 基础核心 `src/core/`
- **`core/bytes.hpp`**：Minecraft 协议基础线格式（VarInt/VarLong/大端整型/Position/UTF-8 长度前缀字符串/ByteReader/ByteWriter）。
- **`core/mpmc_queue.hpp`**：Vyukov 有界多生产者多消费者无锁队列，缓存行对齐，零分配。
- **`core/thread_pool.hpp`**：基于信号量与 MPMC 的通用工作线程池，提供安全的任务隔离。
- **`core/log.hpp`**：无锁队列驱动的异步日志系统，支持控制台颜色高亮与磁盘文件持久化。
- **`core/config.hpp`**：严格类型的轻量级 TOML 配置解析器，启动期快速校验非法参数。
- **`core/time.hpp`**：高精度定拍计时器（Ticker）与 TPS/Tick 耗时统计器（TickStats）。

### 2.2 协议与网络通信 `src/proto/` & `src/net/`
- **`net/reactor.hpp` & `net/socket.hpp`**：非阻塞 Socket 与 Linux epoll ET 抽象。
- **`net/net_service.hpp`**：多线程 Reactor 拓扑，支持 SO_REUSEPORT 负载分发。
- **`net/connection.hpp`**：单客户端会话生命周期管理，涵盖：
  - 握手（Handshake）与状态反馈（Status/Ping，支持 MOTD 与在线人数）。
  - 加密认证（RSA-1024 交换与 AES-128-CFB8 流加密）与 zlib 帧压缩。
  - 游戏通信（Play 状态 40+ 数据包分发与处理）。
  - 突发区块下发（Burst Delivery）与平滑限流（`send_pending_chunks`）。
- **`net/player_hub.hpp`**：多人状态广播中心，提供切比雪夫距离局部广播（`broadcast_near`）与跨区块视距平滑过渡（`transition_entity`）。
- **`net/packet_writers.hpp`**：协议数据包标准编码器（零分配，规范线格式唯一定义点）。

### 2.3 世界、区块与存储 `src/world/`
- **`world/world.hpp`**：完整区块世界模型。支持多 Section 数组索引、方块读写、光照数据和脏标记追踪。
- **`world/chunk.hpp`**：区块（16×256×16）与 Section（16×16×16）数据结构，内建独立轴地板除 `ChunkPos::from_world`。
- **`world/blocks.hpp`**：方块 ID、全局状态值（BlockState）与 Metadata 转换、AABB 碰撞盒判定与附着方块支撑关系。
- **`world/region.hpp`**：原版 Anvil `.mca` 区域文件低级读写引擎，实现扇区分配表管理、原子落盘（temp + rename）。
- **`world/anvil.hpp`**：区块与原版 NBT 数据双向编解码器。
  - 支持 `encode_chunk_merged`：以磁盘原始数据为底，精准增量替换 Blocks/Data/Add 与 TileEntities，实现**零损耗覆盖**。
  - 实体序列化：落盘掉落物（`minecraft:item`）与生物（`minecraft:pig/cow/sheep/chicken/zombie/creeper/skeleton/spider`）。
- **`world/pathfinding.hpp`**：**原版 A\* 寻路引擎**。
  - 包含 `PathHeap`、`WalkNodeProcessor`（方块通行代价评估、水/火/熔岩加权）。
  - 依据实体宽度/高度包围盒实时做跳跃与台阶通过性检测，从根本上保证生物寻路不坠崖。
- **`world/physics.hpp`**：AABB 扫掠移动与方块碰撞（`move_with_collision`）、地表滑动摩擦。

### 2.4 物品、背包与工艺 `src/item/`
- **`item/item_stack.hpp`**：1.12.2 Slot 网络编解码（支持物品 ID、堆叠上限、Damage、NBT/附魔透传）。
- **`item/player_inventory.hpp`**：46 槽玩家标准背包（主背包、热区栏、盔甲槽、副手）。
- **`item/crafting.hpp`**：数据驱动的配方系统（从 `config/recipes.toml` 动态解析），支持有序（Shaped）与无序（Shapeless）合成。
- **`item/item_tools.hpp` & `item/item_traits.hpp`**：工具材质采集资格、武器攻击伤害、护甲减伤系数表。

### 2.5 实体与玩法生态 `src/net/` & `src/game/`
- **`net/mob_manager.hpp`**：生物生命周期与 AI 状态机驱动器：
  - 被动生物：漫游（Wander）、停留（Idle）、受击恐慌奔跑（Panic）。
  - 敌对生物：索敌追击（Chase）、近战挥击、苦力怕膨胀引爆（Swell）、骷髅持弓风筝走位（Kiting）与射击。
  - 生态交互：喂食繁殖（In Love）、剪羊毛（Shear）、挤牛奶。
  - 消失控制：原版 Despawn 规则（动物永不清除、虚空玩家不触发清除、敌对生物 >128 格清除）。
- **`net/item_drop.hpp`**：掉落物管理器（`ItemDropManager`）：
  - 原版 `EntityItem` 物理模拟（抛掷初速、地面滑动阻尼、浮力弹起、相邻同类合并吸收、5 分钟消亡）。
- **`net/xp_orb.hpp`**：经验球系统（物理下落、跟随玩家吸附、合并与升级公式结算）。
- **`net/projectile_manager.hpp`**：投掷物系统（箭矢弹道轨迹模拟、重力加速度、方块与生物碰撞检测）。
- **`net/container_store.hpp` & `net/furnace_store.hpp`**：容器持久化：
  - 27 槽箱子、小容器（发射器/投掷器/漏斗）。
  - 反应式熔炉状态机（燃料燃烧倒计时、冶炼进度步进、产出槽满阻塞、进度条网络同步）。
- **`game/player_data.hpp`**：玩家 `.dat`（gzip NBT）序列化与反序列化，与原版双向无缝互通。
- **`game/world_persistence.hpp`**：世界持久化调度器，自动保存定时落盘与脏区块管理。

---

## 3. 设计原则与规范

1. **性能优先（Performance First）**：
   - 核心通信与 Tick 循环避免昂贵内存拷贝与隐式动态分配。
   - 实体视距过渡使用精确的切比雪夫增量算法，杜绝全服泛洪广播。
2. **原版权威机制优先（Vanilla Fidelity）**：
   - 机制、发包格式、伤害计算与寻路逻辑严禁臆造，必须有原版反混淆源码（1.12.2）或 Cuberite 源码作为事实凭据。
3. **现代 C++ 规范（Modern C++ Standards）**：
   - 全面使用 C++23 特性（`std::expected`、`std::span`、`<format>`、三向比较运算符 `<=>`、现代结构化绑定等）。
   - 采用 Clang 严格告警级别编译（`-Wall -Wextra -Wpedantic -Wconversion -Werror`）。
