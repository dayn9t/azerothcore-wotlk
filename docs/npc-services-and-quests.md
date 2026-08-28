# NPC 服务与任务系统实现流程（源码探索存档）

> 探索日期：2026-08-28 · 分支：`Playerbot` · 核心代码：`src/server/game/`
>
> 本文基于 codebase-memory-mcp 索引（fast 模式）+ 源码核验整理。行号以当前 `Playerbot` 分支 HEAD 为准；
> `src/server/scripts`（内容脚本）与 `modules/`（含 mod-playerbots）不在索引范围，但本文主题的核心引擎
> 全部位于 `src/server/game/`，覆盖完整。上游 `doc/` 目录不含此部分架构说明。

NPC 提供的服务（对话、买卖、维修）与任务，在 AzerothCore 中由同一套「数据驱动 + 协议处理器」分层结构实现。

---

## 一、总体架构：数据驱动 + 协议处理器

所有 NPC 交互都是同一套三层结构：

```
world DB 静态数据 ──启动加载──▶ ObjectMgr 内存 store（模板/缓存）
                                     ▲
客户端 opcode 包 ──▶ WorldSession::Handle*Opcode（Handler 层）
                                     ▼
                          Player / Creature 实体逻辑（校验 → 状态变更 → 回包）
```

NPC 能提供什么服务，由 `creature_template.npcflag` 位掩码决定，枚举定义在
`src/server/game/Entities/Unit/UnitDefines.h:322` 起：

| 术语（NPC flag） | 值 | 含义 |
|---|---|---|
| `UNIT_NPC_FLAG_GOSSIP` | 0x1 | 有对话菜单 |
| `UNIT_NPC_FLAG_QUESTGIVER` | 0x2 | 任务 giver |
| `UNIT_NPC_FLAG_VENDOR` | 0x80 | 商人（可买卖） |
| `UNIT_NPC_FLAG_REPAIR` | 0x1000 | 可修理（术语叫 **Armorer**，见 `Unit::IsArmorer()`） |

所有交互入口都先过同一个守门函数 `Player::GetNPCIfCanInteractWith(guid, npcflagmask)`
（`src/server/game/Entities/Player/Player.cpp:2067`）：统一校验距离、存活、阵营、
**NPC 是否带对应 flag**——这是防作弊/WPE 的第一道关卡。

---

## 二、NPC 服务：对话（Gossip）是入口

维修和买卖不是独立的交互，而是**先打开 gossip 菜单，再点菜单项进入对应服务窗口**。

### 2.1 打开菜单

1. 右键 NPC → 客户端发 `CMSG_GOSSIP_HELLO` / `CMSG_QUESTGIVER_HELLO`
2. `WorldSession::HandleQuestgiverHelloOpcode`（`src/server/game/Handlers/QuestHandler.cpp`）
   → `Player::PrepareGossipMenu(source, menuId, showQuests)`（`src/server/game/Entities/Player/PlayerGossip.cpp`）
3. `PrepareGossipMenu` 是菜单组装中枢：
   - 从 `gossip_menu` / `gossip_menu_option` **DB 表**取菜单项（经 `ObjectMgr` 的 `GetGossipMenuItemsMapBounds`）；
   - 每个菜单项的 `OptionType` 对应 `Gossip_Option` 枚举（`src/server/game/Entities/Creature/GossipDef.h`，
     如 `GOSSIP_OPTION_VENDOR=3`），并用条件系统（`sConditionMgr->IsObjectMeetToConditions`）+ NPC flag 过滤能否显示；
   - 若 NPC 是 `UNIT_NPC_FLAG_QUESTGIVER`，还会调 `Player::PrepareQuestMenu`（`PlayerQuest.cpp`）
     把可接/可交的任务填进 **QuestMenu**。
4. `Player::SendPreparedGossip` → `PlayerMenu::SendGossipMenu`（`src/server/game/Entities/Creature/GossipDef.cpp`）
   组 `SMSG_GOSSIP_MESSAGE` 回包。

**术语**：`PlayerTalkClass` / `PlayerMenu` 是玩家侧的「对话会话」对象（Player 的成员，类型为 `PlayerMenu`），
内部含 `GossipMenu`（选项菜单）和 `QuestMenu`（任务列表）两个容器。

