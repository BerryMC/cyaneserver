# MILESTONE 03 — Login Flow 协议修复

## 完成状态
✅ **Login 流程协议修复完成** — 76/76 tests pass，probe 脚本验证所有包格式正确。

## 修复内容

### 1. Online-mode UUID 解析
- **问题**: `finish_login` 接收在线模式 32-bit 紧凑 UUID，但直接当作 36-char dashed UUID 传给 `fields.string()`
- **修复**: 调用 `crypto::uuid_with_dashes()` 转换

### 2. JoinGame (0x23) 包格式
- **问题**: entityID 和 dimension 使用 `i32` (4 bytes) 而非 `varint`
- **修复**: 改用 `fields.varint()` 发送 entityID 和 dimension
- **验证**: payload = `01 00 00 02 14 07 64 65 66 61 75 6c 74 00` (14 bytes)

### 3. PlayerInfo (0x2E) 包格式
- **问题1**: UUID 使用 `fields.string(uuid_dashed)` (36 bytes) 而非 16 bytes raw
- **问题2**: 多余 `fields.boolean(false)` (hasDisplayName) — 1.12.2 不需要此字段
- **问题3**: properties 应在 gameMode 之前
- **修复**: 用 `crypto::parse_uuid_string()` 获取 16 bytes，移除多余 boolean，调整字段顺序
- **验证**: payload = `00 01 09 7d 33 92 86 5a 3f 3c 8b 4a da 1c 34 73 46 6c 08 54 65 73 74 55 73 65 72 00 00 14 00` (31 bytes)

### 4. SpawnPosition (0x46) 包格式
- **问题**: 使用 `i64(encode_position(...))` 发送打包位置，但 1.12.2 需要三个独立 i32
- **修复**: 改用三个 `fields.i32(x/y/z)`
- **验证**: payload = `00 00 00 00 00 00 00 40 00 00 00 00` (12 bytes)

### 5. Chunk Data (0x20) 包格式
- **问题**: 使用 `fields.string(world_name)` 作为 chunk data 内容
- **修复**: 发送 0 bit mask + 0 chunk data + 0 block entities
- **状态**: 字段顺序已修正，但 chunk data 内容为空（待 world 系统实现后填充）

## 包序列 (offline mode)
```
0x03 → SetCompression (threshold=256)
0x02 → LoginSuccess  (UUID + username)
0x23 → JoinGame      (entityID, gamemode, dimension, difficulty, maxPlayers, levelType, reducedDebugInfo)
0x46 → SpawnPosition (x, y, z)
0x47 → TimeUpdate    (worldTime, dayTime)
0x2E → PlayerInfo    (action=0, entries=[UUID, name, properties=0, gameMode, ping, hasDisplayName=false])
0x41 → UpdateHealth  (health, foodLevel, saturation)
0x1A → Disconnect    (world system not implemented yet)
```

## 测试验证
- 76/76 tests pass
- probe_login_full.py 脚本成功解析所有 8 个包
- 所有包字段格式符合 Minecraft 1.12.2 协议规范

## 文件变更
- `src/net/connection.cpp`: finish_login() 所有包格式修正
- `tools/probe_login_full.py`: 新增完整登录探测脚本
- `include/cyane/proto/packet_ids.hpp`: enum 命名统一（已在 M02 完成）

## 下一步
1. 实现 Chunk Data 有效载荷（发送出生点区块）
2. 实现 PlayerAbilities (0x2C) 包发送
3. 实现 PlayerPositionAndLook (0x2F) 包发送
4. 实现 KeepAlive (0x1F) tick 机制
5. 实现 world/chunk 系统
