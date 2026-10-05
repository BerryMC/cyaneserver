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

### R-015 — 区块释放后 materialize 假区块覆盖真实地形（数据丢失事故复盘）

- 现象：玩家报"右键打不开箱子"；磁盘审计发现世界长期累积"单 section 超平坦区块"（历史运行退化 93 次 + 一次新增 4 个），箱子所在区块地形被抹平，服务端内存里已无该方块。
- 根因链（两层）：
  1. **保存覆盖**：区块被视距释放出内存后，其箱子/熔炉记录仍在容器存储 → 保存时命中写集 → `world_.chunk_at(pos)` 把缺块物化为超平坦 baseline → `encode_chunk` 以 1-section 超平坦覆盖原版地形（原版地形/建筑/方块实体全部丢失）。
  2. **回归即超平坦**：释放的区块玩家回来时走 `ensure_locked` 物化超平坦而不是从磁盘重载——远处的原版地形在会话中直接变平。
- 修复：
  - `World::contains` 区分真区块与物化假区块；保存对不在内存的写集区块改走 `encode_chunk_entities_only`（以 region 缓存的磁盘原 NBT 为底只合并 TileEntities/Entities，方块数据原样保留）。
  - `World::set_loader`：区块缺失时经 `WorldPersistence::attach_loader` 注入的回调从 region 缓存按需重载真实地形并恢复方块实体（容器/熔炉以内存态优先，不覆盖运行时改动）；失败才退回物化超平坦。
  - `regions_` 缓存加 `cache_mutex_`（loader 在 reactor 线程、save 在 tick/停机线程并发）——注意非重入锁：锁内用 `region_locked`，勿调用会加锁的 `region()`（本次曾因此同线程自锁挂死测试）。
- 回归实测：vanilla 生成 625 区块真实地形 → cyane 载入 → 两个玩家进出（触发释放/回载）→ 保存 → 全部区块保持多 section、零退化。
- 教训：**`chunk_at` 的隐式物化语义是保存路径的陷阱**——任何"缺块即造"的兜底都不该出现在写盘路径上；另运维脚本对世界目录先验证拷贝成功再删原文件。

### R-016 — 1.12.2 死亡重生序列（probe 实测对齐）

- 来源：probe 登录 → 跳虚空（PlayerPosition y<-64 触发服务端击杀）→ ClientCommand(0) 重生 → 逐包记录。
- 结论：
  - **sb ClientCommand = 0x03**（0x04 是 Client Settings；respawn 动作 varint 0），实测 0x03 触发、0x04 无效。
  - 服务端 respawn 序列**不应重发 JoinGame**：实测序列为 `Respawn(0x35) → PlayerAbilities(0x2C) → UpdateHealth(0x41) → SpawnPosition(0x46) → TimeUpdate(0x47) → HeldItemChange(0x3A) → 区块流 → PlayerPosLook(0x2F)`。早期实现 Respawn 后重发 JoinGame，强制客户端重建世界并**卡死在"加载地形"**。
  - 同维度重生客户端**不清世界**：服务端不应补发实体（会双份）；死亡时向他人广播 EntityStatus(3) 死亡动画，重生时 broadcast_despawn + broadcast_spawn 同步他人视角。
  - 重生点区块可能已被客户端 UnloadChunk：服务端清空 loaded_chunks_ 重新下发（重复 ChunkData 客户端就地覆盖，无害）。
- 落地：`net/connection_combat.cpp::respawn_player/kill_player`。

### R-017 — EntityMetadata Slot 类型值（vanilla RCON 实测纠正）

- 来源：vanilla 1.12.2 服务器开 RCON，`summon Item` 后抓 EntityMetadata(0x3C) 字节：掉落物条目实测 `idx=06 type=05 <Slot 6字节> ff`。
- 结论：1.9–1.12.2 的 metadata 类型表 **Item(Slot)=5、Boolean=6**（Cuberite `Protocol_1_9.h` eMetadataType 同值）。type=6 是 1.13+ 才成立。
- **教训**：上一轮“修掉落物不可见”把 type 从 5 改成 6 是**基于错误记忆的猜测**，方向反了——真正的不可见另有其因（仍在排查），不该在没抓包时乱动已对的值。现已 RCON 实测回退为 5，并以黄金向量 `packet_item_entity_metadata_matches_vanilla` 锁定。

### R-018 — 新玩家出生点落到原点虚空（卡加载/悬空根因）

