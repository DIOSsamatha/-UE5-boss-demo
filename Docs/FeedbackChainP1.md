# P1 反馈链重构 —— 第一批落地（2026-09-25）

> 四段式：设计理由（补哪个反馈链节点）→ 根因假设 → 改法（文件/类/函数）→ 验收标准。
> 参考基准：`Docs/ReferenceFrames/`（B 站 BV1bZVM6kEGu），UI 规格见 `ref_004.jpg` / `ref_007.jpg`。

---

## 1. 双异常条（P1-1）✅ 已实现并编译

**设计理由 —— 补哪个节点**
反馈链最缺的一环是"**输出 → 看得见的进度**"。原设计里玩家砍 BOSS 只喂养了两个系统：
血条（慢，读不出节奏）和韧性（隐藏，UI 上只有破防那一刻才可见）。
中间那整段"我到底打出进展没有"是空白 —— 参考 Demo 用两条异常槽把它填上了。

**根因假设**
1. `DemoStatusEffects` 的 7 种效果（燃烧/中毒/流血/易伤/减速/攻升/防升）都是"挂上去就按周期结算"的
   **结果型效果**，没有一条是**积累型资源**，所以不存在"打满 → 状态跃迁"的台阶。
2. 没有任何属性承担"可见进度条"的职责 → 打满了也没有信号可发。

**改法**

| 层 | 文件 / 类 | 内容 |
|---|---|---|
| 标签 | `Core/DemoGameplayTags.{h,cpp}` | 新增 `State_Unstable`（失衡）；`Data_AnomalyImpact` / `Data_AnomalyElement`（SetByCaller 键） |
| 属性 | `Core/DemoAttributeSet.{h,cpp}` | 新增 4 个属性：`AnomalyImpact` / `MaxAnomalyImpact`（橙·冲击）、`AnomalyElement` / `MaxAnomalyElement`（青·元素）。`PreAttributeChange` 夹 0~Max；`PostGameplayEffectExecute` 判定槽满 |
| 阈值 | `Core/DemoCombatTuning.h` | 容量 100/100；积累效率表；`UnstableDamageBonus=1.25`、`UnstableDuration=6s`、`AnomalyBrokenWeakDuration=3.5s` |
| GE | `Effects/DemoGameplayEffects.{h,cpp}` | 新增 `UGE_AnomalyGainEffect`（Instant，两个 SetByCaller Additive 通道） |
| 入口 | `Core/DemoAbilitySystemLibrary.{h,cpp}` | 新增 `ApplyAnomalyGain()`；在 `ApplyHit` 的两处调用 |
| 闸门 | `Core/DemoCharacterBase.{h,cpp}` | 新增 `bHasAnomalyBars`（只有 BOSS 开）+ `HasAnomalyBars()` + `ClearAnomalyBars()` |

**两条槽的喂养规则（关键：逼玩家两种手段都用）**

| 手段 | 冲击槽(橙) | 元素槽(青) |
|---|---|---|
| 普攻连段（削韧 14） | +7 | +0（第 3/4 段起才给，当前按第 2 段基准） |
| 技能（削韧 50） | +10 | +18 |
| 普通弹反 | 0 | +22 |
| **完美弹反** | 0 | **+30** |

> 于是"只砍不弹"永远推不动第二条槽，"只弹不砍"推不动第一条 —— 读招与输出被强制耦合。

**状态跃迁**
- 冲击槽满 → `State_Unstable`（失衡，绿轮廓 + 受伤 1.25x，持续 6s，到期清槽）
- 元素槽满 **且** 已失衡 → `State_Broken`（击破）；`HandleAnomalyBroken()` 复用成熟的
  `HandlePoiseBroken()` 链路（硬直 + 广播 + 到点自动恢复），同时退失衡 + 清槽

**为什么用 GE 而不是直接改属性**：直接 `SetAnomalyImpact` 绕过 `PostGameplayEffectExecute`，
"打满 → 挂 tag"这条链不会触发，UI 与材质两条通道全哑火；且跨端不复制。
**为什么清空用 `SetNumericAttributeBase` 而不是再发一个 GE**：那是"重置"不是"结算"，
走 GE 会在清零的瞬间又判定一次"打满"→ 状态自激。

**验收标准**
- 开 `demo.DamageLog 1`，砍 BOSS 应看到 `[异常] ... 冲击+7.0 元素+0.0` 逐次累加。
- 冲击槽满时出现 `[Boss] 冲击异常条打满 → 进入失衡（绿轮廓 + 受伤加成 1.25x，持续 6.0s）`。
- 失衡期间 BOSS **仍能行动**（不是硬控），但伤害数字明显变大。
- 失衡期间把元素槽打满 → BOSS 进虚弱窗口（`Boss_Weak_*`），两条槽清零，轮廓恢复。

---

## 2. 状态双通道信号（P1-2）✅ 两条通道都已实现并编译

**设计理由**：状态必须"UI 标签 + 材质变色"双通道同时到。
玩家在打的时候视线在 BOSS 身上，看 HUD 标签是低效的；但转头看别处时材质又不生效。

