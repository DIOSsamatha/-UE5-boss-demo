# P0 地基审计 + 反馈链基准（2026-09-25）

> 本文档按「设计理由（补反馈链哪个节点）→ 根因假设 → 改法（具体文件/函数）→ 验收标准」四段式写。
> 参考 Demo：B 站 `BV1bZVM6kEGu`《高频交互 Boss 战 Demo》，视频已在工程根目录，
> 抽帧基准见 `Docs/ReferenceFrames/`（脚本 `Tools/ExtractReferenceFrames.py`）。

---

## 0. 回归根因：为什么"声称已交付"的修复在视频里被复现

**结论：不是修复失效，是修复没治到根；且中段有连续多轮改动根本没编译进 DLL。**

| 声称已交付 | 代码是否存在 | 实际生效状态 | 真相 |
|---|---|---|---|
| "BOSS 不飞" | `UGA_BossAttackBase::SetBossGrounded` 存在且在起手/`EndAbility` 成对 | 突进本就是 0.7s 匀速冲刺（日志 `距离=801cm 时长=0.70s 速度=1144cm/s`），**不是瞬移** | "打飞/推着走"的真因是**玩家↔BOSS 胶囊物理互推**（CMC 每帧"去穿插"），位移闸又用 `TeleportPhysics` 拉回 → 视觉瞬移 |
| "锁定锁在身上" | `ULockOnComponent::GetFocusLocation` 用胶囊体 | 焦点=胶囊中心（`FocusHeightRatio=0`） | 偏心来自**相机侧**：弹簧臂 `SocketOffset(0,85,95)` 不被补偿，`atan(95/480)≈11°` 俯仰 + `atan(85/480)≈10°` 偏航 → BOSS 落在画面中心下方+侧边 |
| "打击感已补" | `UDemoHitStopSubsystem`、`ADemoImpactBurst`、`UDemoTrailComponent` 都在 | 顿帧 0.08s@0.02（50× 慢放几乎无感） | **六通道里只有 2 条通**：无相机震屏、无受击闪光；唯一"震屏"是全局时间膨胀，连相机一起慢 |

**工程级教训（已写入长期记忆）**：编辑器开着时 UBT 拒绝**任何** C++ 构建
（`Unable to build while Live Coding is active`，exit 6），不只是"新增 UCLASS 才要关编辑器"。
中段多轮改动因此卡在"没编译"，用户反复看到的是更早的半成品。

---

## 1. 参考 Demo 视觉基准（抽帧规格）

抽帧证据（`Docs/ReferenceFrames/`）：

| 帧 | 时间 | 读到的规格 |
|---|---|---|
| `ref_004.jpg` | 37s | **UI 布局**：左上=玩家 HP(青) + 架势/耐力(黄) 双条；屏幕底部居中=Boss 长血条（红色，两端带 `«` `»` 装饰框）；**血条正下方挂两条独立积累槽**：长条（青）+ 短条（橙），各带一个白色填充游标。字幕："橙色异常状态条满后，敌人进入**失衡**状态，材质轮廓变为**绿色**" |
| `ref_005.jpg` | 81s | "完美格挡会在不影响招式可读性的情况下**打断 Boss 的一些攻击动画**，增强格挡反馈" |
| `ref_007.jpg` | 110s | "**乏力**状态下，如果橙色异常状态被打满，那么 Boss 也会被**击破，失去格挡能力** ↓" |
| `ref_011.jpg` | 169s | "因为**架势值过低格挡**，被 Boss **破防**了，进入硬直且**受到部分伤害**" |
| `ref_014.jpg` | 213s | 阶段 2：场地由冷蓝转**火光/爆炸**、地面出现警戒纹路、Boss 武器与轮廓变色（场景氛围切换） |
| `ref_017.jpg` | 257s | 终局：整屏火光、Boss 倒地、玩家空中追击 |

**可执行规格提炼**

1. 异常资源必须是**两条独立积累槽**，挂在 Boss 血条下方（不是把血条染色）。
2. 状态流转必须有**双通道信号**：UI 浮动标签（UNSTABLE / BROKEN）+ **材质轮廓变色**（失衡=绿）。
3. 异常满 → 失衡；**乏力**（虚弱）叠加异常满 → 击破（失去格挡）。
4. 玩家侧有**架势值**：格挡消耗、过低即破防（进入硬直 + 受部分伤害）。
5. 完美格挡可**打断 Boss 部分攻击动画**。
6. 阶段 2 = 招式组 + AI 参数 + **场景氛围**同时切换。

