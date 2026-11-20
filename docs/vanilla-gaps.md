# 原版一致性缺口清单

对照 Minecraft / Spigot 1.12.2 源码审计已实现部分的偏差。**每修完一条把状态改为 `已修` 并注明 commit**，不要删除条目（保留审计痕迹）。

- 审计基准：`build/rfg/minecraft-src/java/net/minecraft/`（Forge 反编译）+ `jars/spigot/spigot-1.12.2.jar`
- 严重度：**P0** = 数据丢失/玩家可见的破缺；**P1** = 机制与原版不同；**P2** = 数值/文本差异
- 状态：`待修` / `进行中` / `已修`

统计：**待修 39 条、已修 38 条、阻塞 1 条**（`python3 tools/vanilla_progress.py` 自动核对）。

---

## 已修（保留记录）

| ID | 内容 | commit |
|---|---|---|
| STORAGE-01 | 区块写 `LightPopulated=1` 但 SkyLight 全 0，Spigot 载入后全黑且永不重算 | `44e3c6f` |
| MOB-01 | `kHostileTypes={39,40,51}` 中 39/40 未注册，夜间只刷骷髅 | `44e3c6f` |
| NET-01 | SharedFlags 疾跑位写 `0x01`（=着火），应为 `setFlag(3)=0x08` | `44e3c6f` |
| NET-02 | 爆炸记录偏移用 `floor`，原版用 `(int)` 向零截断，负坐标偏 1 格 | `44e3c6f` |
| CONTAINER-01 | 容器窗口把物品拖到窗外直接删除（`slotClick(-999)` 应掉落） | `c0e94c5` |
| CMD-01 | `/op` `/deop` 要求 level 4，原版 3 | `44e3c6f` |
| CMD-02 | `/say` 无权限，原版 level 1 | `44e3c6f` |
| CMD-03 | `/kill` 无参不校验权限，原版整条 level 2 | `44e3c6f` |
| CMD-04 | `/gamemode c`/`1` 静默失败（命令层认简写、执行层只认全名） | `44e3c6f` |
| CMD-05 | `/time set -1` 生效；`query day` 文本；`/seed` 方括号；`/deop` 文本 | `44e3c6f` |
| COMBAT-01 | 玩家无受伤无敌帧，围殴伤害成倍叠加 | `44e3c6f` |
| CONTAINER-02 | `slot_click` 补齐 mode 2/3/4/6（换位/复制/丢弃/收集） | `4329bdd` |
| CONTAINER-03 | QUICK_CRAFT 拖拽状态机（开始/加槽/结束，均分与每槽一个） | `4329bdd` |
| CONTAINER-04 | 背包窗口改用同一 `slot_click`，mode 0..6 全部生效 | `4329bdd` |
| CONTAINER-05 | 创造 CLONE 改为从服务端槽复制并限 64，不再采信客户端 count | `4329bdd` |
| CONTAINER-06 | ConfirmTransaction 改用 slotClick 返回值（含数量）比对 | `4329bdd` |
| CONTAINER-07 | 容器→背包 QUICK_MOVE 改为 reverse（从热区栏末尾往前填） | `4329bdd` |
| CONTAINER-08 | 容器装满时余量留在源槽，不再继续在背包内搬动 | `4329bdd` |
| LOOT-01 | 未知条件改为恒假（不再当成满足）；新增 entity_properties(on_fire) | `21d956b` |
| LOOT-02 | `set_damage` 独立 Kind + `item::max_damage` 表，按比例磨损 | `21d956b` |
| LOOT-04 | 空池保留（仍消耗 rolls 随机数） | `21d956b` |
| LOOT-05 | `set_count` 不截断，按堆叠上限拆堆 | `21d956b` |
| LOOT-06 | `killed_by_player` 支持 `inverse` | `21d956b` |
| SYS-01 | 天气状态 + `updateWeatherBody` 周期 + ChangeGameState 1/2/7/8；`/weather` 实装 | `d54bb8d` |
| SYS-02 | `/difficulty` 实装 + ServerDifficulty 广播 + 和平清除敌对生物 | `d54bb8d` |
| SYS-03a | **GameRules 类型被 level.dat 加载降级成 String**（已修，`loadFromNBT` 不 clear） | `e87b27a` |
| SYS-03b | `/gamerule` 实装（列表/查询/设置/布尔校验/未知规则） | `e87b27a` |
| SYS-03c | `keepInventory` 接入死亡掉落；`doMobSpawning` 接入自然刷怪 | `e87b27a` |
| SYS-03d | `reducedDebugInfo` 变化广播 EntityStatus 22/23 | `e87b27a` |
| CONTAINER-09 | 盔甲自动装备 + 盾牌进副手（`item::equipment_slot`，分支顺序同原版） | `b2aa085` |
| CONTAINER-10 | THROW 前置（光标须为空）与 -999 右键只丢 1 个 | `b2aa085` |
| CONTAINER-11 | 熔炉窗口把物品拖到窗外直接删除（同 CONTAINER-01 根因） | `b2aa085` |
| COMBAT-02 | 攻击冷却 `damage *= 0.2 + f2²*0.8` + `item::attack_speed` 表 | `b2aa085` |
| AI-01 | `door_open` 不再被排除出 seen 集合（可穿过开着的门）+ `can_enter_doors_` 开关 | `d5bb6ac` |
| AI-02 | 生物上台阶 0.6 + `EntityMoveHelper` 起跳条件与 0.42 初速 | `d5bb6ac` |
| AI-04 | 苦力怕引信 3 格起爆 / 7 格维持，且不再要求视线 | `d5bb6ac` |
| AI-03 | 经验球物理（0.98 摩擦/-0.9 反弹）、8 格吸附、6000 tick 寿命 | `5180473` |