**已实现（材质通道）**
- `ABossCharacterBase::UpdateUnstableVisual()`（`Tick` 中调用）：`State_Unstable` 期间把
  `MI_BossUnstable` 挂到 `Mesh->SetOverlayMaterial`，1.6Hz 缓慢呼吸（**刻意不快闪** ——
  快闪是"起手预警"的视觉语言，两者必须区分）；退出即摘掉。
- 材质兜底路径 `/Game/Boss/VFX/MI_BossUnstable`，由 `Tools/SetupBossVFX.py` 生成（已加绿色实例）。
  拿不到时优雅降级：只留 HUD 那一条通道，不会把 BOSS 涂黑。

**已实现（UI 通道）**

| 项 | 位置 | 说明 |
|---|---|---|
| 两条槽 | `UBossHealthBarWidget::EnsureDefaultTree()` | 血条正下方、韧性条之上：橙=`BossAnomalyImpactBar`（冲击·失衡）、青=`BossAnomalyElementBar`（元素·击破），各带固定标签说明"谁喂它" |
| 读数 | `SyncAnomalyBars()`（20Hz，与血/韧同节拍） | `SetValueAndMax`；慢变量不需要每帧刷，逐帧刷只是白白让 Slate 重绘 |
| 状态标签 | `SyncAnomalyStateLabel()`（**每帧**，`TickHUD`） | `UNSTABLE`（绿，**与 BOSS 轮廓同色**，两条通道互相印证）/ `BROKEN —— 失去格挡`（橙红）；只在跃迁时改文字与颜色 |
| 读数出口 | `ADemoCharacterBase::GetAnomalyImpact()` 等 4 个 getter | ⚠ 定义在 .cpp：头文件只有 `UDemoAttributeSet` 前向声明，内联解引用会 C2027 |

**验收标准**
- 砍中 BOSS：橙槽一小格一小格地涨；弹反成功：青槽明显跳一大截（22/30）。
- 失衡瞬间：BOSS 全身绿轮廓呼吸 **+** HUD 橙槽满格 **+** `UNSTABLE` 绿色标签（三件事同一帧发生）。
- 击破瞬间：绿轮廓撤掉 + 橙/青两槽清零 + `BROKEN —— 失去格挡` 标签 + BOSS 进虚弱动作。
- 6s 后失衡自然到期：标签消失、两槽归零，不需要玩家做任何事。

---

## 3. 架势条（P1-3）✅ 已实现并编译

**设计理由 —— 补哪个节点**

参考 Demo（`ref_011.jpg`）："架势值过低时格挡会被 Boss 破防，进入硬直且受到部分伤害"。
它补的是**防御手段在反馈链里零产出**这个洞：

- 没有架势条 → 举盾是免费的，最优解永远是按住不放，弹反和闪避没人用；
- 有了架势条 → 挡是"借"，必须靠完美闪避 / 完美弹反（稳心）还回去，
  于是【挡 / 弹 / 闪】三种防御手段第一次有了相互约束的关系 —— 这才是"高频交互"。

**根因假设**
防御侧只有 `State_Blocking`（伤害 ×0.25）一个开关，没有任何消耗/偿还机制，
所以"一直挡"不会累积任何代价，玩家没有理由去学弹反和闪避。

**改法**

| 层 | 文件 / 类 | 内容 |
|---|---|---|
| 属性 | `Core/DemoAttributeSet.{h,cpp}` | 新增 `Posture` / `MaxPosture`；夹 0~Max；归零广播 `OnPostureBroken` |
| 阈值 | `Core/DemoCombatTuning.h` | `PostureMax=100`；`BlockPostureCostBase=8` + `PerDamage=0.25`；`PerfectDodgePostureRefund=25`；`PerfectParryPostureRefund=18`；`GuardBreakStaggerTime=1.0s`；`GuardBreakDamageScale=0.6`；`GuardBreakPostureRecoverRatio=0.5` |
| GE | `Effects/DemoGameplayEffects.{h,cpp}` | `UGE_PostureChangeEffect`（Instant，SetByCaller `Data_Posture`） |
| 入口 | `Core/DemoAbilitySystemLibrary.{h,cpp}` | `ApplyPostureChange()` / `ApplyDamageOnly()`；三处调用 |
| 处理 | `Core/DemoCharacterBase.{h,cpp}` | `HandlePostureBroken()` / `RecoverFromPostureBreak()` |
| UI | `UI/CombatHUDWidget.{h,cpp}` | 玩家状态区新增 `PostureBar`（金；低于 30% 整条转红） |

**三条接入点**
1. **格挡扣**（`ApplyHit` 的 `bBlocked` 分支）：`-(8 + 伤害 × 0.25)`。
   带伤害项是为了教会玩家"重招别硬挡" —— 只扣固定值的话挨小招和挨大招代价一样。
2. **完美闪避回 +25**（最窄窗口、最高风险 → 给最多）。
3. **完美弹反回 +18**（稳心）：比闪避少，因为弹反已经拿到"额外削韧 + 攻击方硬直 + 推进元素槽"三重产出，
   这项必须让位，否则弹反会全面优于闪避。