---

## 2. P0-a：Boss 飞天 / 被推着走

**设计理由（补哪个节点）**：反馈链的"输入→反馈"要求**玩家的每一次站位压迫都产生可读结果**。
BOSS 被玩家身体推着走 = 玩家的"站位"变成了一种无成本输出，且它同时污染了位移可信度
（玩家会认为"这游戏物理是错的"），这是反馈链最底层的可信度问题。

**根因假设**
1. Pawn↔Pawn 默认 `Block`，`UCharacterMovementComponent` 每帧执行"去穿插"（depenetration），
   把重叠双方互相顶开：默认 `MaxDepenetrationWithPawn=32cm` 是**单帧瞬移**（≈1900cm/s）。
2. 只把通道改成 `Overlap` **不够** —— CMC 对 Overlap 响应同样会做去穿插，只是把 32cm/帧 降到 4cm/帧（≈240cm/s），
   所以仍能把 BOSS 推走。
3. 位移安全阀用 `SetActorLocation(..., ETeleportType::TeleportPhysics)` 拉回，反而把"慢慢被推"变成"瞬移"。

**改法（引用函数）**
- `ADemoCharacterBase` 构造函数：对 `ECC_Pawn` 设 `ECR_Overlap`（消除物理互推的响应基础）。
- `ABossCharacterBase::UpdatePlayerPushImmunity()`（`Tick` 中调用）：`GetCapsuleComponent()->IgnoreActorWhenMoving(Player, true)`
  → **BOSS 侧忽略玩家**，CMC 的位移修正不再把 BOSS 顶开（根治）。玩家侧不忽略 BOSS，仍被挡，不会穿模。
- `ABossCharacterBase::UpdateDisplacementGuard()`：去掉 `ETeleportType::TeleportPhysics`，改为平滑夹回。
- `UGA_BossAttackBase::SetBossGrounded(bool)`：起手 `true`（`SetPlaneConstraintNormal(UpVector)` + `SetPlaneConstraintOrigin(当前位置)`），
  `EndAbility` `false`。**注意原点必须设成当前位置**，设成世界原点会导致招式结束弹回。
- 本轮新增 `ABossCharacterBase::EnsureMeshCapsuleAlignment()`（BeginPlay + 1s 周期复查）：
  用网格**世界包围盒最低点**对比**胶囊底面**，偏差超过 `MeshAlignmentTolerance=5cm` 才沿胶囊 Up 补偿
  → 兜住"网格陷地（穿地）/ 网格浮空（看着像飞天）"，且不覆盖作者手调的偏移。

**验收标准**
- 贴脸输出、绕后推挤：BOSS 站位不动，日志 `[Boss][位移闸]` 基本不再出现。
- 突进斩全程 0.7s 匀速、落点与预告距离一致；招式结束不弹回。
- 日志出现 `[Boss][Mesh对齐]` 只在真正错位时打印一次。

---

## 3. P0-b：锁定失效

**设计理由**：锁定的本质是"把玩家从'找目标'的认知负担里解放出来"，让他能把注意力放在读招上。
锁定不稳 = 玩家被迫回到手动镜头，反馈链的"读招→应对"一环直接断掉。

**根因假设（三层，逐层修掉）**
1. **三方抢方向盘**（第 9 轮修）：`bUsePawnControlRotation=true` 让弹簧臂每帧按控制朝向重设旋转，
   同时代码又 `SetControlRotation` + `SetWorldRotation`，再加锁定组件每帧改角色朝向 → 互拽。
2. **忽略肩位偏移**（第 11 轮修）：`相机 = 枢轴 + 朝向×臂长 + 朝向旋转后的 SocketOffset`，
   用「枢轴→焦点」当相机朝向时，相机的实际视线并不指向焦点 → BOSS 偏下偏侧。
3. **补偿角在近距离爆炸**（第 12 轮修）：固定长度偏移 127cm 时，让 BOSS 居中所需偏角
   `atan(偏移 / sqrt(D² - 偏移²))`：D=480 → 15°，**D=150（近战贴身）→ 58°**，且对 D 极度敏感 → 视角疯狂抖动。

**改法（引用函数）**
- `AFightPlayerCharacter` 构造函数：`CameraBoom->bUsePawnControlRotation=false`、
  `SetUsingAbsoluteRotation(true)`（旋转不再被角色转身带动）、`bDoCollisionTest=true` + `ProbeChannel=ECC_Camera`（防突脸穿模）。
