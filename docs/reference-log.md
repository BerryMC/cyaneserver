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
### R-010 — playerdata `<uuid>.dat` 格式（原版预言机实测）

- 来源：原版 1.12.2 服务器（Java 21 可直接运行）在线模式关闭下，真实客户端登录后停机，取 `world/playerdata/<uuid>.dat`（708 字节）；载入 `tests/fixtures/vanilla_player_oracle.dat`（sha256 `140e093e…db8d8`）。
- 结论：
  - 根为**未命名** TAG_Compound，字段**直接**位于根下（无 `Data` 包装——`Data` 是 level.dat 的结构）。
  - 空列表元素类型写 `TAG_End`(0)（`Inventory`/`EnderItems` 实测）。
  - 字段名实测：`Pos`(List<Double>×3)、`Motion`(×3)、`Rotation`(List<Float>×2)、`Health`(Float)、`playerGameType`/`Dimension`/`Score`/`XpLevel`/`XpTotal`/`foodLevel`/`foodTickTimer`/`DataVersion`(Int=1343)、`XpP`/`foodSaturationLevel`/`foodExhaustionLevel`(Float)、`Air`/`Fire`(Short)、`OnGround`/`Invulnerable`/`seenCredits`(Byte)、`SelectedItemSlot`(Byte)、`UUIDMost`/`UUIDLeast`(Long)、`abilities`(Compound：invulnerable/flying/mayfly/instabuild/mayBuild=Byte，flySpeed/walkSpeed=Float)、`EnderItems`(List)。
  - 载体为 **gzip**（region 是 zlib 版本字节 2，.dat 无版本字节直接 gzip）。
  - Inventory NBT 槽位（公认布局，EntityEquipmentSlot.getSlotIndex）：0-8 热区、9-35 主背包、100-103 护甲（100=脚…103=头）、40 副手；2x2 合成格不持久化。
- 落地：`include/cyane/game/player_data.hpp` + `src/game/player_data.cpp`（`build_vanilla_nbt`/解析、`window_slot_to_nbt`/`nbt_slot_to_window` 槽位映射、遗留 JSON 自动迁移）；`proto::deflate_gzip`；`nbt::parse_compressed`；`tests/test_player_data.cpp`（fixture 锁定 + 双向互通）。
- 互操作实测：我们写的 `.dat` 由原版服务器加载，玩家以文件中的游戏模式（创造，server.properties 默认生存，字段值必出自我们的文件）与坐标出生；原版停机后回写保留该值。

### R-011 — 区块记录（region）与 level.dat 格式（原版预言机实测）

- 来源：原版 1.12.2 服务器生成的世界（扁平模式，6.3MB region）；区块记录与 `level.dat` 逐字节观测，fixture 入库（`tests/fixtures/vanilla_region_chunk.bin` sha256 `6bc883be…c2bd`、`tests/fixtures/vanilla_level_oracle.dat` sha256 `080b30f6…5270`）。
- 结论：
  - **区块记录格式**：`[长度:4B 大端，含压缩字节][压缩类型:1B][压缩数据]`。压缩类型 1=gzip、2=zlib、3=未压缩。实测 `00 00 01 0a 02 78 9c…` → 长度 266、类型 2、载荷 265 字节 zlib。
  - **修正**：仓库早期 `region.cpp` 按"3 字节长度 + 版本字节"读取并写出，与自己的写自洽但**与原版不互通**（原版区块全部解压失败，我们的文件原版也读不了真数据）；`write`/`read` 已统一为上述 4 字节布局。
  - **level.dat**：根 compound 含 `Data` compound（与 playerdata 的"字段直接在根下"不同），出生点在 `Data.SpawnX/SpawnY/SpawnZ`（实测 247/4/1091）。容器为 gzip。
  - **区块 Level 字段实测**：`LightPopulated`、`HeightMap`(i32[256])、`Sections`（每节含 `Y`/`Blocks`/`Data`/`BlockLight`/`SkyLight`）。我们早期只写 Blocks/Data/Add 会让这些字段丢失——无损保存改为"以磁盘原始 NBT 为底打补丁"。
- 落地：`world/region.cpp`（记录布局）、`world/level_dat.cpp`、`world/anvil.cpp::encode_chunk_merged`、`game/world_persistence.cpp`（载入保留 source NBT）；测试 `tests/test_world.cpp`。
- 互操作实测：cyane 加载原版世界 1576 区块；改块保存后交原版服务器重新加载零错误；重写区块保留 SkyLight/BlockLight/HeightMap/Biomes。

