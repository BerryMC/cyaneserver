# 原版一致性缺口清单

对照 Minecraft / Spigot 1.12.2 源码审计已实现部分的偏差。**每修完一条把状态改为 `已修` 并注明 commit**，不要删除条目（保留审计痕迹）。

- 审计基准：`build/rfg/minecraft-src/java/net/minecraft/`（Forge 反编译）+ `jars/spigot/spigot-1.12.2.jar`
- 严重度：**P0** = 数据丢失/玩家可见的破缺；**P1** = 机制与原版不同；**P2** = 数值/文本差异
- 状态：`待修` / `进行中` / `已修`

统计：P0 15 条、P1 24 条、P2 18 条，已修 16 条。

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
| CONTAINER-02 | `slot_click` 补齐 mode 2/3/4/6（换位/复制/丢弃/收集） | `CONTAINER-02` |
| CONTAINER-03 | QUICK_CRAFT 拖拽状态机（开始/加槽/结束，均分与每槽一个） | `CONTAINER-03` |
| CONTAINER-04 | 背包窗口改用同一 `slot_click`，mode 0..6 全部生效 | `CONTAINER-04` |
| CONTAINER-05 | 创造 CLONE 改为从服务端槽复制并限 64，不再采信客户端 count | `CONTAINER-05` |
| CONTAINER-06 | ConfirmTransaction 改用 slotClick 返回值（含数量）比对 | `CONTAINER-06` |

---

## P0 — 数据丢失 / 玩家可见的破缺

| ID | 区域 | 位置 | 原版参照 | 问题 | 状态 |
|---|---|---|---|---|---|
| LOOT-01 | 战利品 | `loot_table.hpp:289-294` | `LootConditionManager.java:120-126` | 未注册条件静默 `continue` → 条件恒真；`entity_properties(on_fire)` 失效 → **牛/鸡/猪永远掉熟肉** | 待修 |
| LOOT-02 | 战利品 | `loot_table.hpp:359-366` | `SetDamage.java:75-80` | `set_damage` 映射成 `SetData` → 钓到的破损物品耐久永远满 | 待修 |
| LOOT-03 | 战利品 | `loot_table.hpp:317-385` | `EnchantRandomly/EnchantWithLevels/SetNBT/SetAttributes` | 缺 4 个函数（数据里 21+4+2 处引用）→ 附魔书/附魔金苹果无附魔 | 待修 |
| AI-01 | 寻路 | `pathfinding.hpp:657-659` | `WalkNodeProcessor.java:364-367` | `door_open` 被 `continue` 排除；全落在门洞时返回 `blocked` → 生物无法穿过开着的门 | 待修 |
| AI-02 | 移动 | `mob_manager.cpp:969` | `EntityLivingBase.java:216` | 上台阶 1.0（应 0.6），无 `velocity_y=0.42` 跳跃 | 待修 |
| AI-03 | 经验球 | `xp_orb.cpp:100-121` | `EntityXPOrb.java:114-146,168-173` | 无 8 格吸附、无 6000 tick 寿命、无 `xpCooldown` | 待修 |
| AI-04 | 苦力怕 | `mob_manager.cpp:762` | `EntityAICreeperSwell.java:24-25` | 引信在 7 格就触发（原版 3 格起爆、7 格维持）；且永不近战 | 待修 |
| COMBAT-02 | 战斗 | `connection_combat.cpp:60` | `EntityPlayer.java:1387-1390` | 无攻击冷却缩放，连点满额伤害 → DPS 约 5 倍；无暴击/横扫/附魔 | 待修 |
| SYS-01 | 天气 | `command.cpp:497-508` | `CommandWeather.java:180-220` | 无任何天气状态与 `ChangeGameState(1/2/7/8)` 发包；`/weather` 只回显 | 待修 |
| SYS-02 | 难度 | `command.cpp:521-524` | `CommandDifficulty.java:280-294` | 不校验不回写；和平不清敌对生物、不阻刷怪；困难无骷髅加速 | 待修 |
| SYS-03 | 规则 | `server.cpp:548` | `GameRules.java` | 24 条规则只有 `doDaylightCycle` 被读取；`keepInventory` 无效导致死亡必掉物品 | 待修 |
| GEN-01 | 群系 | `biome.hpp:57-109` | `GenLayer.java:22-88` | 自创 sin/cos 气候函数取代 20+ 层 GenLayer 链；无河流/海岛/丘陵/Voronoi，尺度差 1~2 个数量级 | 待修 |
| GEN-02 | 装饰 | `chunk_generator.hpp:55-67` | `ChunkGeneratorOverworld.java:390-503` | 无 populate 阶段，特征全裁在本区块 → 树/湖/矿脉在区块边界硬切 | 待修 |
| GEN-03 | 结构 | `structure.hpp` 仅 136 行 | `MapGenStructure` 全家族 | 矿井/村庄/要塞/神殿/海底神殿/府邸/下界要塞/末地城全部缺失；地牢/水井/化石写好但未接线 | 待修 |

---

## P1 — 机制与原版不同

| ID | 区域 | 位置 | 原版参照 | 问题 | 状态 |
|---|---|---|---|---|---|
| CONTAINER-07 | 容器 | `connection_container.cpp:185-188` | `ContainerChest.java:70` | 容器→背包 `QUICK_MOVE` 方向反了（应 `reverse=true`，从热区栏末尾往前填） | 待修 |
| CONTAINER-08 | 容器 | `connection_container.cpp:224-229` | `ContainerChest.java:75-78` | 容器装满时余量继续在背包内搬动（原版留在源槽） | 待修 |
| CONTAINER-09 | 容器 | `connection_inventory.cpp:168-192` | `ContainerPlayer.java:167-182` | 缺盔甲/副手 shift 自动装备 | 待修 |
| CONTAINER-10 | 容器 | `connection_inventory.cpp:193-226` | `Container.java:441-451` | `THROW` 缺“光标必须为空”前置；窗口外右键丢整叠（应只丢 1） | 待修 |
| LOOT-04 | 战利品 | `loot_table.hpp:446` | `LootPool.java:301-311` | 空池被丢弃；原版空池仍消耗 rolls 随机数 → 同种子掉落不同 | 待修 |
| LOOT-05 | 战利品 | `loot_types.hpp:92-96` | `SetCount.java:24-27` + `LootEntryItem.java:165-183` | `set_count` 截断到 64 且不拆堆 | 待修 |
| LOOT-06 | 战利品 | `loot_types.hpp:66-67` | `KilledByPlayer.java:104-113` | `killed_by_player` 忽略 `inverse` | 待修 |
| AI-05 | 目标 | `mob_manager.cpp:516-534` | `EntityAITarget.java` | `unseen_ticks` 计的是“出范围”不是“看不见” → 僵尸隔墙永久追击 | 待修 |
| AI-06 | 近战 | `mob_manager.cpp:943` | `EntityAIAttackMelee.java:139-160` | 挥击要求视线（原版不要求）→ 隔墙角贴脸不挥拳 | 待修 |
| AI-07 | 骷髅 | `mob_manager.cpp:27,907` | `AbstractSkeleton.java:235-237`、`EntityArrow.java:368` | 射箭间隔写死 40（困难应 20）、箭伤写死 2.0；箭 5 秒空中自毁（原版无） | 待修 |
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