**被破防的组合拳**（`HandlePostureBroken()`）
- 挂 `State_Broken`：① `ApplyHit` 靠它判断"这下格挡被打破"，补上 25%→60% 的伤害差额；
  ② `DemoDamageExecution` 里破防受伤 ×1.5 → 破防后的几刀会很疼，玩家下次才会更慎重。
- `ApplyStagger(this, true)`：硬直（参考 Demo 的"进入硬直"）。
- `CancelAbilities(Ability.Defense.Block)`：**把盾打掉**。少了这步会出现"破防了松开再按又能立刻挡"的手感 bug。
- 1s 后 `RecoverFromPostureBreak()`：摘 `State_Broken` + 架势回 **半槽**（代价延续到起身之后，但不会死亡螺旋）。

**验收标准**
- 举盾挨打：金色架势条逐格下降，日志 `[架势] Player：-12.5（当前 87.5 / 100）`。
- 完美闪避 → 条明显回一截（+25）；完美弹反 → +18。
- 架势见底（≤30%）→ **整条转红**，给"快挡不住了"的预警。
- 打空瞬间：日志 `[架势] 架势被打空 → 被破防`，角色进硬直、盾被取消、这一下吃 60% 伤害而不是 25%。
- 1 秒后自动起身，架势回到 50%。

---

## 4. 拼刀 / 稳心（P1-3b）✅ 已实现并编译

**设计理由 —— 补哪个节点**
拼刀是**第三种防御性交互**，和弹反 / 闪避平级。它补的是"挡不住也来不及闪"这个空档：
之前玩家在这个时刻只能挨打，现在有一个正面解法 —— 迎着对方的刀砍上去。

**根因假设**
没有任何"双方同时出手"的判定：攻击命中只会走"伤害 + 硬直"这一条路，
于是正面对撞和偷袭的结果完全一样，玩家没有理由去赌对撞。

**改法**

| 层 | 文件 / 类 | 内容 |
|---|---|---|
| 时间戳 | `Core/DemoCharacterBase.{h,cpp}` | `MarkSwing()` / `HasRecentSwing()` / `HasRecentClash()` / `MarkClash()`；`PerformMeleeTrace()` 开头打戳 |
| 判定 | `Core/DemoAbilitySystemLibrary.cpp` | `ApplyHit` 中：双方都在 `ClashWindow` 内挥过刀 **且** `bCanBeParried`（只认刀剑近战）→ `ResolveClash()` |
| 处理 | `Core/DemoAbilitySystemLibrary.cpp` | `ResolveClash()`：抵消硬直 + 回架势 + 火花 + 顿帧 + `Event_Clash` |
| 标签 | `Core/DemoGameplayTags.{h,cpp}` | `Event_Clash` |
| 阈值 | `Core/DemoCombatTuning.h` | `ClashWindow=0.15s`、`ClashPostureRefund=15`、`ClashCooldown=0.6s`、`ClashHitStop=0.10s@0.10` |

**两个关键实现细节**

1. **判定基于"正在挥刀"而不是"处于出招状态"**。
   `PerformMeleeTrace()` 被调用 = 攻击判定窗正在生效，这是"出手"的精确定义；
   只看 `State_Attacking` 会把"我刚起手、对方已经收招"也算成拼刀，玩家会觉得判定很水。

2. **抵消硬直必须移除 GE，不能只摘 tag**。
   硬直是 GE 授予的 tag，`RemoveLooseGameplayTag` 会在下一次 GE 刷新时被写回，
   表现为"拼刀了但还是僵在原地"。正确做法是
   `ASC->RemoveActiveEffectsWithGrantedTags({State_Stagger})`。
   调用点在伤害结算**之后** —— 要抵消的是"这一下打出来的硬直"，顺序反了就抵消了个寂寞。

**数值为什么这么给**
`ClashPostureRefund=15` **低于**完美闪避(25)和完美弹反(18)：拼刀是"撞上了"而不是"操作对了"，
回报必须低于两种完美操作，否则玩家会去赌对撞而不是去读招。
`ClashCooldown=0.6s` 防止连段每一刀都稳心、把硬直系统架空。

⚠ **一处需要实战校准的取舍**：目前拼刀抵消的是**双方**硬直（含 BOSS 的受击硬直），
这会略微削弱玩家的压制力。如果测下来觉得"拼刀太便宜了 BOSS"，
把 `ResolveClash()` 里对 `TargetActor`（挨打方）的硬直抵消去掉即可 —— 一行的事。

**验收标准**
- 迎着 BOSS 的挥砍出刀 → 日志 `[拼刀] Player ⇄ Boss 稳心：抵消硬直 + 架势 +15.0（冷却 0.6s）`。
- 两刀相交处爆出金属火花 + 一次 0.10s 定帧。
- 拼刀后角色不进入受击硬直（本来会被打断的动作接得下去）。
- 连段里不会每刀都触发（0.6s 冷却生效）。

---

## 5. BOSS 默认格挡（P1-4）✅ 已实现并编译