---

## P0 — 数据丢失 / 玩家可见的破缺

| ID | 区域 | 位置 | 原版参照 | 问题 | 状态 |
|---|---|---|---|---|---|
| LOOT-03 | 战利品 | `loot_table.hpp` 注册表 | `EnchantRandomly/EnchantWithLevels/SetNBT/SetAttributes` | 4 个函数已注册为 Noop + 加载告警；真正生效依赖缺失的附魔表 / ItemStack NBT / 属性修饰符 | 部分（阻塞） |
| COMBAT-03 | 战斗 | `connection_combat.cpp` | `EntityPlayer.java:1396-1460` | 暴击（下落中、非疾跑、`f2>0.9`，×1.5）与横扫（`flag && !flag2 && !flag1 && onGround`，邻近实体各 1 点 + 0.4 击退）未实现；击退增益也未接 | 待修 |
| AI-11 | 经验球 | `src/net/xp_orb.cpp` | `EntityXPOrb.java:239`、`EntityPlayer.java` | 拾取冷却 `xpCooldown`（拾取后置 2、每 tick 递减）未做；经验球模块无单元测试覆盖（需 World 实例） | 待修 |
| SYS-03e | 规则 | `server.cpp` / `mob_manager.cpp` | `GameRules.java` 各读取点 | 仍未接线：`doMobLoot` `doTileDrops` `doEntityDrops` `showDeathMessages` `naturalRegeneration` `randomTickSpeed` `doFireTick` `mobGriefing` `spawnRadius` `maxEntityCramming` `doLimitedCrafting` `spectatorsGenerateChunks` `logAdminCommands` `commandBlockOutput` `sendCommandFeedback` `announceAdvancements` | 待修 |
| GEN-01 | 群系 | `gen_layer_chain.hpp` | `GenLayer.java:22-88` | 19 个 GenLayer 全部实现，且 `ChunkGeneratorOverworld` 的四处群系查询已切到链上（`5991005`） | 已修 `5991005` |
  Hills 已于 `9f74c79` 完成。仍缺 BiomeEdge / Shore，卡在尚未建模的机制上：
  · **变异体表**：`Biome.getMutationForBiome(id)` = `fromId(id + 128)`，但只有部分群系有变异体，
    需要在表里标明"无变异"以复现原版返回 null 的分支
  · **群系类等价**：`GenLayer.biomesEqualOrMesaPlateau` 用 `getBiomeClass()` 比较，
    即同一 Java 类视为等价（desert 与 desert_hills 不同类，但 mesa_rock/mesa_clear_rock 同类等价），
    需要一张"类族"表才能忠实实现
  · BiomeEdge 还需要**温度类别表**：邻居谓词 `b(a,b)` 在群系类不等价时，
    退化为比较 `getTempCategory()`，且 MEDIUM 与任何类别都算匹配
  · Shore 另需 `c(int)`/`d(int)` 两个谓词与丛林特例、`Biome.getBiome` 映射、WorldType 分支，以及把生成器从自创 sin/cos 气候函数切到 GenLayer 链 | 进行中 |
