# 参考日志

记录读了哪些原版/Spigot 产物、得出什么结论、落地到哪个文件。目的是让"参考"与"使用"的边界可审计：本项目源码不得包含参考代码的复制品，只允许自写实现。

## 规则

- 阅读参考代码是为了理解**语义与签名**，落地实现必须自写。
- 机械提取的事实（类名/方法名/描述符/字段名、包 ID、字段布局）可记录并用于生成清单。
- 不得把参考代码逐段搬运进 `src/`、`java/`。
- 每条记录注明：来源、结论、落地位置。

## 参考源

| 标识 | 路径 | 说明 |
|---|---|---|
| `vanilla-1.12.2` | `jars/vanilla/server.jar` | 原版服务端，2279 个混淆 NMS 类 + 库 + `assets/minecraft/` 数据 |
| `vanilla-client-1.12.2` | `jars/vanilla/client.jar` | 原版客户端，用于对照客户端对封包的期望 |
| `spigot-1.12.2` | `jars/spigot/spigot-1.12.2.jar` | 由 `tools/reproduce_jars.sh` 离线派生，MCP 命名 NMS + Bukkit API |

## 记录

### R-001 — Spigot 引导壳的物化方式

- 来源：`spigot/server.jar`（Paperclip 引导壳）、`patch.properties`
- 结论：该 jar 不是服务端本体，`Main-Class: io.papermc.paperclip.Paperclip`，内含 `spigotMC.patch`（jbsdiff，37MB）与 `patch.properties`（`version=1.12.2`、`originalHash`、`patchedHash`、`sourceUrl`）。用 `org.jbsdiff.ui.CLI patch` 把补丁打在 vanilla 上即可离线得到真 Spigot jar，不必联网，也不必运行 paperclip。
- 校验：`jars/vanilla/server.jar` 的 sha256 等于 `originalHash`；派生结果等于 `patchedHash`。
- 落地：`tools/reproduce_jars.sh`，`data/registry.toml` 的 `[reference]`。

### R-002 — 兼容面规模实测

- 来源：`spigot-1.12.2` 类清单
- 结论：
  - `org.bukkit` 净 API **825** 类（排除 `craftbukkit` 346 类与 shaded libs 10873 类）。
  - `net.minecraft.server.v1_12_R1` **2324** 类。聚类：`Block*` 304、`Entity*` 256、`World*` 229、`Packet*` 169、`Item*` 125、`TileEntity*` 49、`Player*` 42、`Server*` 23、`NBT*` 20、`Chunk*` 17。
  - 热点类公开成员数：`Entity` 290、`World` 250、`EntityLiving` 208、`EntityPlayer` 135、`ItemStack` 86、`WorldServer` 64、`PlayerConnection` 48、`NBTTagCompound` 43、`Container` 36。
- 落地：`docs/compat.md` 表面积实测。

### R-003 — NMS 命名是 MCP 与混淆的混合体

- 来源：`javap` 读取 `EntityPlayer`、`Entity`
- 结论：类名是 MCP 名（`EntityPlayer`/`WorldServer`/`PacketPlayOutChat` 均在），但成员名只有一部分被 MCP 覆盖——`EntityPlayer` 的公开成员中 **57 个是混淆短名**（`d`/`e`/`f`），77 个是 MCP 名。
- 影响：签名清单必须由 `javap` 从 jar 机械提取，**禁止按 MCP 知识手写**，否则插件链接失败。
- 落地：`docs/compat.md` NMS 分层策略 的 `tools/nms_manifest` 设计（M6 实施）。

### R-004 — 公开可变字段属于 ABI

- 来源：编译探针（`javac -cp spigot.jar`）+ `javap`
- 结论：插件直接读写 `entity.locX`、`entity.motY`、`entity.dead` 这类公开字段，不经过方法调用。这类字段分布在继承链上（如 `locX` 声明在 `Entity`，`EntityPlayer` 继承）。
- 影响：NMS shim 必须提供**镜像字段**而非纯方法委托；C++ 侧保持权威，在 tick 边界批量同步。
- 落地：`docs/compat.md` NMS 分层策略 的镜像同步设计（M6 实施）。