**设计理由 —— 补哪个节点**
这是让前面那套异常条真正产生意义的那一环。没有它，玩家随时都能把伤害打进去，
"击破窗口"和"后摇惩罚窗口"就都没有稀缺性 —— 既然站着砍也行，为什么要费劲打满异常条？
整条反馈链会因此塌掉一半。有了它，BOSS 才从沙袋变成博弈对象。

**根因假设**
`State_Blocking` 只被玩家的格挡技能使用，BOSS 侧从来不挂这个 tag，
于是 `ApplyHit` 的格挡分支（伤害 ×0.25 + 扣架势）对 BOSS 永远走不到。

**改法**

| 层 | 文件 / 类 | 内容 |
|---|---|---|
| 举盾开关 | `Boss/BossCharacterBase.{h,cpp}` | `UpdateGuardStance()`（Tick）：按大脑状态挂/摘 `State_Blocking` |
| 事件 | `Core/DemoCharacterBase.{h,cpp}` | 新增 `OnBlockedIncomingHit`（BlueprintNativeEvent），由 `ApplyHit` 格挡分支调用 |
| 反制 | `Boss/BossCharacterBase.cpp` | 重写该事件：累计 `BlockedStreak`，≥ `BossCounterAfterBlocks` → `Brain->RequestRevengeAttack()` |
| 阈值 | `Core/DemoCombatTuning.h` | `BossCounterAfterBlocks=3`；`BossPostureMax=180`（BOSS 容量单独给，和玩家一样会太脆） |

**举盾时机是设计出来的，不是漏出来的**

只在中立状态举盾：**Idle / Chase / Reposition**。
起手(Telegraph) / 出招(Attack) / 后摇(Recover) / 硬直(Stagger) / 击破(Broken) **一律不挡** ——
这几段时间天然就是玩家的输出窗口。玩家要做的事于是变成"**读它什么时候在做事**"，而不是无脑贴脸。

**三条玩家解法（对应反馈链的三个出口）**
1. **异常击破**：冲击槽满 → 失衡（不挡 + 受伤 1.25x）→ 元素槽满 → 击破（虚弱 3.5s，完全不挡）
2. **后摇/出招窗口**：BOSS 在做事的时候打进去（不需要任何资源，纯读招）
3. **打破它的架势**：连续猛攻把 BOSS 的架势也打空 → 被破防（和玩家同一套规则，对称）

**反制的必要性**
只挡不反 = 玩家的最优解还是"贴脸砍到它抬手为止"（因为挨挡只是伤害少，没有别的代价）。
连续挡下 3 次就强制还手一次，才让"被挡"真正有代价。
同时每次被挡会在 BOSS 身前爆一团**冷蓝火花**（和受击的白/橙区分开）——
否则玩家只看到伤害数字变小，会读成"BOSS 血厚 / 我打不动"，而不是"它在防御"。

**验收标准**
- BOSS 站着不动/追你/走位时砍它 → 伤害只有 25%，身前爆冷蓝火花，日志可见。
- BOSS 起手/出招/后摇/硬直/虚弱时砍它 → **全额伤害**（不再被挡）。
- 连续被挡 3 次 → 日志 `[Boss] 连续挡下 3 次 → 反制还手`，BOSS 立刻起手还击。
- 站桩输出不再是最优解：不打满异常条/不抓窗口的话，输出会被压到 1/4。

---

## 6. 格挡表现层（P1-4b）✅ 已实现并编译

**设计理由 —— 补哪个节点**
P1-4 让 BOSS 会挡了，但"它在防御"这件事当时只有**一条通道**：伤害数字变小。
而伤害数字是最不可靠的读数——血厚、格挡、抗性都会让数字变小，玩家无法归因。
所以必须补上视觉和听觉两条独立通道。

**根因假设**
`State_Blocking` 是一个纯数据 tag，没有任何表现层订阅它：不播音效、不挂材质、不点亮光。
玩家只能靠"数字变小"去猜，归因错误会直接导致"这游戏打不动"的挫败感。

**改法**

| 通道 | 实现 | 说明 |
|---|---|---|
| 轮廓（视觉·持续） | `UpdateGuardVisual()` → `Mesh->SetOverlayMaterial(MI_BossGuard)` | 冷蓝描边，举盾期间常驻，退出即摘 |
| 护体光（视觉·持续） | `GuardLight` 常驻点光，0.7Hz 极慢呼吸 | 举盾是"状态"不是"冲击"，闪快了会被误读成起手预警 |
| 爆闪（视觉·瞬间） | 被挡时 `GuardImpactFlashRemaining=0.25s` 额外提亮 ×4 | 给"叮"的一下一个明确的视觉落点 |
| 火花（视觉·瞬间） | `ADemoImpactBurst` 冷蓝爆闪（P1-4 已有） | 位置在 BOSS 身前、朝攻击者 |
| 音效（听觉） | `BlockSound` → `UGameplayStatics::PlaySoundAtLocation` | 玩家盯着脚下时，声音仍能告诉他"被挡了" |

**四种状态、四种颜色（这条是整套状态信号规范的底色）**