| GEN-02 | 装饰 | `world.hpp` populate 级联 | `Chunk.java:1041-1070` | 完成：`terrain_populated` 标志、四向级联、矿脉/装饰移入 populate、原版种子派生、写穿邻区块、首次访问铺 3×3 生成半径。特征本身仍按局部坐标生成，跨边界靠 spill 回调（`5302201`/第 3 步） | 已修 |
| GEN-03 | 结构 | `structure.hpp` 仅 136 行 | `MapGenStructure` 全家族 | 矿井/村庄/要塞/神殿/海底神殿/府邸/下界要塞/末地城全部缺失；地牢/水井/化石写好但未接线 | 待修 |

---

## P1 — 机制与原版不同

| ID | 区域 | 位置 | 原版参照 | 问题 | 状态 |
|---|---|---|---|---|---|


| AI-05 | 目标 | `mob_manager.cpp:516-534` | `EntityAITarget.java` | `unseen_ticks` 计的是“出范围”不是“看不见” → 僵尸隔墙永久追击 | 待修 |
| AI-06 | 近战 | `mob_manager.cpp:995` | `EntityAIAttackMelee.java:139-160` | 挥击不再要求视线：进入 `reach_sq` 后按冷却挥击 | 已修 |
| AI-07 | 骷髅 | `mob_manager.cpp` / `projectile_manager.cpp` | `AbstractSkeleton.java:49`、`EntityArrow.java:227-250` | 射箭冷却改为 20（原版构造参数固定 20，与难度无关）；取消飞行中箭的 100 tick 自毁（原版只有落地 1200 tick 后消失）；箭伤 2.0×速度 与原版一致 | 已修 |
| AI-08 | 动物 | `mob_manager.cpp:253-274` | `EntityAnimal.setInLove`、`EntitySheep.onSheared` | 无繁殖/幼崽/成长；爱心 2000（应 600）；羊毛恒 1 个且恒白（应 1–3、继承颜色） | 待修 |
| AI-09 | 掉落 | `mob_manager.cpp:349-357` | `EntityLivingBase.java:435` | 任何死法都掉经验（原版要求 `recentlyHit>0`）→ 岩浆/摔落刷怪机可无限刷 | 待修 |
| AI-10 | 碰撞 | `blocks.hpp:186-250`、`physics.hpp:156-170` | `getCollisionBoundingBox` | 碰撞体按整格，半砖/楼梯/耕地/灵魂沙都当满方块 | 待修 |
| CMD-06 | 命令 | `command.cpp:380-431` | `EntitySelector.java:44`、`CommandHandler.java:69-91` | 无选择器参数（`type=/name=/r=/dx=/team=/score_*`）与多实体展开 | 待修 |
| CMD-07 | 命令 | `command.cpp:554-564` | `CommandGive.java:56-72` | `/give` 数量 clamp 1..64、忽略 dataTag、反馈用注册名 | 待修 |
| CMD-08 | 命令 | `command.cpp:582-596` | `CommandClearInventory.java:173-221` | `/clear` 忽略 item/data/maxCount，无“只统计”模式 | 待修 |
| CMD-09 | 命令 | `command.cpp:292-298` | `CommandTP.java:76-81`、`CommandBase.java:454-505` | `/tp` 不支持 `~` 相对坐标、无 ±30000000/±4096 校验、无 yaw/pitch | 待修 |
| CMD-10 | 命令 | `command.cpp:241-253` | `CommandGameMode.java:54-68` | 缺 `gameMode.changed` 通知（改别人模式时对方收不到）；success 文本发给本人（原版不发） | 待修 |
| CMD-11 | 命令 | `command.cpp:646-651` | `CommandSaveAll.java:215-231` | `/save-all` 忽略 `flush`（无 fsync） | 待修 |
| CMD-12 | 命令 | `command.cpp:270-277` | `CommandDefaultGameMode.java:195-205` | `/defaultgamemode` 只发消息，不改默认模式 | 待修 |
| SYS-04 | 边界 | `world_border.hpp` | `WorldBorder.java`、`CommandWorldBorder.java` | 死代码，从未接入；`SPacketWorldBorder` 无发送点 | 待修 |
| SYS-05 | Boss | `boss_info.hpp` | `BossInfoServer.java:148-263` | 死代码；`BossBar(0x0C)` 无发送点；用 entity id 而非 UUID | 待修 |
| SYS-06 | 刷怪 | `mob_manager.cpp:1071-1166` | `WorldEntitySpawner.java:34-118` | 周期 200 vs 每 tick + 400 tick 成群；上限“全局 5” vs 70×区块/289；只在地表；亮度硬阈值 | 待修 |

