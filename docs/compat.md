# Spigot 兼容契约

Spigot 兼容 = **Bukkit API** + **NMS shim** + **第三方类路径**，三者缺一插件就崩。以下数据由本机 Spigot jar 实测得出。

## 表面积实测

| 组成 | 类数 | 说明 |
|---|---:|---|
| `org.bukkit.*`（净 API） | **825** | 插件编译链接的接口面，M7/M8 主目标 |
| `org.bukkit.craftbukkit.v1_12_R1.*` | 346 | CraftBukkit 实现类，插件常 `import` 其静态工具 |
| `net.minecraft.server.v1_12_R1.*` | **2324** | NMS 面 |
| `org.bukkit.craftbukkit.libs.*` | 10873 | shaded 库（fastutil 等），插件可 import |

NMS 类聚类（2324 总计）：`Block*` 304、`Entity*` 256、`World*` 229、`Packet*` 169、`Item*` 125、`TileEntity*` 49、`Player*` 42、`Server*` 23、`NBT*` 20、`Chunk*` 17。

热点类公开成员数（方法+字段）：`Entity` 290、`World` 250、`EntityLiving` 208、`EntityPlayer` 135、`ItemStack` 86、`WorldServer` 64、`PlayerConnection` 48、`NBTTagCompound` 43、`Container` 36。

**两个决定性事实**（实测）：

1. **NMS 命名是 MCP 与混淆的混合体**。类名是 MCP 名（`EntityPlayer`/`WorldServer`/`PacketPlayOutChat` 均在），但成员名只有一部分被 MCP 覆盖：`EntityPlayer` 57 个公开成员是混淆短名（`d`/`e`/`f`），77 个是 MCP 名。
   → **签名清单必须从 jar 机械提取（javap），不能按 MCP 知识手写**，否则插件链接失败。
2. **公开可变字段是 ABI 的一部分**。插件直接写 `entity.locX = ...`、`entity.motY = ...`、`entity.dead = true`，这类访问不走方法调用。
   → shim 必须提供**镜像字段**，不能只做方法委托。

## NMS 分层策略

结论：**NMS 可以做，但形态是"镜像字段 + 委托的兼容层"，不是通用转译器**。转译只能改名字、不能造语义。

| 层 | 内容 | 成本 | 覆盖价值 |
|---|---|---|---|
| **T0** | 仅 Bukkit API（825 类） | M7–M8 主线 | 纯 API 插件 |
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

**转译层真正的位置**：不是用来"猜测语义"，而是用来**维持一套 shim 服务多个版本**——在自研 `ClassLoader.findClass` 里用 ASM `ClassReader → ClassRemapper → ClassWriter` 把 `v1_8_R3` 等旧版引用映射到我们唯一的 `v1_12_R1` shim。约 300 行，仅类加载期一次，运行期零开销。我们只做 1.12.2，所以这是后期的可选增益。

**配套机制（比 shim 本身更重要）**

- **插件扫描器**（M7 就做，很便宜）：加载期扫常量池，判定插件属于哪个 tier；不支持时给出**缺失符号清单**而不是运行期 `NoClassDefFoundError`。同时它产出真实需求数据——**T1 的覆盖面靠扫描报告数据驱动增长，而不是预先规划**。
- **API 签名校验**（已验证可行）：对着 Spigot jar 编译 + javap 签名 diff，机械保证我们 825 类 API 与 shim 的 ABI 一致。

## 第三方类路径契约

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

## 参考 jar（`jars/`，不入库）

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

## 工具链

| 用途 | 工具 | 状态 |
|---|---|---|
| 签名提取（ABI 契约） | `javap -cp <spigot.jar>` | 已有（JDK 21） |
| 语义参考（阅读逻辑） | Vineflower / CFR | 已有 |
| 协议对照 | 原版服务端 + 客户端抓包 | jar 已就位 |
| 编译校验 | `javac -cp <spigot.jar>` | **已验证**：含 NMS 的探针类编译通过 |
| ABI diff | 自研 `tools/apidiff`（javap → 结构化清单对比） | M7 交付 |

## 环境

clang 22.1.8、cmake 4.4.3、make、lld、JNI 头（GraalVM 21）、zlib-ng 2.3.3、libdeflate 1.26、OpenSSL 3.6.4、liburing 2.15、zstd 1.5.7、xxhash。
缺：ninja（用 Makefile 生成器）、mold（用 lld）。
硬件：4 核 / 7GB 内存（可用约 3GB）/ 36GB 空闲——**1000 玩家压测不在此机进行**。