### 2.2 点菜单项 → 分发到具体服务

客户端发 `CMSG_GOSSIP_SELECT_OPTION` → `HandleGossipSelectOptionOpcode`
（`src/server/game/Handlers/MiscHandler.cpp`）→ 最终在 `PlayerGossip.cpp:324-331` 按 `OptionType` 分发：

```cpp
case GOSSIP_OPTION_QUESTGIVER:
    PrepareQuestMenu(guid); SendPreparedQuest(guid); break;
case GOSSIP_OPTION_VENDOR:
case GOSSIP_OPTION_ARMORER:                       // 维修+买卖同窗口
    GetSession()->SendListInventory(guid, menuId); break;
```

### 2.3 买卖（Vendor）

**数据加载**：启动时 `ObjectMgr::LoadVendors`（`src/server/game/Globals/ObjectMgr.cpp:10268`）
读 **`npc_vendor` 表**（字段 `entry, item, maxcount, incrtime, ExtendedCost, slot`；item 为负数表示
「引用模板」即批量复用的商品列表 `LoadReferenceVendor`），校验后存入 `_cacheVendorItemStore`
（`VendorItemData` 列表，元素 `VendorItem`，定义在 `src/server/game/Entities/Creature/CreatureData.h`）。

**运行时流程（买）**：

1. `SMSG_LIST_INVENTORY` 由 `WorldSession::SendListInventory`（`src/server/game/Handlers/ItemHandler.cpp`）发出：
   校验 `UNIT_NPC_FLAG_VENDOR` → 取 `sObjectMgr->GetNpcVendorItemList()` 组包
   （同时触发 hook `OnPlayerSendListInventory`）。
2. 玩家点购买 → `CMSG_BUY_ITEM` → `HandleBuyItemOpcode` / `HandleBuyItemInSlotOpcode`
   → **`Player::BuyItemFromVendorSlot`**（`src/server/game/Entities/Player/Player.cpp:10734`）：
   - 校验：商品在列表内、`ItemExtendedCost`（荣誉/徽章等代币消耗）、金钱 `ModifyMoney`、
     背包空间 `CanStoreNewItem`；
   - hook `OnPlayerBeforeBuyItemFromVendor` 允许脚本改价/否决；
   - `StoreNewItem` 入包，更新限时库存。
3. **限时库存**术语：`maxcount`（限量份数）+ `incrtime`（恢复间隔）→ 运行时由
   `Creature::GetVendorItemCurrentCount` / `UpdateVendorItemCurrentCount`（`Creature.cpp:3193/3228`）
   做补货计数。

**运行时流程（卖）**：`CMSG_SELL_ITEM` → `WorldSession::HandleSellItemOpcode`
（`src/server/game/Handlers/ItemHandler.cpp:578`）：按 `ItemTemplate.SellPrice` 计价（按耐久度折价），
扣物品、加钱，并把物品放入 **buyback slot**（`Player::AddItemToBuyBackSlot`，`PlayerStorage.cpp`）——
这就是「回购」机制，`GetItemFromBuyBackSlot / RemoveItemFromBuyBackSlot` 支撑买回。

### 2.4 维修（Repair）

`CMSG_REPAIR_ITEM` → `WorldSession::HandleRepairItemOpcode`
（`src/server/game/Handlers/NPCHandler.cpp:760`）：

```
GetNPCIfCanInteractWith(npcGUID, UNIT_NPC_FLAG_REPAIR)   // flag 校验
→ GetReputationPriceDiscount(unit)                        // 声望折扣（专属/友好/尊敬…打折系数）
→ hook OnPlayerBeforeDurabilityRepair
→ Player::DurabilityRepair(pos, true, discountMod, guildBank)   // 修单件
   Player::DurabilityRepairAll(...)                             // 修全部
```

计价基于每件物品的耐久损耗（`Item` 的 durability 字段）× 物品修理费基数 × 声望折扣；
`guildBank=1` 时走公会银行付款。买卖同理可享 `GetReputationPriceDiscount` 折扣（购买价 `BuyPrice`）。

---

## 三、NPC 任务（Quest）系统