- 现象：玩家"进不去"（卡加载地形）、"复活点很高"。probe 抓包：SpawnPosition 正确为 (-28,64,244)，但真正的 PlayerPositionLook 落在 (0.5,4,0.5) 原点。
- 根因：`PlayerData` 默认位置硬编码 (0.5,4,0.5)；登录流程先 `player_pos_ = spawn_point()`（已含地表探测），随后 `load_player_data()` 用 `load_or_default` 的默认值**无条件覆盖**，新玩家（无 .dat）被挪回原点。而当前世界原点区块是空的（void），玩家落进虚空 → 客户端表现为卡加载/悬空。
- "很高"则是另一面：surface_y 之前的旧版本直接用 level.dat 的 SpawnY=64，而该世界地表在 y~15，玩家从 y64 自由下落。
- 修复：`load_or_default` 增 spawn_x/y/z 参数作新玩家默认位置；连接层传入 `spawn_point()` 预置的 `player_pos_`（含逐列地表探测）。有 .dat 时仍读存档 Pos 覆盖。
- 测试：`new_player_defaults_to_world_spawn_not_origin`；probe 实测新玩家落在 (-15.5,4,246.5)，所在区块在已发送集合内。

### R-019 — 采集等级与交互方块（Cuberite 对照）

- 来源：Cuberite `Items/ItemPickaxe.h`（CanHarvestBlock 分级表）、`BlockInfo.cpp`（GetHardness）、`Items/ItemShovel.h`（速度倍率）。
- 结论（1.12.2）：
  - 镐等级：wood/gold=1、stone=2、iron=3、diamond=4；金镐能力等同木镐。
  - 采集门控：黑曜石→4；钻石/金/绿宝石矿及块、红石矿→3；铁/青金矿及块→2；石头系/煤矿/砂石/砖/熔炉等→1。不满足时**方块仍被破坏但不掉落**。
  - 镐/斧/锹速度倍率：wood=2、stone=4、iron=6、diamond=8、gold=12（锹对泥土/沙/砾石类，斧对木类，镐对石/金属类）。
  - 交互方块 meta 开关位：拉杆(69) 0x8；活板门(96)/栅栏门(107) 0x4；木门(64) 0x4（上下半同翻）。
- 落地：`item/item_tools.hpp`（工具识别/等级/速度）、`world/block_drops.hpp::harvest_rule + block_drops(state, tool)`、`connection_world.cpp`（右键切换方块 + 挖掘传入手持工具）。

### R-020 — 交互方块音效（vanilla jar 对照修正）

- 来源：`spigot-1.12.2` 反编译 `BlockButtonAbstract`/`BlockStoneButton`/`BlockWoodButton`/`BlockLever`/`BlockDoor`/`BlockTrapdoor`/`BlockFenceGate`；`javap` 读 `SoundEffect`/`SoundEffects`/`PacketPlayOutNamedSoundEffect`/`World`/`SoundCategory`；`vanilla/{server,client}.jar` 的音效注册序对照。
- 结论（1.12.2）：
  - **Named Sound Effect (0x49) 的首字段是 SoundEffect 注册表 id（VarInt），不是音效名字**：`PacketPlayOutNamedSoundEffect.a/b` 走 `SoundEffect.a` 的 id↔对象映射。按名字发（把长度前缀当 id、名字首字节当类别序数）会让客户端抛 `ArrayIndexOutOfBoundsException` 断连——实测按下按钮报的 `98` 正是 `'b'`。
  - 注册表 id 由 `SoundEvent.b()` 按名字顺序从 0 分配；客户端 `qe/qf` 与服务端 `SoundEffect` 两侧的序列**逐条一致（实测 549 条）**。本项目所需 id 抄自该序列，落在 `tests/fixtures/sound_registry_ids.txt`。
  - 位置字段 = `(方块坐标 + 0.5) × 8`：`World.a(EntityHuman, BlockPosition, ...)` 先把方块坐标加 0.5，`PacketPlayOutNamedSoundEffect` 构造再 ×8 —— 即 `x*8+4`（不是 `x*8+8`）。
  - 按钮与拉杆走 0x49：`SoundCategory.BLOCKS`（枚举序 **4**，序 0 是 MASTER），音量 **0.3F**。按钮按下 = `block.stone_button.click_on` / `block.wood_button.click_on`（音高 0.6），回弹 = 对应 `click_off`（音高 0.5）；拉杆始终 `block.lever.click`，音高开 0.6 / 关 0.5。
  - 按钮音效**只有 `click_on`/`click_off` 两个变体，没有裸 `.click`**；木按钮是 `block.wood_button.*`（不是 `wooden_button`）。
  - 门/活板门/栅栏门在 vanilla 走 **World Event (0x25)** 数字 id（`BlockDoor.e()/g()`、`BlockTrapdoor.a()`、`BlockFenceGate` 内的 1005–1014/1036/1037 常量），而非命名音效。
  - **客户端预测决定要不要回发给操作者**：按下按钮（`BlockButtonAbstract.a`）、开关门/活板门/栅栏门都把自己（`entityhuman`）传给 `World.a`，`PlayerList.sendPacketNearby` 里 `if_acmpeq` 跳过该玩家——因为客户端已本地播放，服务端再发就是双响；按钮**回弹**（`BlockButtonAbstract.b`）与拉杆传 `null`，所有人都收得到。实测印证：按下按钮会双响、回弹无声（回弹没有任何本地预测）。
