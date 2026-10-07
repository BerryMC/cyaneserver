# 协议 340（Minecraft 1.12.2）实现与验证状态

依据事实优先级：**本机 jar 行为观测（Oracle 抓包） > Forge 反混淆源码 / Cuberite 源码 > minecraft.wiki**。

---

## 1. 核心线格式规则

| 规则 | 编码细节 |
|---|---|
| **VarInt / VarLong** | 7 位一组小端，MSB 为延续位。VarInt 最多 5 字节，VarLong 最多 10 字节。负数按 32/64 位补码展开。 |
| **String** | 前缀为**字节数**的 VarInt，其后为 UTF-8 字节。最大长度按协议常量校验（聊天 256/32767，标识符 32767）。 |
| **Position** | 64 位整型打包：`((x & 0x3FFFFFF) << 38) \| ((y & 0xFFF) << 26) \| (z & 0x3FFFFFF)`。 |
| **Angle Byte** | `degrees * 256.0 / 360.0 & 0xFF`，以 1/256 圈为单位的无符号单字节。 |
| **压缩帧** | `数据包长 VarInt \| 未压缩长 VarInt (0 表示未压缩) \| 数据`。阈值默认 256 字节，采用标准 zlib 格式。 |
| **加密传输** | AES-128-CFB8 流加密，IV 与 Key 均为客户端协商的 16 字节共享密钥。 |
| **Slot 格式** | `short id`。若 `id == -1` 表示空槽（占用 2 字节）；若非空，接 `byte count`、`short damage`、`TAG_End(0x00)` 或压缩 NBT 树。 |

---

## 2. 状态机与包覆盖表

### 2.1 Handshake (握手状态)
| 方向 | ID | 包名 | 字段布局 | 状态/验证 |
|---|---|---|---|---|
| SB | 0x00 | Handshake | `protocol VarInt \| host String \| port u16 \| nextState VarInt` | ✅ 原版对齐 |

### 2.2 Status (服务器列表状态)
| 方向 | ID | 包名 | 字段布局 | 状态/验证 |
|---|---|---|---|---|
| SB | 0x00 | StatusRequest | 空 | ✅ |
| CB | 0x00 | StatusResponse | `response JSON String` (键序: description → players → version) | ✅ 逐字节对照 |
| SB | 0x01 | PingRequest | `payload i64` | ✅ |
| CB | 0x01 | PongResponse | `payload i64` (原样回显) | ✅ 逐字节对照 |

### 2.3 Login (登录状态)
| 方向 | ID | 包名 | 字段布局 | 状态/验证 |
|---|---|---|---|---|
| SB | 0x00 | LoginStart | `username String(≤16)` | ✅ |
| CB | 0x00 | Disconnect | `reason JSON String` | ✅ |
| CB | 0x01 | EncryptionRequest | `serverId String(空) \| pubKey VarInt+bytes \| verifyToken VarInt+bytes(4)` | ✅ RSA-1024 SPKI DER (162字节) |
| SB | 0x01 | EncryptionResponse | `sharedSecret VarInt+bytes \| verifyToken VarInt+bytes` | ✅ RSA 解密通过 |
| CB | 0x02 | LoginSuccess | `uuid String \| username String` | ✅ 离线/在线双模 |
| CB | 0x03 | SetCompression | `threshold VarInt` (256) | ✅ 先于 LoginSuccess 发出 |

---

### 2.4 Play (游戏状态 — 服务端接收 Serverbound)

| ID | 包名 | 关键字段与用途 |
|---|---|---|
| 0x00 | TeleportConfirm | `teleportId VarInt`：确认传送已完成 |
| 0x02 | ChatMessage | `message String`：玩家聊天或以 `/` 开头的命令 |
| 0x03 | ClientStatus | `action VarInt`：0 = 请求重生（PERFORM_RESPAWN），1 = 请求统计 |
| 0x04 | ClientSettings | `locale String \| viewDistance byte \| chatMode VarInt \| colors bool \| displayedSkinParts u8 \| mainHand VarInt` |
| 0x05 | TabComplete | `text String \| assumeCommand bool \| hasPosition bool [\| pos i64]` |
| 0x07 | ClickWindow | `windowId byte \| slot short \| button byte \| actionNumber short \| mode VarInt \| clickedItem Slot` |
| 0x08 | CloseWindow | `windowId byte`：客户端关闭容器窗口 |
| 0x0A | UseEntity | `target VarInt \| type VarInt (0=interact, 1=attack, 2=interact_at) [\| targetX/Y/Z f32] [\| hand VarInt]` |
| 0x0B | KeepAlive | `keepAliveId i64`：响应服务端心跳 |
| 0x0C | Player | `onGround bool` |
| 0x0D | PlayerPosition | `x f64 \| y f64 \| z f64 \| onGround bool` |
| 0x0E | PlayerPositionAndLook | `x f64 \| y f64 \| z f64 \| yaw f32 \| pitch f32 \| onGround bool` |
| 0x0F | PlayerLook | `yaw f32 \| pitch f32 \| onGround bool` |
| 0x14 | PlayerDigging | `status VarInt \| position i64 \| face byte`<br>• 0/1/2: 挖掘开始/取消/完成<br>• 3: Ctrl+Q 丢弃整堆 (`DROP_ALL_ITEMS`)<br>• 4: Q 丢弃单件 (`DROP_ITEM`) |
| 0x15 | EntityAction | `entityId VarInt \| actionId VarInt \| jumpBoost VarInt` (潜行/疾跑切换) |
| 0x1A | HeldItemChange | `slot short (0..8)`：切换快捷栏选择槽位 |
| 0x1B | CreativeInventoryAction | `slot short \| item Slot`：创造模式直接放置/拿取物品 |
| 0x1F | PlayerBlockPlacement | `position i64 \| face VarInt \| hand VarInt \| cursorX/Y/Z f32` |
| 0x20 | UseItem | `hand VarInt`：对空使用物品（右键） |