| 状态 | 颜色 | 触发 |
|---|---|---|
| 起手预警 | 🔴 红（**快闪**） | Telegraph |
| 受击 | ⚪ 白（一次脉冲） | 任何命中 |
| 失衡 UNSTABLE | 🟢 绿（慢呼吸） | 冲击槽满 |
| **举盾 GUARD** | 🔵 冷蓝（极慢呼吸） | 中立状态 |

**关于音效资源**
`BlockSound` 走约定路径 `/Game/Boss/SFX/SC_BossBlock` 兜底加载；
**没有这个音频资源也不会报错**，只是静音 —— 视觉四条通道仍然完整。
（项目里目前没有任何音效资产，所以默认就是静音；拖一个 wav 进来命名 `SC_BossBlock` 即可接上。）

**验收标准**
- BOSS 中立时：全身冷蓝描边 + 身周淡蓝护体光，缓慢呼吸（不是快闪）。
- 砍它一刀：护体光爆亮一下（0.25s 衰减）+ 冷蓝火花 + 撞击音（若有资源）。
- BOSS 起手/出招时：蓝轮廓和护体光**立刻消失** —— 这个消失本身就是"现在可以打"的信号。

---

## 6.5 举盾姿态动画（P1-4c）✅ 已实现并编译

**设计理由 —— 补哪个节点**
P1-4b 之后，"它在防御"有轮廓+护体光+音效+火花四条通道，但 BOSS 的**肢体语言**还是空的：
骨架一直停在普通待机/移动姿势，举不举刀看不出来。对动作游戏来说，"姿态"本身是最直白的
防御信号 —— 玩家扫一眼姿势就知道该不该硬上。这一步把材质+光的代偿换成真正的举刀架住动作。

**根因假设**
`State_Blocking` 只驱动了材质覆盖与点光，没有任何蒙太奇订阅它；
BOSS 举盾时骨架姿态和没举盾时完全一致，所以"防御"缺了最靠前的一条通道（肢体语言）。

**改法**

| 层 | 文件 / 类 | 内容 |
|---|---|---|
| 资产 | `Tools/SetupBossMontages.py` | 新增 `Boss_Guard_Idle_Montage`（取材 `In-Place/Block_Hold_Anim`，**循环**），并加 `LOOP_MONTAGES` 机制给循环蒙太奇设 `LoopSetting=Enabled` |
| 槽位 | `Boss/BossCharacterBase.h` | 新增 `GuardMontage`（约定路径 `/Game/Boss/Animation/Boss_Guard_Idle_Montage`）+ `bGuardPosePlaying` 播放标记 |
| 表现 | `Boss/BossCharacterBase.cpp` | `UpdateGuardStance` 在**站着(Idle)举盾**时播循环举盾蒙太奇；`UpdateGuardVisual` 的轮廓/护体光照旧 |
| 收刀 | `PlayGuardPose()` / `StopGuardPose()` | 播用 `Montage_Play`；停用 `Montage_Stop(0.2s)` 平滑收刀，不"啪"地切回待机 |

**两个必须一起做的决定（否则"举盾姿态"会塌）**

1. **只在中立 Idle 播全身举盾，移动中(Chase/Reposition)不播。**
   全身蒙太奇会抢掉腿部位移，BOSS 一追你就在原地踏步/滑步。移动时的"在防御"靠轮廓+护体光表达，
   已经够读；举刀姿态留给"它站着和你对峙"那个最该看清的时刻。
2. **被格挡的命中不再触发僵直/受击蒙太奇（吸收）。**
   否则每砍一刀 BOSS 就播一次受击硬直、把循环举盾蒙太奇顶掉，看起来在"格挡→挨打→格挡"地抖。
   改动在 `OnDamagedReaction`：挡住的命中跳过白光闪烁/短硬直/四向受击蒙太奇，
   只保留 `OnBlockedIncomingHit` 的冷蓝火花+护体光爆闪+`BlockedStreak` 反制。
   同时被格挡的命中**不计入** `HitsSinceLastAttack` 复仇计数（两条计数器语义分开：
   前者="真打进去了"，后者="一直被挡"），避免你只敲盾时 BOSS 反制得过频。

**验收标准**
- BOSS 站着举盾：手脚举刀架住的循环姿态 + 冷蓝描边 + 护体光（姿态是"防御"最靠前的肢体信号）。
- 砍它一刀：BOSS **不僵直、不抖**，举盾姿态保持；只有冷蓝火花+护体光爆一下。
- BOSS 一起手：举盾姿态 0.2s 平滑收回 + 蓝轮廓和护体光**立刻消失** —— 这个"收回+消失"本身就是"现在可以打"的信号。
- 缺失 `Boss_Guard_Idle_Montage` 资产也不会崩：C++ 按约定路径加载，拿不到就退化成只剩轮廓+光（P1-4b 的代偿）。

---

## 6.6 完美交互窗口（P1-6）✅ 已实现并编译

**设计理由 —— 把"高风险高回报"收口成三档可操作交互**
反馈链里"命中 → 异常 → 失衡 → 破防 → 输出窗口 → 回复"已经闭环，但玩家除了"砍/躲"还缺三种
**主动博弈**动作：完美弹反、完美闪避、拼刀。它们共用同一套哲学——**卡时机才有高回报**，
无脑连按没有用。P1-6 把这三档交互的"窗口判定"和"派生/打断"接上：