### 3.1 数据层（world DB → 内存）

| DB 表 | 加载函数 | 内存结构 |
|---|---|---|
| `quest_template` (+`quest_template_addon`) | `ObjectMgr::LoadQuests`（`ObjectMgr.cpp:5080`） | `Quest` 对象（`src/server/game/Quests/QuestDef.h`），含目标、奖励、flags、`QUEST_SPECIAL_FLAGS_*` |
| `creature_questrelation` / `gameobject_questrelation` | `LoadQuestRelationsHelper` | `_creatureQuestRelations` 等 —— **quest starter**（谁发这个任务） |
| `creature_involved_questrelation` / `gameobject_involved_questrelation` | 同上 | `_creatureQuestInvolvedRelations` —— **quest finisher**（谁能收这个任务） |

starter/finisher 是本项目的关键术语对：`PrepareQuestMenu` 用 `GetCreatureQuestRelationBounds`（发任务）
与 `GetCreatureQuestInvolvedRelationBounds`（交任务）分别列出；`Unit::hasQuest` / `hasInvolvedQuest`
则用于防伪造校验。

### 3.2 玩家侧状态（characters DB）

- **`QuestStatus`** 枚举（`QuestDef.h`）：`QUEST_STATUS_NONE / INCOMPLETE / COMPLETE / FAILED / REWARDED`；
- **`QuestStatusData`**：单个任务在玩家身上的进度（各 objective 的计数、Explored 等），
  存于 `Player::m_QuestStatus`（map）；已奖励过的记录在 `m_RewardedQuests`；
- 持久化表：`character_queststatus`（进行中，prepared statement `CHAR_REP_CHAR_QUESTSTATUS`）、
  `character_queststatus_rewarded`，外加 daily/weekly/monthly/seasonal 各自的状态表
  （`CHAR_INS_CHARACTER_DAILYQUESTSTATUS` 等）；登录时 `_LoadQuestStatus`、保存时 `_SaveQuestStatus` 装载/回写。

### 3.3 交互全流程（按玩家视角的时序）

```
① 头顶任务标记
   CMSG_QUESTGIVER_STATUS_QUERY → HandleQuestgiverStatusQueryOpcode
   → Player::GetQuestDialogStatus → SMSG_QUESTGIVER_STATUS（! / ? / 灰? …）

② 右键 NPC 打开菜单（同 2.1，showQuests=true）
   → Player::PrepareQuestMenu（PlayerQuest.cpp）：
      遍历该 NPC 的 starter/finisher relation，
      用 GetQuestStatus + CanTakeQuest + IsAutoComplete/IsRepeatable 过滤
      → QuestMenu.AddMenuItem

③ 点任务名看详情
   CMSG_QUESTGIVER_QUERY_QUEST → HandleQuestgiverQueryQuestOpcode
   → PlayerMenu::SendQuestGiverQuestDetails（GossipDef.cpp，SMSG_QUESTGIVER_QUEST_DETAILS）
   （IsAutoAccept 任务直接跳到接受）

④ 接受
   CMSG_QUESTGIVER_ACCEPT_QUEST → HandleQuestgiverAcceptQuestOpcode（QuestHandler.cpp:113）
   → CanInteractWithQuestGiver（防 WPE）→ CanTakeQuest → CanAddQuest
   → Player::AddQuestAndCheckCompletion → Player::AddQuest（PlayerQuest.cpp）：
       · 新建 QuestStatusData（按 QUEST_SPECIAL_FLAGS_KILL/CAST/SPEAKTO/DELIVER 初始化 objective 计数）
       · GiveQuestSourceItem（发「任务物品」如信件/道具）
       · 写 m_QuestStatus + 立即存 DB
   → hooks: sScriptMgr->OnPlayerQuestAccept、CreatureAI::QuestAccept（SmartAI 的 sQuestAccept）
   （QUEST_FLAGS_PARTY_ACCEPT 时向队友发 SendQuestConfirmAccept 确认共享）

⑤ 进度记名（quest credit）—— 各类事件回调进 Player：
   · 杀怪：Unit::Kill（Unit.cpp:14118 RewardPlayerAndGroupAtKill）→ Player::KilledMonster（PlayerQuest.cpp:1951）
   · 拾取物品：Player::ItemAddedQuestCheck（:1871）
   · 与 NPC 对话：Player::TalkedToCreature（:2148）
   · 探索/事件触发：Player::AreaExploredOrEventHappens（:1827）
   每次 credit → 内部判定达标 → Player::CanCompleteQuest → QUEST_STATUS_COMPLETE → 发 QuestUpdateComplete

⑥ 交任务
   CMSG_QUESTGIVER_COMPLETE_QUEST → HandleQuestgiverCompleteQuest
   → SendQuestGiverRequestItems（「需要物品」核对界面）
   CMSG_QUESTGIVER_CHOOSE_REWARD → HandleQuestgiverChooseRewardOpcode（QuestHandler.cpp）
   → Player::RewardQuest（PlayerQuest.cpp:662）：
       · 移除 RequiredItemCount 任务物品
       · 发奖励：RewardChoiceItemId（多选一）、金钱、GiveXP、声望、RewardItem、RewardSpell
       · 记入 m_RewardedQuests、解锁任务链（SatisfyQuestNextChain/PrevChain）
   → hook OnQuestReward / OnPlayerCompleteQuest
```