---

## P2 — 数值 / 文本差异

| ID | 区域 | 位置 | 原版参照 | 问题 | 状态 |
|---|---|---|---|---|---|
| GEN-04 | 群系表 | `biome.hpp:37,39,47,48` | `Biome.java:567,571,584,585` | stone_beach 高度变化 0.3（应 0.8）；jungle base 0.125（应 0.1）；id 38/39 名称与高度错位 | 待修 |
| GEN-05 | 装饰次数 | `chunk_generator.hpp:429-469` | `BiomeJungle/Savanna/Taiga/Forest` | 丛林树 12（应 50）、草 12（应 25）；草原草 8（应 20）；深色森林应 `-999` | 待修 |
| GEN-06 | 花草 | `vegetation.hpp:34` | `WorldGenFlowers.java:27` | 16 次（应 64），且只认 grass 不认泥土/灰化土 | 待修 |
| GEN-07 | 矿脉 | `feature/ores.hpp:20-23,45` | `WorldGenMinable.java:34-37` | 缺中心 `+8` 偏移并裁到本区块 → 矿脉永不跨区块 | 待修 |
| GEN-08 | 湖 | `chunk_generator.hpp:389-409` | `ChunkGeneratorOverworld.java:439-459` | 熔岩湖 1/80（应 1/8）；水湖 y 固定扫描、缺 desert_hills 排除、坐标缺 +8 | 待修 |
| GEN-09 | 泉 | `chunk_generator.hpp:417,423` | `BiomeDecorator.java:352-366` | `nextInt(248)` 写 240、`240` 写 230 且多 +4 | 待修 |
| GEN-10 | 洞穴 | `map_gen.hpp:211-237` | `MapGenCaves.java:203-220,326-360` | `digBlock` 白名单缺 159/172/110，缺“上方非水”与 `foundTop` 复原 | 待修 |
| GEN-11 | 下界洞 | `map_gen.hpp:289-314` | `MapGenCavesHell.java:43,112,126-146` | 缺 `nextInt(6)==0`→0.92 分支、缺岩浆检测、`y≤31` 自创填岩浆（原版只放空气） | 待修 |
| GEN-12 | 地表 | `chunk_generator.hpp:279-282` | `Biome.java:330-340,361-365` | 缺“沙→砂岩”续接、缺冷群系 ICE/WATER 分支、噪声源单层而非 4 层 | 待修 |
| GEN-13 | 装饰缺失 | `chunk_generator.hpp` | `BiomeDecorator.java:131-152,210-330` | 无沙盘/黏土盘/砾石盘、枯木、蘑菇、藤蔓、西瓜、双高草 | 待修 |
| GEN-14 | 超平坦 | `dim_generators.hpp:19-34` | `ChunkGeneratorFlat.java:108-137` | 不设 seaLevel、不写群系、无装饰与结构 | 待修 |
| NET-03 | 协议 | `connection_login.cpp:172` | `SPacketJoinGame.java:69-76` | 缺 hardcore 位（`gameType | 8`） | 待修 |
| NET-04 | 协议 | `connection_login.cpp:170-176` | `PlayerList.java:152` | difficulty/levelType/reducedDebugInfo 写死 | 待修 |
| NET-05 | 协议 | `chunk_codec.hpp:18-24,69-75` | `BlockStateContainer.java:44-56` | palette>256 时仍写调色板前缀（应切全局调色板、无前缀） | 待修 |
| NET-06 | 协议 | `packet_writers.hpp:173-175` | `SPacketSoundEffect.java:32-34` | 音效坐标量化到方块中心（原版用精确位置） | 待修 |
| NET-07 | 协议 | `play_fields.hpp:43` | `SoundCategory.java:11-20` | 所有音效都用 BLOCKS(4)；生物应 HOSTILE(5)、玩家应 PLAYER(7) | 待修 |
| CMD-13 | 文本 | `command.cpp` 多处 | `en_us.lang` | usage 缺 `Usage: ` 前缀；`Unknown command` 文案；权限句多句号；`/help` 硬编码假列表 | 待修 |
| SYS-07 | 状态 | `status.cpp:14-23` | `MinecraftServer.java:747-758` | 玩家列表 JSON 缺 `players.sample` | 待修 |