1. **完美闪避派生（闪A）**：完美闪避后 0.45s 内按攻击 → 取消闪避后摇直接接普攻/技能。
   普通闪避没有派生窗，逼玩家去"卡出招瞬间"而不是无脑连按。回报 = 慢动作窗口里白捡一刀。
2. **完美格挡只打断"部分"攻击**：近战/弹幕可被完美弹反**打断动画 + 长硬直**；
   但重型招式（**突进斩 / 大招**）`bInterruptibleByParry=false` —— 即使完美弹反也只免伤+奖励，
   招式继续，玩家必须改走闪避。这是"完美格挡只打断部分攻击"的实现依据，避免完美弹反变成无脑 panic 键。
3. **拼刀稳心取消硬直**：双方同窗挥刀 → `ResolveClash` 取消双方硬直 + 回架势（已在 P1 早期实现），
   P1-6 把它的 HUD 表现接上（银白闪 + "稳心!" + 轻镜头冲击），让"撞上稳住了"看得见。

**关键改动**

| 层 | 文件 / 类 | 内容 |
|---|---|---|
| 标签 | `Core/DemoGameplayTags.h/.cpp` | 新增 `State_DodgePerfect` |
| 调参 | `Core/DemoCombatTuning.h` | 新增 `PerfectDodgeFollowupWindow = 0.45s` |
| GE | `Effects/DemoGameplayEffects.h/.cpp` | 新增 `UGE_DodgePerfectFollowupEffect`（挂 `State_DodgePerfect`）；`UGE_PerfectParryStaggerEffect` 去掉对 `Ability_Boss_Attack` 的自动取消（改由代码按"可打断"显式取消） |
| 派生 | `Player/FightPlayerCharacter.cpp` | 普攻/技能输入处理里，若正在闪避且 `State_DodgePerfect` → `CancelAbilities(&DodgeTags)` 取消闪避后接招（`TryPerfectDodgeFollowupCancel`） |
| 门控 | `Boss/BossCombatData.h` | `UBossAttackData` 新增 `bInterruptibleByParry`（默认 true） |
| 门控 | `Boss/BossBrainComponent.cpp` | 突进斩 / 大招 设为 `bInterruptibleByParry=false` |
| 门控 | `Boss/BossCharacterBase.h/.cpp` + `Abilities/BossAbilities.cpp` | `IsCurrentAttackInterruptible()` 查询；起手时写入当前招式标记 |
| 门控 | `Core/DemoAbilitySystemLibrary.cpp` | 弹反分支按 `IsCurrentAttackInterruptible()` 决定"打断+长硬直"还是"只免伤+奖励不冻结" |
| HUD | `UI/CombatHUDWidget.h/.cpp` | 接入 `Event_Clash` → `HandleClashEvent` → `OnClash()`（银白闪 + "稳心!" + 镜头冲击） |

**两个容易踩的坑（已规避）**
- UE5.7 没有 `CancelAbilitiesWithTag`，正确 API 是 `CancelAbilities(const FGameplayTagContainer* WithTags)`（传指针）。
- 派生必须"先取消闪避、再激活攻击"：闪避用 `BlockAbilitiesWithTag` 挡着攻击，不取消就激活不了。

**验收标准**
- 完美闪避后立刻按攻击：闪避后摇被取消，普攻/技能直接出手（闪A），不再等闪避收招。
- 完美弹反近战/弹幕：BOSS 招式被打断 + 1.3s 长硬直（可接一套连击）。
- 完美弹反突进斩/大招：只免伤 + 金闪 + 奖励，**招式继续** → 必须改走闪避（"部分攻击不可打断"成立）。
- 拼刀：双方刀相撞 → 银白闪 + "稳心!"，双方硬直被取消（连段里被撞也不僵在原地）。

---

## 6.7 手感优化三连：体力重构 → 贪刀/Cancel → 锁敌镜头 ✅ 已实现并编译

按"缺口最大 → 手感上限 → 镜头观感"的优先级依次落地。三条的共同原则是：
**惩罚必须看得见但不致命** —— 玩家要能感觉到自己贪了 / 没体力了，而不是被罚到不敢出手。

### ④ 体力系统重构（缺口最大）

**背景**：普攻原本**零消耗**、技能 `StaminaCost=25` **只声明从未施加**、
`PerfectDodgeStaminaRefund=20` 从未被使用（两处完美奖励都回 25）、
**没有任何低体力 / 耗尽标签** —— 体力实际是装饰品，玩家永远打不出"体力见底"这个状态，
"要不要收手"也就根本不是一个决策。