**前置条件族**：`CanTakeQuest` 由一组 `SatisfyQuest*` 谓词组成（`PlayerQuest.cpp:950-1361`）：
`SatisfyQuestLevel / Class / Race / Skill / Reputation / PreviousQuest / ExclusiveGroup / Breadcrumb /
Day / Week / Seasonal / Month / Timed …`——术语上这就是任务「接取资格检查」的完整清单。

### 3.4 脚本扩展点（ScriptMgr hooks）

核心流程在 `src/server/game/` 内置钩子，模块/C++ 脚本通过 `ScriptDefines/*.h` 的接口介入，
任务与买卖相关主要有：

- `PlayerScript::OnPlayerQuestAccept / OnPlayerCompleteQuest / OnPlayerBeforeBuyItemFromVendor / OnPlayerBeforeDurabilityRepair`
- `CreatureScript::OnQuestAccept / OnQuestReward / CanCreatureQuestAccept`
- `AllItemScript::OnQuestAccept`
- SmartAI 对应事件：`SmartAI::QuestAccept`

（均在 `src/server/game/Scripting/ScriptDefines/`。）本仓库的 mod-playerbots 模块正是大量挂这些
hook 来驱动 bot 行为的——bot 的「找 NPC 买东西/交任务」复用的就是上述同一条核心流程。

---

## 四、术语速查表

| 术语 | 本项目含义 | 代表符号/位置 |
|---|---|---|
| Creature | NPC 实体（Unit 子类） | `src/server/game/Entities/Creature/` |
| npcflag | NPC 服务能力位掩码 | `UnitDefines.h` `UnitNPCFlags` |
| Gossip | NPC 对话菜单系统 | `gossip_menu(_option)` 表、`PlayerMenu`、`GossipDef.cpp` |
| Vendor | 商人（买卖） | `npc_vendor` 表、`VendorItemData`、`SendListInventory` |
| ExtendedCost | 代币价（荣誉/徽章） | `ItemExtendedCost`、`npc_vendor.ExtendedCost` |
| Buyback slot | 卖出后回购槽 | `Player::AddItemToBuyBackSlot` |
| Durability / Repair | 耐久/修理 | `HandleRepairItemOpcode`、`DurabilityRepairAll` |
| Reputation discount | 声望折扣系数 | `GetReputationPriceDiscount` |
| Quest starter / finisher | 发任务者 / 收任务者 | `creature_questrelation` / `creature_involved_questrelation` |
| QuestStatus / QuestStatusData | 任务状态机 / 玩家进度 | `QuestDef.h`、`Player::m_QuestStatus` |
| Quest credit | 目标进度记名 | `KilledMonster / ItemAddedQuestCheck / TalkedToCreature` |
| SatisfyQuest* | 接取资格谓词族 | `PlayerQuest.cpp:950+` |
| Opcode Handler | 客户端包处理函数 | `WorldSession::Handle*Opcode`，`src/server/game/Handlers/` |