- 落地：`world/block_sounds.hpp`（方向 → 注册表 id/名字/音量/音高/广播半径/是否本地预测表）、`net/packet_writers.hpp::write_named_sound`、`proto/play_fields.hpp::sound_category::kBlocks`、`connection_world.cpp::send_block_sound`（统一 0x49，预测音效不回发操作者、按原版半径广播附近玩家：按钮/拉杆 16 格、门类 64 格）、`connection.cpp::tick`（按钮到期回弹时补发回弹音）。门类用 0x49 的 `open`/`close` 而非 0x25 数字 id：听觉等价，尚未做字节级对齐。
- 测试：`packet_named_sound_encodes_registry_id_and_block_center`（黄金向量，含多字节 varint id）、`toggle_sound_matches_vanilla_registry`（表内 id 与 fixture 注册表逐条对齐）。

### R-021 — 门物品映射、按钮回弹与附着方块掉落

- 来源：`spigot-1.12.2` 的 `Block`/`Item` 静态注册表（id + 名字机械提取）、反编译 `BlockButtonAbstract`/`ItemDoor`、`PlayerList.sendPacketNearby` 字节码。
- 结论（1.12.2）：
  - **门是纯物品，item id ≠ block id**：324 橡木门 / 330 铁门 / 427–431 云杉·白桦·丛林·金合欢·深色橡木门 → 方块 64 / 71 / 193–197。同一份提取里 54=箱子、61=熔炉、77/143=按钮、154=漏斗 与项目既有常量完全一致（交叉验证）。未映射时服务端把门当"非方块物品"忽略放置，客户端本地预测画出的门在重进（重收区块）后消失——即"放门→退出服务器→重进→门消失"。
  - 附着方块（按钮/拉杆）的支撑：`BlockButtonAbstract.a` 用 `pos.shift(FACING.opposite())` 取支撑格，支撑没了就 `dropBlock + setAir`；meta 低 3 位 = FACING（`fromLegacyData`：0=下 1=东 2=西 3=南 4=北 5=上）。该掉落走方块自身的 dropBlock，**不受创造模式影响**（与玩家挖方块不同）。
  - 已按下（POWERED）的按钮再次右键：`interact` 直接 `return true`，不自作回弹。
  - 方块 id 表（`Block` 静态注册）：木门 64、铁门 71、云杉/白桦/丛林/金合欢/深色橡木门 193–197。
- 落地：`world/blocks.hpp`（`door_block_from_item` + `block_state_from_item` 扩门物品、`is_wooden_door`/`is_door`/`is_button`/`is_lever`/`support_delta`/`supported_by`）、`world/block_drops.hpp`（门掉落门物品）、`net/block_ticks.{hpp,cpp}`（**世界级**按钮回弹调度，状态不在连接上，按下者断线也照常弹）、`connection_world.cpp`（放置/切换/破坏统一 door/button 判定；`break_unsupported_neighbors` 连带破坏+掉落）。
- 测试：`door_items_map_to_door_blocks`、`door_drops_are_door_items`、`attached_block_support_directions`、`button_release_is_world_level`；探针活体验证：门物品放置出双半块、按下→（再次右键被忽略）→回弹音 id 109/0.3/0.5、断线后仍回弹、支撑破坏后按钮消失并掉出物品实体。
- 旁注：`PlayerList.sendPacketNearby` 按玩家**当前坐标**的区块距离过滤广播目标，快照位置没更新的客户端收不到广播（探针自查时踩过）。

### R-022 — 生物物种、刷怪蛋、实体物理参数（vanilla jar 对照）