- `AFightPlayerCharacter::UpdateLockOnCamera()`：
  - 肩位偏移改为**恒定夹角**：`偏移长度 = min(|LockOnSocketOffset|, D × tan(LockOnShoulderAngleDeg))`，默认 `7°`
    → 补偿角与距离无关，任何距离都稳定。
  - **迭代解算**：给定朝向 R 算出相机真实位置 `枢轴 + R.Vector()*臂长 + R.RotateVector(SocketOffset)`，
    再让 R 对准"该位置→焦点"，循环 3 次（误差 <0.2°）。
  - 保险闸：解算结果相对"枢轴→焦点"直接瞄准线的偏离，Pitch/Yaw 各夹在 ±2×肩位角内。
  - 俯仰限位 `LockOnMinPitch=-30 / LockOnMaxPitch=10`（贴合验收规格）。
- `ULockOnComponent::GetFocusLocation()`：焦点 = 胶囊中心 + `HalfHeight × FocusHeightRatio + Offset`，
  当前 `FocusHeightRatio=0`（身体正中）。
- 失效条件：`bTargetGone || 距离 > LockBreakDistance(2800)` → `DisableLockOn()`；
  视线被挡（`ECC_Visibility` 探测命中）也解除。

**验收标准**
- 中键锁定后 BOSS **持续**落在画面正中（任何距离，含贴脸），锁定框与身体重合。
- 俯仰稳定夹在 -30°~10°，不随距离漂移。
- BOSS 位移/跳跃全程跟随；死亡/超距自动解除并平滑回基准臂长。

---

## 4. P0-c：打击感断链

**设计理由**：反馈链要求"每一次交互同时喂养至少两个系统"。
命中只掉血 = 单通道输出 → 玩家读不到"我打中了什么部位、什么力度"。

**根因假设**
1. 顿帧是**全局时间膨胀**（`SetGlobalTimeDilation`），连相机一起慢，缺少"相对冲击"感；且强度 0.02 几乎无感。
2. 全工程**没有任何相机震屏**（Camera Shake）。
3. BOSS 霸体覆盖过宽（`HasSuperArmor()==IsAttacking()`，整个出招技能期间含收招都霸体），
   命中只爆火花 → 玩家认为"僵直没了"。
4. `DemoStatusEffects` 的异常、韧性等资源没有可视化出口 → 打了也不知道在积累什么。

**改法（引用函数）**
- **顿帧分级**（第 10 轮）：`UDemoAbilitySystemLibrary::ApplyHit` 五个分支全覆盖，参数落 `DemoCombatTuning.h`：
  - 普通命中 `NormalHitHitStop=0.09s @ 0.15`；暴击/破防 `CriticalHitHitStop=0.13s @ 0.10`
  - 普通弹反 `NormalParryHitStop`、完美弹反 `RequestHitStopThenSlowMotion`（先冻后慢两段式）
  - 格挡 `0.06s @ 0.05`
  - **关键结论：顿帧是代码驱动的，不依赖蒙太奇动画通知** → 不存在"漏绑蒙太奇"，任何走 `ApplyHit` 的攻击都被覆盖。
- **相机震屏**（第 9/10 轮）：`AFightPlayerCharacter::PlayHitImpactShake(Damage, Duration)`
  叠在 `FollowCamera` 的相对旋转上（sin 自衰减、结束精确归零），强度随伤害缩放
  （`HitImpactShakeDamageScale=0.05`，夹在 `0.6°~3.0°`）；锁定时**不再屏蔽**。
- **受击闪光 + 火花**：`ABossCharacterBase::TriggerHitFlash()`（独立白点光，`HitFlashLightIntensity=9000`）
  在 `OnDamagedReaction` 顶部触发，**连霸体期间也闪**；`ADemoImpactBurst::SpawnBurst` 爆闪。
- **飘字**：`UCombatHUDWidget::HandleHitLanded`（必须 `UFUNCTION`，否则 `AddDynamic` 静默失败）。
- **受击反应**：非霸体窗口走 `PlayReactionMontage(HitReactMontage)` + `ApplyBossHitstun`（`State_Stagger` 0.28s）。

**验收标准（六通道同时到达）**
每次命中的同一帧内应同时看到：① 顿帧（分级）② 爆闪 + 火花 ③ 相机震屏 ④ 飘字 ⑤ BOSS 受击反应/闪光 ⑥ 削韧数值变化。

---

## 5. 反馈链 checklist（落地要求 1 / 2）

### 5.1 玩家动作 → 喂养的系统