### R-012 — 区块 Entities 列表的实体格式（原版预言机实测）

- 来源：原版 1.12.2 世界 `r.0.0.mca` 内 89 个真实实体记录逐字段解析（村民/铁傀儡等）；cyane 保存含掉落物的世界后由原版服务器重载。
- 结论（与方块实体 TileEntities 的字段名**不同**）：
  - 实体类型字段是**小写 `id`**（如 `minecraft:villager`），不是方块实体那样的小写 `id` 之外的大写 `ID`；1.12.2 实体 id **带 `minecraft:` 命名空间前缀**。
  - 位置字段是 **`Pos`（TAG_List of TAG_Double×3）**，不是方块实体的 `x`/`y`/`z` 标量。`Motion` 同为 List×3。
  - 其余公共字段实测：`Health`(f32)、`Air`(i16=300)、`Fire`(i16)、`FallDistance`(f32)、`OnGround`/`Invulnerable`/`FallFlying`/`Leashed`/`CanPickUpLoot`(u8)、`PortalCooldown`/`HurtTime`/`DeathTime`/`HurtByTimestamp`/`Age`/`ForcedAge`/`Riches`/`Dimension`(i32)、`UUIDMost`/`UUIDLeast`(i64)、`Rotation`(List<f32>×2)。
  - 掉落物实体（`id="minecraft:item"`）带 `Item`{`id`(i16),`Count`(i8),`Damage`(i16)}。
  - 原版读取宽容：缺失字段取默认（`getTagList("Pos")` 越界返回 0），因此我们写最小集（id/Pos/Motion/Health/Age/Item）即可互通；重写实体时未建模字段（UUID 等）由原版重新生成。
  - **教训**：最初实现误用方块实体的 `ID`+`x/y/z` 字段名写实体——那种记录原版读到的位置恒为 (0,0,0)。由本预言机实测纠正为 `id`+`Pos`。
- 落地：`world/anvil.cpp`（`item_entity` 编码 / `read_item_entity` 解码 / merged 透传过滤）、`net/item_drop`（restore/all_drops）、`game/world_persistence`（载入回填掉落物、保存按 chunk 归位）。
- 互操作实测：cyane 载入原版世界（633 区块，含 83 生物 + 6 掉落物实体）→ 保存 → 原版重载 `Done (4.317s)` 零错误；掉落物实体往返字段完整。

### R-013 — 区块 section 光照数组为原版读取的必需字段（真机预言机实测）

- 来源：纯 cyane 生成（无原版存档底档）的世界交原版 1.12.2 服务器重载，日志 10 处 `Couldn't load chunk: ChunkNibbleArrays should be 2048 bytes not: 0`（NibbleArray 构造）。
- 结论：
  - 原版 1.12.2 读取端对**每个非空 section 无条件**构造 `BlockLight`/`SkyLight` 的 NibbleArray（`getByteArray` 对缺失键返回空数组 → 抛异常），**没有** hasKey 保护。
  - 此前未暴露：历次 oracle 都基于原版世界走 merged 保存（保留原光照）；只有全新编码的 section（`encode_chunk` / merged 追加节）会缺这两个键。
  - 修复：新编码 section 补 2048 字节全 0 占位；`LightPopulated` 保持缺省 0，原版加载后自行重算天光（该标志即为此流程设计）。
- 落地：`world/anvil.cpp`（`build_section_arrays` 增光照数组、`encode_chunk` 主循环与 merged 追加分支补 `BlockLight`/`SkyLight`）。
- 互操作实测：纯 cyane 超平坦世界（含 12 只落盘生物）交原版重载 `Done (5.268s)` 零区块错误。

### R-014 — 实体幽灵副本：跨区块移动/移除后的磁盘残留

- 来源：生物落盘真机验证——第一轮保存 12 只，第二轮保存后磁盘出现 14 个实体条目（多 1 牛 1 鸡）。
- 结论：生物在两次保存间漫游跨区块时，新位置区块被重写而旧位置区块不再满足写出条件（不脏、无方块实体）→ 磁盘残留旧副本，下次载入重复。掉落物被拾取后同理（位置不变但内容消失，所在区块可能不再被重写）。
- 修复：`WorldPersistence` 维护 `drop_chunks_`/`mob_chunks_`（载入时从磁盘发现、保存时从内存态归组双向更新），保存时无条件并入写集——重写为内存态（可能为空）以清除幽灵。
- 实测：三轮启动-漫游-保存循环后实体总数恒为 12。