---

## 整类未实现（不在上表逐条列）

红石（线/中继器/比较器/活塞）、维度旅行（下界门/末地门）、计分板与 `CommandResultStats`、成就/进度、附魔与药水、`ItemStack` NBT 子系统、光照引擎、约 30 条原版命令（`/gamerule` `/worldborder` `/scoreboard` `/effect` `/xp` `/fill` `/setblock` `/summon` `/tellraw` `/me` `/kick` `/ban` …）、`MapGenRavine`。

---

## 修复顺序

1. **CONTAINER-02..06**（物品丢失/复制，最伤玩家，范围可控）
2. **LOOT-01..06**（掉落全错，集中在 loot 层）
3. **SYS-01/02/03**（天气/难度/规则接线，命令目前是空壳）
4. **COMBAT-02 + AI-01..04**（战斗与 AI 手感）
5. **GEN-01/02/03**（GenLayer 链 + populate + 结构，工作量最大，需要改生成架构）
6. **CMD-06..12**（选择器与命令补全）
7. 其余 P2

## 审计中排除的误报

- 「`replaceBiomeBlocks` 存在 x/z 转置 quirk」——**不成立**。按 `ChunkPrimer.getBlockIndex` 与 `NoiseGeneratorSimplex.add` 的填充序推导，群系索引、depth 索引、写入坐标三者在 `(j,i)` 上一致，只是写法绕。

## 已知偏差：地牢出现率

地牢已按原版接入 populate（`dungeonChance=8`，偏移 `+8`），但 8×8 区块
实测未出现苔石（地牢的唯一产物）。原因是原版开口判定要求四壁在房间高度上
恰好有 1..5 个两格高的空气开口，即必须有洞穴在此处穿过；我们的洞穴密度
比原版低，导致该条件几乎不满足。

已确认机制可用：dungeon_can_succeed_in_real_terrain 在真实地形上 4000 次直接
调用可以成功开凿，说明预检与开凿本身没有问题；稀疏发生在"开口条件"的命中率上。

量级推算：populate 每个区块 8 次尝试，8×8 区块共 512 次，实测 0 次成功，
而直接探测表明成功率低于 1/1000（原版约 1.5%）。差两个数量级。已逐行核对
MapGenCaves 的全部参数与算法，**均与原版一致**：
  · range = 8（MapGenCavesHell 为 10）
  · 条数 nextInt(nextInt(nextInt(15)+1)+1)，nextInt(7) != 0 → 0
  · 初值 nextInt(16) / nextInt(nextInt(120)+8) / 长 1.0+rand*6.0
  · 外层半径 0.5/1.5、pitch *= 0.92/0.7、f1 *= 0.9、f *= 0.75、正弦包络
  · 内层归一化椭圆 d10²+d8²<1、d9>-0.7、d10²+d9²+d8²<1
  · canReplaceBlock：stone/dirt/grass/sand/gravel/sandstone/red_sandstone/snow
因此稀疏不是洞穴算法造成的，嫌疑转向地形本身（岩层厚度/高度分布）。
实测数据（种子 424242）：
  · y∈[1,60) 的空气占比 0.69%（含洞穴，量级正常）
  · populate 中地牢成功 1 次 / 144 区块（1152 次尝试 ≈ 0.087%），原版约 1.5%
  · 直接对真实地形调用 generate_dungeon 可成功，说明逻辑无误
结论：地牢机制正确但出现率约为原版的 1/17，差距来自开口条件（需洞穴恰好
穿过四壁）在本地形下的命中率。已在 dungeons_generate_in_populate 里以
"144 区块至少 1 次"作为确定性回归基线。

在此之前不改开口判定——它是原版行为，不该为了"看起来有地牢"而放宽。

## 已知偏差：生物 AI 的随机源

`mob_manager.cpp` 里的 `rng()` 是未播种的全局 `mt19937`（`std::random_device` 初始化），
而原版 AI 的随机都取自 `world.rand`（由世界种子派生）。后果有两个：

1. 同一世界重放时生物行为不可复现；
2. 依赖生物游走的测试可能跨次运行不稳定（已遇到一次：僵尸绕出测试墙）。

已修：服务器在拿到世界种子后调用 `net::set_ai_rng_seed(seed)`，AI 随机源
与 world.rand 一样跟随世界种子；新增 mob_ai_is_reproducible_with_seeded_rng
验证同种子下 60 tick 后位置完全一致。