| 动作 | 产出（喂养哪些系统） | 是否合格 |
|---|---|---|
| 普攻 1 段（28 伤 / 14 削韧） | 伤害 + 削韧(→破防进度) + 顿帧 + 火花 + 震屏 + 飘字 + BOSS 受击反应 + **异常积累** | ✅ 待接异常 |
| 普攻 2 段（34 / 14） | 同上（连段节奏压力） | ✅ 待接异常 |
| 普攻 3 段（42 / 14） | 同上 + 更长的后摇惩罚窗口（逼玩家管理取消） | ✅ 待接异常 |
| 普攻 4 段终结（56 / 14） | 同上 + **大削韧权重**（结束后破防窗口开启的主要手段） | ✅ 待接异常 |
| 弹反（右键，`UGA_PlayerParry`） | 免伤 + 攻击方硬直（`NormalParryStaggerTime`）+ 顿帧 + 火花 + 削韧 + **玩家侧奖励**（`UGE_PerfectRewardEffect`） | ✅ |
| 完美弹反（窗口 0.22s） | 上述全部 + **额外削韧 45** + 两段式"先冻后慢" + **打断 BOSS 部分攻击动画（参考 Demo 规格）** | ⚠ 打断未实现 |
| 闪避（左 Shift 短按，`UGA_PlayerDodge`） | 无敌帧（技能显式 `ApplyEffectToSelf`）+ 位移 + 姿态重置 | ⚠ 缺"完美闪避派生" |
| 奔跑（长按 Shift） | 走位/拉扯 | ✅ |
| 架势（P1 新增） | 格挡消耗、完美闪避返还、拼刀稳心 → 架势过低即破防 | ❌ 未实现 |

### 5.2 BOSS 招式 → 交互意图

| 招式 | 技能类 | 前摇（预警） | 交互意图 |
|---|---|---|---|
| 近战挥砍（6 段，`Boss_Slash_01`~`06`） | `UGA_BossAttack` | `TelegraphTime`（阶段2 ×`Phase2TelegraphScale`） | **压制**：逼玩家用弹反/闪避读招，而不是站桩输出 |
| 弹幕（`FBossBarrageParams`） | `UGA_BossRangedAttack` | 起手吟唱 | **逼走位**：用位移进入掩体/近身 |
| 突进斩（`FBossChargeParams`） | `UGA_BossChargeSlash` | 蓄势（**可弹反**）→ 0.7s 匀速突进 | **惩罚距离管理失误**：远端玩家被拉近 |
| 大招（`Boss_Ult` 阶段事件） | `UGA_BossUltimate` | 长蓄力 | **阶段里程碑**：全屏演出 + 换招式组 |
| 收招后摇 / 破防虚弱（`Boss_Weak_*`） | `HandleBossPoiseBroken` | 无 | **白给喘息**：明确的高额输出窗口 |

---

## 6. P1 实施计划（未开始，按依赖排序）

1. **双异常条**：`Effects/DemoStatusEffects.{h,cpp}` 的 7 种效果收编为两条积累槽
   （橙=冲击/失衡，青=元素），积累入口在 `UDemoAbilitySystemLibrary::ApplyHit`，
   显示在 `UCombatHUDWidget`（Boss 血条下方，参照 `ref_004.jpg` 布局）。
   阈值落 `DemoCombatTuning.h`，状态标签走 `Core/DemoGameplayTags.h`（新增 `State_Unstable`）。
2. **状态双通道信号**：`ABossCharacterBase` 加轮廓材质切换（失衡=绿，参照 `MI_BossFlash` 同族新增 `MI_BossUnstable`）
   + `UCombatHUDWidget` 浮动标签（UNSTABLE / BROKEN）。
3. **玩家架势条**：`UDemoAttributeSet` 新增 `Posture` 属性（与现有韧性并存，职责分离：
   韧性=被打削，架势=格挡消耗）；UI 用左上第二条（`ref_004.jpg` 的黄条槽位）。
4. **BOSS 默认格挡**：`ABossCharacterBase` 常态挂 `State_Blocking`（GE），被格挡 N 次后反制；
   输出必须走"异常击破"或"后摇惩罚窗口"。
5. **阶段演进事件**：复用 `Boss_Ult` 事件，阶段切换 → 换招式组 + 加 AI 参数（攻击欲望/反击概率/周旋时间）+ 氛围切换。

**性能红线**：所有新增索敌/状态查询节流 ≥0.1s；`Tick` 内禁止 `GetAllActorsOfClass`（现有实现均走 `EngineUtils`/已有缓存）。