| 层 | 文件 / 类 | 内容 |
|---|---|---|
| 调参 | `Core/DemoCombatTuning.h` | `PlayerComboStaminaCost[4]={8,10,12,16}`、`PlayerEmptyParryStaminaCost=5`、`StaminaLowRatio=0.20`、`StaminaDodgeWeakRatio=0.15`、`StaminaExhaustedTime=2.5`、`StaminaExhaustedRegenPerTick=-1.5`、`StaminaExhaustedDamageScale=0.6`、`DodgeWeakInvincibleTime=0.28`、`DodgeWeakDistanceScale=0.7` |
| 标签 | `Core/DemoGameplayTags.h/.cpp` | 新增 `State_StaminaLow`、`State_StaminaExhausted`、`Data_StaminaCost` |
| GE | `Effects/DemoGameplayEffects.h/.cpp` | 新增 `UGE_StaminaCostEffect`（SetByCaller 通用消耗）、`UGE_StaminaExhaustEffect`（2.5s，净恢复降到 15/s）、`UGE_InvincibleWeakEffect`（低体力短无敌帧）、`UGE_PerfectDodgeRewardEffect`（+20，与弹反 +25 拉开档次） |
| helper | `Core/DemoAbilitySystemLibrary.h/.cpp` | `ApplyStaminaCost()` 统一取负 + 见底自动挂耗尽（避免每个调用点漏掉检测） |
| 施加点 | `Abilities/PlayerAbilities.cpp` | 普攻每段扣（越后越贵）；技能真正扣 25；空弹（举刃未命中）扣 5 |
| 门控 | `Abilities/PlayerAbilities.cpp` | <20% 锁死第 3/4 段；<15% 闪避降级（无敌帧 0.28、距离 ×0.7）；=0 拒绝举刃 + 普攻 ×0.6 |
| HUD | `UI/CombatHUDWidget.cpp` | 低体力时体力条在绿 / 红之间脉动（用世界时间正弦，不额外起计时器） |

**耗尽是"软"惩罚**：不封移动、不硬控，玩家仍可走位拉扯 —— 硬控会把失误变成坐牢。

### ③ Cancel 窗口表 + 贪刀惩罚

| 项 | 实现 |
|---|---|
| 贪刀 | `bOvercommitted`：连段打满一轮后段数绕回 → 后摇 +0.15s、伤害 ×0.7（持续到技能结束） |
| 连闪扑空 | `LastDodgeEndRealTime`（技能是 InstancedPerActor，实例跨次保留）：0.2s 内再闪 → 距离 ×0.6 |
| 误弹重招 | 普通弹反去接**不可打断**重型招式 → 新增 `UGE_MisParryStunEffect`（0.4s）+ 体力 -15 |
| Cancel 补全 | 技能可取消普攻 / 弹刀；普攻、闪避可取消弹刀（此前弹反成功后接招会石沉大海） |

打折来源一律**叠乘**（体力耗尽 0.6 × 贪刀 0.7）而不是二选一 —— "又没体力又不收手"就该更亏。

### ① 绕背预测偏移 + Turn In Place

| 项 | 实现 |
|---|---|
| 绕背预测 | `UpdateLockOnCamera`：横向绕行时焦点沿速度外推，但外推量 = `距离 × tan(LockOnPredictAngleDeg)`（**按恒定角度**，不是写死长度），并做 smoothstep 渐变 + `VInterpTo` 逐帧平滑。**距离仍取真实焦点**，保住 7° 恒定角肩位补偿。**默认 0° = 关闭** |
| Turn In Place | `ULockOnComponent`：夹角 <5° 死区内不强制 `SetActorRotation`（只滤亚度级噪声）；≥5° 平滑转过去 |

**验收标准**
- 普攻 1/2/3/4 段各扣 8/10/12/16；技能扣 25；空弹扣 5；完美闪避回 20、完美弹反回 25。
- 体力 <20% 打不出第 3/4 段；<15% 闪避变短；=0 无法弹反、普攻 ×0.6 但**可自由走位**。
- 连点第 5 击明显变慢变弱；0.2s 内二次闪避扑短；误弹重招硬直 0.4s + 扣体力。
- 绕背跑动时 BOSS 始终在画面内；近距离不抖动；锁定后大角度平滑转身、小角度不抖。

**踩坑记录**
- `PlayerAbilities.cpp` 用 `UDemoAttributeSet::GetStaminaAttribute()` 必须
  `#include "Core/DemoAttributeSet.h"` —— 否则 C2027「使用了未定义类型」+ C3861。
- 空弹惩罚不能直接用 `!bParrySucceeded` 判断：体力耗尽时会**拒绝起手**并立刻 `EndAbility`，
  那条路径上它也是 false，照搬会给"根本没举起刃"也扣一笔 → 用 `bEmptyParryCostArmed` 精确武装。

**返工：绕背预测第一版写错了（已修）**
初版外推写成【固定厘米数】（`Speed×0.12s`，上限 120cm），实机表现为
"贴身打时镜头疯狂晃动 + BOSS 不在身体中心"。两个原因：
- 固定长度对距离极度敏感 —— 贴身 D≈150 时 `atan(120/150)≈39°`，瞄准点被推出 BOSS 身体之外。
  这正是本项目早先在肩位偏移上踩过的同一个坑（**偏移必须按恒定角度缩放**）。