- 来源：`spigot-1.12.2` 的 `EntityTypes`/`Item` 静态注册（机械提取 id）、反编译各 `Entity*` 构造器与 `initAttributes()`、`EntitySkeletonAbstract`（射箭）、`EntityCreeper`（引信）、`PacketPlayOutSpawnEntityLiving`/`PacketPlayOutSpawnEntity`/`EntityTrackerEntry`（包格式与 object id）、vanilla server.jar 的 `assets/minecraft/loot_tables/entities/*.json`。
- 结论（1.12.2）：
  - **实体类型 id**（`EntityTypes` 注册自增）：苦力怕 50、骷髅 51、蜘蛛 52、僵尸 54；猪 90、羊 91、牛 92、鸡 93（与项目既有常量吻合）。SpawnMob(0x03) 用该 id；刷怪蛋 item **383** 的 damage 也是这个 id。
  - **尺寸/血量/速度/攻击**（`setSize`/`initAttributes`）：僵尸 0.6×1.95、20 血、速度 0.23、近战 3.0、FOLLOW_RANGE 35；苦力怕 0.6×1.7、20 血、0.25、引信 30 tick、爆炸半径 3；骷髅 0.6×1.99、20 血、0.25（AttributeModifier 注入 0.3 概率加速，忽略）、射箭初速 1.6/间隔 20 tick/射程 15；蜘蛛 1.4×0.9、16 血、0.3、近战 2；猪 0.9×0.9、10 血；羊 0.9×1.3、8 血；牛 0.9×1.4、10 血；鸡 0.4×0.7、4 血。
  - **掉落表**（loot_tables/entities）：僵尸 腐肉367×0–2 + 铁265/胡萝卜391/土豆392 各 2.5%；骷髅 箭262×0–2 + 骨352×0–2；苦力怕 火药289×0–2；蜘蛛 线287×0–2 + 蜘蛛眼375×0–1（仅玩家/驯服狼击杀，本服务端简化为无条件）；猪 排319×1–3；羊 羊肉423×1–2 + 毛方块35×1；牛 皮革334×0–2 + 牛肉363×1–3；鸡 羽毛288×0–2 + 生鸡肉365×1。
  - **SpawnMob 0x03 字段序**（`PacketPlayOutSpawnEntityLiving.b`）：varint id | uuid(16) | varint type | f64 x/y/z | byte yaw/pitch/head | short vx/vy/vz | metadata（0xFF 终止）——与项目既有编码一致（黄金向量锁定）。
  - **箭矢**：客户端 `SpawnObject` object id **60**（`EntityTrackerEntry` 对 `EntityArrow`/`EntityTippedArrow`/`EntitySpectralArrow` 均传 60），ObjectData = **射手实体 id**，速度 f=(mot×8000) clamp ±3.9；命中音 `entity.arrow.hit`(137)/`hit_player`(138)，射击音 `entity.arrow.shoot`(139)/`entity.skeleton.shoot`(407)。
  - 音效 id（按 R-020 注册表序）：僵尸 485/484、骷髅 406/405、苦力怕 172/171、primed 173、蜘蛛 431/430、猪 354/353、羊 387/386、牛 168/167、鸡 164/162、玩家 367/366、爆炸 231、通用 233。
  - **Explosion (0x1C) 布局（PacketPlayOutExplosion.b）**：位置 **f32×3**（字段虽声明 double 但 writeFloat 写出！）、半径 f32、记录数 i32、每条记录 byte×3（受影响方块相对爆心偏移）、玩家动量 f32×3。位置按 f64 写会让客户端把半径/记录数读串位——记录数为垃圾值时客户端按其分配内存直接 OOM（线上事故：打苦力怕两下即 DecoderException: OutOfMemoryError，因靠近触发引信后爆炸）。
  - **苦力怕引信表现**：`EntityCreeper` metadata index **16**（VarInt，DataWatcherRegistry 序 1）：-1=空闲、1=引信中（客户端播白闪膨胀动画）；点燃音 `entity.creeper.primed`(173) 在引信开始时播；引信 30 tick 内不寻路；引信期间玩家跑开**不取消**爆炸（vanilla 无 defuse）。
  - **刷怪蛋的实体类型在 EntityTag NBT 里，damage=0**：创造栏蛋由 `ItemMonsterEgg.a(CreativeModeTab,…)` 生成——只写 `EntityTag:{id:"minecraft:…"}`，`h(ItemStack)` 也只从 NBT 读实体（蛋壳斑点色同样查 NBT）。因此真实客户端拿蛋发的 CreativeInventoryAction 携带完整 NBT；本项目 slot 解码曾对任何 NBT 返回 nullopt → 断连（线上表现：一拿刷怪蛋即 "connection terminated" + 服务端 `read error`）。修复：slot 解码用 `nbt::parse_with_size` 解析并跳过 NBT（tag 字节只窥视不消耗，解析器需要完整树），`EntityTag.id` 经物种表翻译成 damage；写出时按 damage 反向重建 EntityTag，保证蛋壳颜色与原版一致。
