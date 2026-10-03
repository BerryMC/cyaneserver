# 刷怪蛋 / 生物 / AI 实施计划

## 现状（已探明）

- **生物模型**：只有 4 种被动（猪90/羊91/牛92/鸡93），纯随机漫游——`pos.y` 恒定、**无重力无碰撞**（穿墙、悬空）、不认玩家、被打不会掉血、没有死亡与消失路径（`MobManager::remove` 是死代码）。
- **生成**：只在启动时 `spawn_passive(12)`（硬编码、绕世界原点而非出生点）；**没有刷怪蛋**。
- **战斗**：`UseEntity 0x0A` 完全未处理（玩家打不到生物）；生物不能伤害玩家（只有 `/kill`、虚空、远程 kill 通道）；无健康值/受伤/击退/掉落。
- **可复用**：hub 邮箱（`send_kill`/`send_gamemode` 的跨线程模式）、`UpdateHealth`/`EntityStatus`/`Animation`/`EntityTeleport` 写包器、20Hz `Server::tick` 挂钩、掉落物系统（生成/拾取/持久化都通）、音效 id 表（`tests/fixtures/sound_registry_ids.txt`，549 条）。
- **顺带要修的既有缺陷**：实体 id 有两套分配器（生的生物 1000+ 与玩家/存档生物 1+ 可能撞号）；存档里未知物种 NBT 会**静默存成猪**（`anvil.cpp` 的越界回退）；SpawnMob 只认 4 种。

## 设计

### 1. 物种表 `include/cyane/world/mob_types.hpp`（新，放 world 层供 anvil 与 MobManager 共用）
`MobType{type, nbt_id, 宽高, 血量, 速度, 近战伤害, hostile, ranged, explodes, hurt/death/attack 音效 id, 掉落表}`，配 `mob_type(type)`、`mob_type_from_nbt(id)`、`spawn_egg_type(item_id, damage)`。
物种：被动 4 种 + **苦力怕50、骷髅51、蜘蛛52、僵尸54**（id 与尺寸/血量/伤害/掉落从 jar 提取核对，记 R-022）；掉落用现有 mt19937 风格（僵尸→腐肉、骷髅→骨头+箭、苦力怕→火药、蜘蛛→线、被动各自掉落）。

### 2. 物理 `include/cyane/world/physics.hpp`（新）+ `blocks.hpp::is_solid`
`is_solid(state)` 用非固体白名单（空气/水/岩浆/花草/火把/红石线/铁轨/按钮拉杆/压力板/告示牌/作物/传送门/地毯…）。
`Aabb` + `move_with_collision(World&, Aabb&, dx, dy, dz, step_height)`：Y 先落地（重力 -0.08/tick、终端速度），再 X、Z 轴分离推进，被挡时尝试**上台阶**（≤ 1 格且上方净空）。返回 `on_ground`。

### 3. MobManager 重构（`net/mob_manager.hpp|cpp`）
- `Mob` 增：`health`、`velocity_y`/`on_ground`、`ai_state`、`target_player`、`attack_cooldown`、`fuse_ticks`、`hurt_ticks`。
- 实体 id 统一走 `entity::allocate_entity_id()`（修撞号）。
- 新 API：`spawn(type,x,y,z,yaw)->id`、`damage(id, 伤害, 攻击者位置)->{受伤/死亡}`、`tick(World&, 玩家快照, now_ms) -> TickResult`。
- **`TickResult` 承载事件**（保持 MobManager 不依赖连接/邮箱）：`moved`（广播 EntityTeleport+HeadLook）、`attacks`（打玩家）、`deaths`（DestroyEntities+掉落）、`explosions`（苦力怕）、`shots`（骷髅射箭）。Server 负责落地这些事件。

### 4. 敌对 AI（目标选择/追击/近战；简化寻路，非 A*）
- 被动：Idle ⇄ Wander（沿用）+ **Panic**（受击后朝反方向跑 3–5 秒）。
- 敌对：Idle → Chase（每 10 tick 扫一次最近玩家，距离 ≤ 16、|dy| ≤ 4）→ Attack（到近战距离后每 20 tick 挥臂 `Animation 0x06` + 造成伤害 + 击退）；转向 = 朝目标归一化水平方向，被挡走上台阶/跳跃，避免走下悬崖（前探检查）。
- 苦力怕：Chase 进 3 格 → **引信 30 tick**（`entity.creeper.primed` 音效）→ `explosions` 事件。
- 骷髅：与目标保持 8–16 格 → 每 40 tick 射箭（`entity.arrow.shoot` 音效）→ `shots` 事件。