### R-005 — 第三方类路径也是兼容契约

- 来源：`spigot-1.12.2` 类清单
- 结论：Spigot 把一批库**未混淆**暴露给插件：`net.md_5.bungee.api.chat`、`org.yaml.snakeyaml`、`com.google.gson`、`com.google.common`、`io.netty`、`org.apache.commons.lang3`、`org.apache.commons.io`、`org.sqlite`、`com.mysql`、`gnu.trove`、`org.spigotmc`；fastutil 则 shaded 在 `org.bukkit.craftbukkit.libs.it.unimi.dsi.fastutil` 下。
- 影响：内嵌 JVM 的 classpath 必须复现这一暴露面，否则大量插件 `NoClassDefFoundError`。
- 落地：`docs/compat.md` 第三方类路径契约（M4 实施）。

### R-006 — 原版数据资产

- 来源：`vanilla-1.12.2` 资源清单
- 结论：jar 内含 `assets/minecraft/`：`advancements/` 504 项、`recipes/` 433 项、`structures/` 118 项、`loot_tables/` 88 项。这些是数据驱动的注册表定义，可作为语义参考。
- 落地：M2/M3 的注册表与合成/战利品系统（待实施）。

### R-007 — 登录流程行为观测

- 来源：`tools/probe_login.py` 打原版 1.12.2 服务端（offline 与 online 两种模式）
- 结论：
  - offline：LoginStart → `SetCompression(0x03, 256)` → `LoginSuccess(0x02, uuid String + name String)` → `JoinGame(0x23)`；无加密。
  - online：LoginStart → `EncryptionRequest(0x01)`：serverId 空串、**1024 位 RSA X.509 SPKI DER 公钥 162 字节**、verifyToken 4 字节。
  - SetCompression 先于 LoginSuccess，之后所有包按 `长度|未压缩长度|数据` 压缩帧编码。
- 落地：`docs/protocol.md`，M1b 的加密登录实现（待实施）。

### R-008 — 1.12.2 位置编码布局

- 来源：反编译 `BlockPosition`（`asLong`/`fromLong`）与 `MathHelper`（c/d/e/g）
- 结论：`c = 1 + e(c(30000000)) = 26`，`f = 64 - 26 - 26 = 12`，`g = 26`，`h = 38`；即 `x(26) << 38 | y(12) << 26 | z(26)`，与 1.8–1.13 一致（1.14+ 改为 y 低位）。
- 影响：初版实现误用 1.14+ 布局，由 `bytes_position_layout_matches_protocol_340` 黄金向量锁定后修正。
- 落地：`include/cyane/core/bytes.hpp` 的 `encode_position` 系列。

### R-009 — M1b 加密登录实现

- 来源：`docs/protocol.md` 已验证事实、R-007 观测结论
- 结论：
  - **Crypto 模块**（`cyane::crypto`）：AES-128-CFB8，IV = 密钥前 16 字节；RSA-1024 密钥生成与解密（OAEP 填充）；SHA-1 用于 serverId 签名；离线模式用 SHA-1(serverId + "undefined") 派生 UUID。
  - **SessionService**（`cyane::session`）：`SessionService` 接口 + `MojangSessionService` 实现，最小 HTTPS GET 验证会话令牌，证书验证可选关闭。
  - **Connection 登录重写**：`handle_login_start` → 发送 `EncryptionRequest(0x01)` → 接收 `EncryptionResponse(0x01)` → 解密会话密钥 → 启用加密读写路径 → 发送 `SetCompression(0x03)` → `LoginSuccess(0x02)`。
  - **密文接入**：读路径解密原始缓冲后交帧解码器；写路径整帧加密后入发送队列。`Connection::alive_` 为 false 时停止加密。
  - **测试验证**：`integration_login_start_gets_disconnect` 通过，76 个测试全绿。`receive_compressed_packet` 用于 SetCompression 后的所有包。
- 落地：`src/crypto/`（aes.hpp, rsa.hpp, sha1.hpp, session_service.hpp）、`src/net/connection.cpp`（handle_login、handle_encryption_response、finish_login）、`tests/test_status_ping.cpp`。