- 落地：`world/mob_types.hpp`（物种表 + nbt↔type + 刷怪蛋映射）、`world/physics.hpp`（AABB 逐轴推进 + 上台阶 + 落地贴合；`blocks.hpp::is_solid` 非固体排除表）、`net/mob_manager.{hpp,cpp}`（血量/物理/AI 状态机 + 事件化 `tick(World, players)`；实体 id 统一走全局分配器，废除 1000+ 独立空间）、`net/packet_writers.hpp::write_spawn_mob`、连接侧刷怪蛋（点方块/对空两条路径）、`anvil.cpp` 改物种表（修"未知物种静默存成猪"）、`StoredMob`/`MobState` 增 `Health` 往返。
- 测试：`tests/test_mobs.cpp`（物种表/刷怪蛋/碰撞表/落地/撞墙与上台阶/追击攻击/逃窜/受伤死亡/实体 id 同源/SpawnMob 黄金向量）。
- 战斗闭环补充（提交 2/3）：玩家攻击走 UseEntity type=1（挥臂 Animation 0x06 + item::attack_damage 表）；生物→玩家伤害经 hub 邮箱新 damage 通道（扣血 + UpdateHealth + EntityStatus(2) + EntityVelocity 击退 + `entity.player.hurt` 367），创造模式免疫；苦力怕引信 30 tick（期间不寻路）→ 爆炸：`entity.generic.explode` 231 + Explosion(0x1C, record=0) + 半径 3 球内方块破坏（30% 掉落）+ 距离衰减伤害（≤2×power，峰值 power×7/格）；骷髅 15 格射程内每 40 tick 射箭，箭 = SpawnObject(60, data=射手id) + 初速 1.6 + 每格 0.05 抬升弹道补偿 + 每 tick 重力 -0.05，命中方块/玩家/生物即销毁（骷髅箭不可拾取，vanilla 同）。

### R-023 — 生物 AI/爆炸公式按原版反编译逐条转写（修正 R-022 的自造公式）

- 来源：`spigot-1.12.2` 反编译 `Explosion`/`EntityCreeper`/`PathfinderGoalSwell`/`PathfinderGoalMeleeAttack`/`PathfinderGoalBowShoot`/`EntitySkeletonAbstract`/`PathfinderGoalPanic`/`PathfinderGoalRandomStroll`/`PathfinderGoalNearestAttackableTarget`/`PathfinderGoalHurtByTarget`/`EntityLiving`（击退 `a(Entity,float,double,double)`）/`EntityZombie`/`EntitySpider`；Cuberite `src/Physics/Explodinator.cpp`（vanilla Explosion 的 C++ 转写，含爆炸抗性表与曝光采样）。
- 结论（1.12.2，逐条对齐后落地）：
  - **爆炸方块破坏是射线追踪，不是球形判据**：16³ 立方体表面 1352 条射线，强度 `power*(0.7+rand*0.6)`，步长 0.3，每遇非空气方块衰减 `(抗性+0.3)*0.3`，每步再衰减 0.225；被毁方块的掉落率 = `1/power`（yield）。抗性表取 Cuberite `GetExplosionAbsorption`（wiki 抗性 ×0.3 系数合成值），落地为 `blocks.hpp::explosion_absorption`（基岩 1080000.09、黑曜石/铁砧 360.09、水/岩浆 30.09、石头类 1.89、木制品 0.99、默认 0.09）。此前"半径=power 的球 + 30% 掉落"作废。
  - **爆炸实体伤害含"曝光率"**：`impact = (1 - dist/(power*2)) * 曝光率`，曝光率 = 包围盒按 0.5 采样后对爆心视线无遮挡的比例；`伤害 = floor((impact²+impact)/2 * 7 * power*2 + 1)`；击退 = 归一方向 × impact 直接加到速度。此前"线性衰减 power*7"作废。
  - **爆炸音效**：volume 4.0、pitch `(1+(r-r)*0.2)*0.7`（此前 1.0/1.0）。
  - **苦力怕引信可熄灭**（**修正 R-022 的"跑开不取消"结论，那条是错的**）：`PathfinderGoalSwell.e()` 每 tick 设状态——目标存在且 distSq≤49 且可见 → 1，否则 −1；`EntityCreeper.B_()` 里 `fuseTicks += 状态`（可负向回退，clamp ≥0），引信到 30 引爆（radius 3、无火、mobGriefing 门控）。metadata 16 = 状态（−1/1），状态翻转时都要广播（熄灭不发声音）。点燃音在"状态>0 且 fuse==0"的时刻播。
  - **近战触发距离**：`PathfinderGoalMeleeAttack.a()`——distSq（3D，到脚底）≤ `(2×width)² + 目标宽度`，冷却 20 tick。此前固定 1.2 格作废。
  - **击退**：`EntityLiving.a(Entity, 0.4F, d0, d1)`——现有水平动量减半后按 方向×0.4 反向叠加，落地时竖直动量减半 +0.4（上限 0.4）；d0/d1 = 攻击者−受击者。此前"直接 += 0.4"作废。爆炸对生物用同一通道，强度传 impact。
  - **逃窜**：`PathfinderGoalPanic(this, 2.0)`——2 倍导航速度（猪 0.25→0.5 b/t，比疾跑快，vanilla 就是这样），受击后 100 tick。此前 1.1×/60–100 tick 作废。
  - **漫游**：`RandomStroll` 倍率按物种：苦力怕 0.8、蜘蛛 0.8（`r()` 构造参数），僵尸/骷髅 1.0 → `MobType.stroll_scale`。
  - **骷髅射箭**：`PathfinderGoalBowShoot(this, 1.0, 20, 15.0F)`，但 `dm()` 在非困难难度把射击间隔改成 **40 tick**；射程 15（超出则接近，不再后退）。瞄准公式 `EntitySkeletonAbstract.a()`：箭起点 = 骷髅位 + 头高 1.74，`d3` = 水平距离，方向 = `(dx, (目标y+length/3−箭y) + d3*0.2, dz)` 归一化 × 1.6，再各轴叠加 `nextGaussian*0.0075*(14−难度*4)`（普通 = 6）。此前"每格 0.05 抬升补偿"作废。
  - **目标选择**：`NearestAttackableTarget` 搜索盒 = 包围盒 grow(follow_range, 4.0 竖直, follow_range)；`HurtByTarget` 拉仇恨 300 tick。僵尸 FOLLOW_RANGE 35，其余默认 16。