---

### 2.5 Play (游戏状态 — 客户端接收 Clientbound)

| ID | 包名 | 关键字段与用途 |
|---|---|---|
| 0x00 | SpawnObject | `entityId VarInt \| uuid 16B \| type u8 (2=item) \| x/y/z f64 \| pitch/yaw Angle \| data i32 \| vx/vy/vz i16` |
| 0x01 | SpawnExperienceOrb | `entityId VarInt \| x/y/z f64 \| count i16` |
| 0x03 | SpawnMob | `entityId VarInt \| uuid 16B \| type VarInt \| x/y/z f64 \| yaw/pitch/head Angle \| vx/vy/vz i16 \| metadata` |
| 0x05 | SpawnPlayer | `entityId VarInt \| uuid 16B \| x/y/z f64 \| yaw/pitch Angle \| metadata` |
| 0x06 | Animation | `entityId VarInt \| animation u8 (0=挥臂)` |
| 0x08 | BlockBreakAnimation | `entityId VarInt \| position i64 \| stage u8 (0..9, 0xFF=清除)` |
| 0x0B | BlockChange | `position i64 \| blockState VarInt` |
| 0x0F | ChatMessage | `message JSON String \| position byte` |
| 0x11 | ConfirmTransaction | `windowId byte \| actionNumber short \| accepted bool` |
| 0x12 | CloseWindow | `windowId byte` |
| 0x14 | WindowItems | `windowId byte \| count short \| slots Slot[]` |
| 0x15 | WindowProperty | `windowId byte \| property short \| value short` (熔炉燃烧/冶炼进度条) |
| 0x16 | SetSlot | `windowId byte \| slot short \| item Slot` |
| 0x1A | Disconnect | `reason JSON String` |
| 0x1C | Explosion | `x/y/z f32 \| power f32 \| recordCount i32 \| records [3]i8[] \| pushX/Y/Z f32` |
| 0x1D | UnloadChunk | `chunkX i32 \| chunkZ i32` |
| 0x1F | KeepAlive | `keepAliveId i64` |
| 0x20 | ChunkData | `chunkX i32 \| chunkZ i32 \| fullChunk bool \| primaryBitMask VarInt \| dataSize VarInt \| data \| blockEntities[]` |
| 0x23 | JoinGame | `entityId i32 \| gameMode u8 \| dimension i32 \| difficulty u8 \| maxPlayers u8 \| levelType String \| reducedDebug bool` |
| 0x2C | PlayerAbilities | `flags u8 \| flySpeed f32 \| walkSpeed f32` |
| 0x2E | PlayerInfo | `action VarInt \| count VarInt \| [uuid + name + gameMode + ping + display]...` |
| 0x2F | PlayerPositionAndLook | `x/y/z f64 \| yaw/pitch f32 \| flags u8 \| teleportId VarInt` |
| 0x32 | DestroyEntities | `count VarInt \| entityIds VarInt[]` |
| 0x35 | Respawn | `dimension i32 \| difficulty u8 \| gameMode u8 \| levelType String` |
| 0x36 | EntityHeadLook | `entityId VarInt \| headYaw Angle` |
| 0x3A | HeldItemChange | `slot u8 (0..8)` |
| 0x3C | EntityMetadata | `entityId VarInt \| entries [index u8, type VarInt, value]... \| 0xFF` |
| 0x3E | EntityVelocity | `entityId VarInt \| vx/vy/vz i16 (单位: 1/8000 格/tick)` |
| 0x3F | EntityEquipment | `entityId VarInt \| slot VarInt (0=主手) \| item Slot` |
| 0x40 | SetExperience | `experienceBar f32 \| level VarInt \| totalExperience VarInt` |
| 0x41 | UpdateHealth | `health f32 \| food VarInt \| saturation f32` |
| 0x46 | SpawnPosition | `position i64` |
| 0x47 | TimeUpdate | `worldAge i64 \| timeOfDay i64` |
| 0x49 | NamedSoundEffect | `soundId VarInt \| category VarInt \| x/y/z i32 (固定点 *8) \| volume f32 \| pitch f32` |
| 0x4B | CollectItem | `collectedEntityId VarInt \| collectorEntityId VarInt \| pickupItemCount VarInt` |
| 0x4C | EntityTeleport | `entityId VarInt \| x/y/z f64 \| yaw/pitch Angle \| onGround bool` |
| 0x4E | TabComplete | `count VarInt \| matches String[]` |

---

## 3. EntityMetadata 索引与序列化器定义 (1.12.2 Forge/原版契约)

- **类型映射**：BYTE(0), VARINT(1), FLOAT(2), STRING(3), TEXT_COMPONENT(4), **ITEM_STACK(5)**, **BOOLEAN(6)**, ROTATIONS(7)。
  *(注意：1.12.2 Forge 将 BOOLEAN 注册为 6，原版插有 OPTIONAL_BLOCK_STATE 为 7，面向 Forge 客户端统一使用 6)*
- **EntityItem (掉落物)**：
  - Index 6: Slot (`ITEM_STACK` = 5) 物品堆叠
- **EntityCreeper (苦力怕)**：
  - Index 12: VarInt (`VARINT` = 1) Swell 引信状态（-1 空闲，1 引信中）
- **EntitySkeleton (骷髅)**：
  - Index 12: Boolean (`BOOLEAN` = 6) 举弓蓄力状态 (`SWINGING_ARMS`)