- 夹角阈值是硬开关，阈值附近来回穿越 → 镜头在 0 / 满值之间反复跳。

修法：外推改 `距离 × tan(角度)`；夹角改 smoothstep 渐变；偏移量再逐帧 `VInterpTo`；
**默认角度 0° = 关闭**（瞄准点恒为 BOSS 身体中心）。顺带把 Turn In Place 死区从 35° 收到 5°。

---

## 6.8 受击僵直 + BOSS 固定背板出招 ✅ 已实现并编译

### 玩家受击僵直（新增）

**背景**：BOSS 侧早就有受击短硬直（`BossCharacterBase::OnHitReact` → `UGE_BossHitstunEffect`，
带"未 committing 才施加 / 已僵直则跳过"两道**防连击锁死**守卫），但**玩家侧完全没有** ——
被打中照样继续挥刀，挨打没有任何"被打断"的代价。

| 层 | 文件 | 内容 |
|---|---|---|
| 标签 | `Core/DemoGameplayTags.h/.cpp` | 新增 `State_Hitstun`（独立标签，不复用破防的 `State_Stagger`） |
| 调参 | `Core/DemoCombatTuning.h` | `PlayerHitstunTime = 0.30s` |
| GE | `Effects/DemoGameplayEffects.h/.cpp` | 新增 `UGE_PlayerHitstunEffect`：授予 `State_Hitstun`；**封禁 + 中断** `Ability_Attack_Normal / Skill` |
| 施加 | `Core/DemoAbilitySystemLibrary.h/.cpp` | 新增 `ApplyPlayerHitstun()`；在 `ApplyHit` 伤害落地后、对非 BOSS 目标施加 |

**设计要点**：僵直**只封进攻、不封防御** —— 闪避 / 弹反 / 格挡全部保留。
动作游戏里受击僵直一旦把脱身手段也封了，玩家就会被打成"连击锁死"，
主观感受从"我失误了"变成"这游戏耍赖"。格挡命中也不给僵直（格挡的意义就是挡住但不被中断）。

### BOSS 固定背板出招 + 节奏放缓

**背景**：原来 `PickAttack` 是**加权随机**（`FRandRange`），且间隔很密
（Phase1 1.4~2.6s / Phase2 1.0~1.9s，叠加各招自己的后摇后几乎不停手）→
玩起来"BOSS 一直在出招"；而随机出招让玩家永远只能靠即时反应，**学不会、背不下来**。

| 层 | 文件 | 内容 |
|---|---|---|
| 数据 | `Boss/BossCombatData.h` | `UBossPhaseData` 新增 `AttackPattern`（FName 序列）+ `PatternSkipLimit` |
| 大脑 | `Boss/BossBrainComponent.h/.cpp` | 新增 `PatternCursor`；`PickAttack` 增加**确定性循环**分支（大招仍走自己的冷却门）；换阶段游标归零 |
| 配置 | `Boss/BossBrainComponent.cpp` | Phase1 四拍 `Slash_01→Barrage→Slash_02→ChargeSlash`；Phase2 六拍循环；间隔放缓到 2.2~3.4 / 1.6~2.6 |

**两套逻辑并存**：`AttackPattern` 非空走背板、为空退回加权随机 —— 改行为只需清空/填入数组。
背板里轮到的那招若因冷却/距离不可用，最多往后跳 `PatternSkipLimit` 招去找可用的
（跳过的下一轮会回来，顺序不乱）。

**验收**
- 玩家被命中：正在挥的刀被中断，0.3s 内按不出普攻/技能；但**随时能翻滚 / 弹反**。
- 格挡命中：不掉僵直。
- BOSS 出招严格按背板顺序循环，两次之间有明显空档（不再"贴脸连招不停"）。
- 大招仍按自己的冷却门出现，不破坏背板记忆。

---

## 7. 尚未开工（按规格排序）

| 项 | 内容 | 锚点 |
|---|---|---|
| P1-1b | 连段**逐段**异常效率：现在按"削韧量"粗分普攻/技能，连段 4 段拿同一个值。需要连段技能把段号传进 `ApplyHit` | `UGA_PlayerAttack`（Combo 段号） |
|---|---|---|
| P1-3c | 拼刀**表现层**：专属音效 / 火花拖尾 / HUD "CLASH" 提示 | 复用 `ADemoImpactBurst`；接 `Event_Clash` 即可 |
| P1-5 | **阶段演进事件**：换招式组 + AI 参数（攻击欲望/反击概率/周旋）+ 场景氛围 | 复用 `Event_Boss_UltimateStart` / `State_Boss_Phase2` |

---

## 4. 工程纪律落实

- 数值只落在 `DemoAttributeSet` / `DemoDamageExecution` / `DemoCombatTuning`（本轮全部遵守）。
- 无 `Tick` 内 `GetAllActorsOfClass`；失衡视觉是 tag 查询（`O(1)`），无索敌。
- UPROPERTY 完整：`UnstableMID` 已挂 `UPROPERTY()`（防 GC）。
- 构建：`Tools/run_build.py`（本机 UBT 子进程被安全层秒退 → 自动回退 `ManualBuild.py`）。