- 落地：`world/blocks.hpp::explosion_absorption`、`world/mob_types.hpp`（`stroll_scale`）、`net/mob_manager.{hpp,cpp}`（`fuse_state`/`fuse_ticks` 双字段 + `MobIgnition.fuse_state`、`damage()` 击退参数、近战/逃窜/漫游/射击公式）、`game/server.cpp`（`apply_explosion` 射线追踪+曝光伤害、`fire_arrow` 原版弹道、swell 状态广播）。
- 测试：`explosion_absorption_matches_reference`、`creeper_fuse_defuses_when_target_flees`（熄灭：state −1 事件 + 引信回 0 + 不爆炸）、`retreat_speed_is_below_sprint`（改断言 0.5 b/t + 100 tick）、`creeper_fuse_then_explodes`（fuse_state==1 判定）。160 测试 × 3 配置（常规/-Werror/ASan+UBSan）全绿。
- 实机联调补充（真实客户端复测发现的三件事）：
  - **客户端只在 SpawnMob 出生包里注册 watcher 条目**：出生 metadata 为空（0xFF）时，之后对 index 16 的增量 EntityMetadata 会被整个忽略——苦力怕引信白闪不显示的根因。出生包必须带初始字段（苦力怕：index 16、VarInt 序 1、值 -1）；引信音 `entity.creeper.primed` 音高 0.5（`this.a(ay, 1.0F, 0.5F)`）。
  - **骷髅主手的弓走 EntityEquipment (0x3F)**：varint id | varint slot 0（主手）| slot；spawn 与补发（send_existing_mobs）都要发，否则客户端看空手。
  - **弹坑会"卡死"AI 的联动态**：目标搜索盒是 `grow(follow_range, 4.0, follow_range)`（竖直 ±4）——骷髅蛋生成在苦力怕弹坑底（y=1）而玩家在坑口（y≥4）时 dy=7 超出，索敌失败后 ai 回 idle 但 target_player 残留，表现为"完全不正常、不射箭"。vanilla 同样如此，不是公式错误；根因是前一发的弹坑地形。
- 落地补充：`packet_writers.hpp::write_spawn_mob` 增出生 metadata 参数、`connection_mobs.cpp`（苦力怕出生 metadata + 骷髅弓装备两处）、`server.cpp` primed 音高 0.5。
- 真实客户端联调修正（Forge 1.12.2 反混淆源码 ~/code/ForgeDevEnv-master 核实）：
  - **metadata 索引必须按 defineId 的父类链计数推**：`DataWatcher.a(Class, Serializer)` 把索引分配为该类及其所有父类已定义 id 的累计数——Entity 6 个(0-5) + EntityLivingBase 5 个(6-10，HAND_STATES/POTION/HIDE_PARTICLES/ARROW_COUNT/HEALTH) + EntityLiving AI_FLAGS(11)，故 **EntityCreeper.STATE = 12**（此前 R-022 写的 16 是错的，客户端据此忽略白闪）、**AbstractSkeleton.SWINGING_ARMS = 12**（Boolean 序 7，举弓蓄力动画）、EntityItem.ITEM = 6（与实测一致）。苦力怕 POWERED=13、IGNITED=14。
  - **客户端 Respawn 包（同维度也）执行 `world.removeAllEntities()` 并重建全新背包**（`Minecraft.setDimensionAndSpawnPlayer`）——重生后服务端必须重发：物品栏 WindowItems + 玩家（register_in_hub 已含 spawn_existing_players）+ 掉落物 send_existing_drops + 生物 send_existing_mobs（含骷髅弓装备）。此前"不补发实体"的注释是错的，正是"重生后物品/掉落物在客户端不可见但服务端存在"的根因。
  - **骷髅瞄准行为**：BowShoot 瞄准期侧移走位（每 20 tick 0.3 概率翻转方向、0.5 倍速侧移）+ 释放前 20 tick 举弓（SWINGING_ARMS 置位）；箭本身按 SpawnObject 速度由客户端模拟飞行（velocity = short/8000，data≠0 才读）。