### 5. 战斗闭环
- **玩家攻击生物**：新增 `handle_play_use_entity`（0x0A：`varint target | varint type | 可选 f32×3`），`type==1` 攻击 → `MobManager::damage(id, 手持伤害)`；受伤广播 `EntityStatus(2)` + `EntityVelocity` 击退；死亡 → `DestroyEntities` + 按掉落表生成掉落物。
- **生物伤害玩家**：`HubMessage` 增伤害字段 + `PlayerHub::send_damage(id, 数值, 来源坐标)`；连接侧新增 `apply_damage()`（扣 `health_` → `UpdateHealth` + 受伤 `EntityStatus(2)` + 击退 → 归零走现有 `kill_player()`）。
- **掉落物生成去重**：把 `Connection` 里的 SpawnObject/EntityMetadata 编码与广播抽成共享函数（供连接与 Server 共用）。
- **音效**：受伤/死亡/攻击/爆炸/箭矢 id 从 `sound_registry_ids.txt` 查表。

### 6. 苦力怕爆炸 + 骷髅箭矢
- 爆炸：范围伤害（按距离衰减，打玩家走 `send_damage`、打生物走 `MobManager::damage`）+ 破坏方块（复用 `block_drops` 掉落）+ `entity.generic.explode` 音效 + 客户端爆炸包/粒子。
- 箭矢：新 `net/projectile_manager.{hpp,cpp}`（实体 id、位置、速度、主人、伤害、TTL）：每 tick 重力 + 位移，命中固体方块或实体（生物/玩家）→ 伤害 + `DestroyEntities` + 落地留下可拾取的箭；客户端用 `SpawnObject(0x00)` type=箭 + 速度字段渲染。

### 7. 刷怪蛋
`spawn_egg_type(383, damage)`（damage 即实体类型 id）。在 `handle_play_block_place`（点方块：目标格取 `face_delta`）与 `handle_play_use_item`（对空：视线前方 1–2 格）**两处**都处理，顺序在交互方块/进食之后、方块放置之前；生成后单发 `SpawnMob` + `hub.broadcast_near` 广播；生存模式消耗 1 个（复用既有消耗模式）。

### 8. 持久化
`MobState` 增 `health`（NBT `Health` 不再恒写 20）；`anvil.cpp` 用物种表做 NBT 名 ↔ 类型（修"未知物种存成猪"），把 4 种敌对纳入 `is_modeled_entity_id`，跨重启保留；读档恢复血量、AI 重置。

## 分三个提交（每个都：构建 + 138+ 测试全绿 + 探针可验证）

1. **生物基座**：物种表、物理（重力/碰撞/上台阶）、统一实体 id、刷怪蛋、持久化（新物种 + 血量）、MobManager 新 API。
2. **战斗闭环**：UseEntity 玩家攻击、生物血量/受伤/死亡/掉落、生物伤害玩家（hub 伤害通道 + `apply_damage`）、敌对 AI（僵尸 + 蜘蛛）、击退与音效。
3. **苦力怕 + 骷髅**：爆炸（伤害/破坏方块/音效/特效）、箭矢投射物管理器、对应 AI。

## 测试与验证

- **单元**：物种表（type↔NBT↔蛋 damage）、`is_solid`、物理（下落落地/撞墙停住/上台阶/不穿墙）、AI（追击向目标收敛、进入距离触发攻击事件、引信计时到点爆炸、射击间隔）、`damage()` 掉血/死亡/掉落、`UseEntity` 与箭矢 `SpawnObject` 黄金向量。
- **活体探针**（扩展 `tools/probe_mobs.py`）：刷怪蛋 → 收到 SpawnMob（正确 type）；僵尸靠近 → 收到 UpdateHealth/受伤状态；玩家左键 → 生物掉血 → 死亡 DestroyEntities + SpawnObject 掉落物 → 拾取入包。
- **三套构建**：常规 / `-Werror` / ASan+UBSan；参考日志记 R-022（实体类型、刷怪蛋、尺寸血量伤害、掉落表、箭矢与爆炸参数，全部 jar 提取）。

## 本轮明确不做（记录为后续）

自然刷怪（昼夜/光照/密度，你已确认下一轮）、经验/附魔/药水、生物装备与幼年变种、A* 寻路（本轮为转向+上台阶+跳跃的简化寻路）、阳光燃烧与距离消失、末影人/女巫等其余物种、怪物骑乘/繁殖。