- 落地补充：`mob_manager`（bow_drawing/strafe 字段 + MobDraw 事件）、`server.cpp`（draws → 索引 12 Boolean metadata；ignition 索引 16→12）、`connection_mobs.cpp`（出生 metadata index 12）、`connection_combat.cpp::respawn_player`（重发物品栏/掉落物/生物）。
- **Forge 客户端的 metadata 序列化器 id 与原版不同（线上断连根因）**：Forge 1.12.2 的 `DataSerializers` 静态注册序为 BYTE(0) VARINT(1) FLOAT(2) STRING(3) TEXT_COMPONENT(4) **ITEM_STACK(5) BOOLEAN(6)** ROTATIONS(7)…（原版把 OPTIONAL_BLOCK_STATE 插在 6，BOOLEAN=7）。本项目此前按原版发 BOOLEAN=7，Forge 客户端把它当 ROTATIONS（读 3×f32=12 字节）——骷髅举弓 metadata（SpawnMob 55 字节+pid=56）恰好被读穿，报 `readerIndex(54)+length(4) exceeds writerIndex(56)`。修：Boolean 元数据一律发 **6**；ItemStack=5 与 VarInt=1 两边恰好一致。注意：这组 id 面向 Forge 客户端（用户环境 1.12.2-Forge-OptiFine），若将来支持原版客户端需要按客户端类型区分。
- 验证：探针线包抓到 `index=12, ser=6, value=1/0` 的举弓序列；160 测试 × 3 配置全绿。
- 骷髅节奏对齐 EntityAIAttackRangedBow（Forge 反混淆源码逐行核对）：**蓄力 20 tick 才放首箭**（此前索敌当帧即射），释放后 attackTime=40 → 完整周期 60 tick；**seeTime ≥ 20 才停步走位**（此前立即走位）；走位为两轴 ±0.5×速度（`strafe(forward, strafe)`：distSq > (0.75×射程)² 前进、< (0.25×射程)² 后退，两轴各 20 tick 0.3 概率翻转）；放箭无距离门（原版 release 只看 canSee）。参数核实：箭伤 = `ceil(命中速度 × 2.0)`（EntityArrow.onHit，速度 1.6 → 近距 ≈ 4 伤害，本服务端固定 2.0 反而更温和）、追击速度 0.25×1.0、箭速 1.6、射击间隔 40（非困难）——均与原版一致。
- **移动模型改 vanilla travel 动量链（速度错误的最终修正）**：`EntityLiving.setAIMoveSpeed(speed)` 会**同时**设 landMovementFactor（travel 的摩擦参数）和 moveForward（输入）→ `moveRelative` 加速度 = aim×aim（aim = 导航倍率×移动速度属性）；每 tick 先摩擦（地面 slipperiness 0.6×0.91 = 0.546、空中 0.91）再加加速度再位移，稳态位移 = 加速度/(1−摩擦) ≈ 2.2×aim²。数值：追击（1.0）骷髅/苦力怕 0.138 格/t ≈ 2.8 m/s、僵尸 0.117 ≈ 2.3 m/s；漫游（0.8）≈ 1.8 m/s；走位每轴加速度 = 0.5×(0.5×属性)（EntityMoveHelper STRAFE）；Panic 2.0 倍率 → 猪 0.55 格/t（原版逃窜确实短暂快过疾跑 0.28）。玩家疾跑 5.6 m/s 可甩开所有常规生物——与原版体感一致。此前"每 tick 直接位移 0.25"比原版快约一倍。
- 实机复核三修：① **EntityAIPanic 倍率按物种**（Forge 源码：猪 1.25、羊 1.25、牛 2.0、鸡 1.4）→ `MobType.panic_scale`，此前统一 2.0 使猪被打后以 ~11 m/s 狂奔 5 秒（击退"一大段"的主因）；② **摩擦移回位移之后**（travel 的 f6 在 move 后乘、用当拍落地状态），击退滑行明显缩短；③ **实体隐身根因**：客户端 `World.spawnEntity` 要求区块已加载，失败进 `entitySpawnQueue` 每 tick 只重试 10 个——生成包先于区块到达、或区块在队列排干前被卸载时实体**永久不可见**（骷髅/掉落物"有时看不见"）。修法同 Cuberite cChunk::AddClient：**每次向客户端发完 ChunkData 就补发该区块内的生物（含骷髅弓）与掉落物**（`send_chunk_entities`，挂在 send_pending_chunks 后），重复 SpawnMob 客户端安全（addEntityToWorld 先 remove 旧的）。走位后退（面向目标倒着走）是原版 kiting 行为，保留。
- 复核反馈逐条落实（Forge 反混淆源码为准）：
  - **#1/#3 复核为误读，实现不变**：`getItemInUseMaxCount() = 72000 − activeItemStackUseCount`（每 tick 递减）→ `i ≥ 20` 是真实 20 tick 蓄力门；`attackTime` 初始 −1 首箭立即举弓，释放后 =40 → **60 tick 周期即原版**。移动增量：`EntityLiving.setAIMoveSpeed` 同时设 landMovementFactor 与 moveForward（同一值）→ 增量 = aim×aim（f7 = 0.16277136/0.546³ ≈ 1.0），终速 ≈ 2.2×aim²。
  - **#2 丢目标重置**：target 变更/超 60 tick 记忆丢弃时收弓（bow_drawing=false + metadata 事件）、see_ticks 归零（对应 resetTask）。
  - **#4 摩擦时序与撞墙**：f6 改取 **move 之前**的落地状态（travel 源码序）；撞墙只清被挡轴（blocked_x/blocked_z 分列），沿墙滑动保留侧向动量。
  - **#5 目标记忆与视线**：`EntityAITarget.unseenMemoryTicks = 60`（扫描选不到目标时保留 60 tick 再丢弃）；近战挥击与骷髅 seeTime/放箭/收弓（seeTime < −60）全部走 `hasLineOfSight` 视线门（0.5 步进实心方块遮挡采样）；苦力怕 swell 状态同样要求可见（`EntitySenses.a`）。
  - **#6 漫游/恐慌**：`RandomStroll` 每 tick 1/120 概率选 10 格内随机落点（无生成点锚定，走到即止）；`EntityAIPanic` 逃向随机落点（findRandomTarget(5,4) 近似），到位或 100 tick 结束。删除 home 锚定与 kWanderRadius。
  - **#7 护甲减伤**：`ArmorUtil.getDamageAfterAbsorb`（damage×(1 − clamp(armor − damage/(2+tough/4), armor×0.2, 20)/25)）落地于 `apply_damage`，护甲值按槽 5..8 累加（`item::armor_points` 皮革/锁链/铁/钻/金表）——近战/爆炸/箭伤全部生效。
  - #8（行为差异）记录在案未实现：蜘蛛 EntityAILeapAtTarget、僵尸破门/村庄、苦力怕坠落引信加速、生物间 HurtByTarget 反击。
- 重生实体分身/抽搐/消失根因与修法：respawn 与 join 的 `send_existing_mobs/drops` **全量补发**与 `send_chunk_entities` 的**按块补发**叠加——客户端 `addEntityToWorld` 对已存在 id 是"removeEntity（从 entityList 摘除）+ 重建"，而 WorldClient.removeEntity 不清理 entitySpawnQueue，滞留副本排干时再次 spawn 造成分身/抽搐；区块在其间被卸载则实体永久不可见。修：**实体只随区块发**（进服/重生都不再全量补发，物品栏照发），与 vanilla tracker/cuberite cChunk::AddClient 同构。探针验证两次重生各 11 只、id 唯一。
- 骷髅继续对齐源码的三处：`seeTime` 在可见性状态翻转时归零（`if (flag != flag1) seeTime = 0`）；面向目标/走位朝向走 `faceEntity(30,30)` 的 30°/tick 限速（近战/苦力怕同）；箭伤改 `ceil(命中速度 × 2.0)`（EntityArrow.onHit，满速 1.6 → 4 点，此前固定 2 偏低）。
- **生物不掉崖（用户指出的原版行为）**：vanilla 由寻路保证——`PathFinder`/`WalkNodeProcessor` 只扩展脚下有地面的可走节点，落差 >3 格（会摔伤）的路径不生成；`EntityMoveHelper` STRAFE 分支迈步前也有 `PathNodeType != WALKABLE` 检查。本服务端无寻路，以"崖边守卫"等价实现：每 tick 动量更新后、位移前，检查**本拍实际落点**（pos+velocity，不能用加速度——动量终端速度约为加速度 2 倍，检查点会冲过头）脚下 3 格内是否有地面，没有则取消本拍位移；漫游/恐慌放弃当前落点重新待机，敌对停在崖边（原版会绕路，待有寻路后替换）。
