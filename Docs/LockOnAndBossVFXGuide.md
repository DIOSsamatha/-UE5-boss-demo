# 本轮交付：锁定视角 · BOSS 特效素材 · 跳砸→突进斩 · 动画流畅化

> 对应需求：①角色视角可锁定 BOSS ②BOSS 特效与素材（尽量程序化生成）
> ③跳砸不要了改成别的 ④远程攻击要有弹道 ⑤所有动画动作流畅一点
>
> 代码侧已全部完成并**编译通过**（`UnrealEditor-demo.dll` 2026-09-21 16:08:47，
> `find Source -newer <dll>` 为空 = 无遗漏源文件）。
> 资产侧需要你在编辑器里跑 3 个脚本（下面第 2 步）。

---

## 0. 一句话总览

| 需求 | 做法 | 落地位置 |
|---|---|---|
| 锁定视角 | 新增 `ULockOnComponent`（管锁谁+角色朝向），镜头分层交给 `AFightPlayerCharacter`（管镜头怎么摆） | `Player/LockOnComponent.*`、`Player/FightPlayerCharacter.*` |
| BOSS 特效素材 | 4 张 512² 贴图**程序化生成**（numpy/pillow 解析式，不是 AI 出图）→ 编辑器内建加法/半透明材质 | `Tools/GenerateBossVFXTextures.py`、`Tools/SetupBossVFX.py` |
| 跳砸替换 | 删除 `LeapSmash`，换成**突进斩 ChargeSlash**（蓄势→匀速突进拖刀→扇形收尾） | `Boss/BossCombatData.h`、`Abilities/BossAbilities.*`、`Boss/BossBrainComponent.cpp` |
| 远程弹道 | `ADemoProjectile` 加核心球+点光+**实例化网格拖尾**，命中再有爆闪+扩散环 | `Combat/DemoProjectile.*`、`Combat/DemoTrailComponent.*`、`Combat/DemoImpactBurst.*` |
| 动画流畅 | 混合时间分档写入蒙太奇 + 出招转向改恒速插值（不再 `SetActorRotation` 硬切） | `Tools/PolishBossAnimations.py`、`Boss/BossCharacterBase.*` |

---

## 1. 编辑器操作清单（按顺序做）

### 第 1 步：完整重启编辑器 ⚠ 必须

本轮新增了 **2 个 UCLASS**（`UDemoTrailComponent`、`ADemoImpactBurst`）—— Live Coding 处理不了新类，
所以刚才构建前已经关掉编辑器。现在**重新打开** `K:\selfgame\demo\demo.uproject`，
等编译器和资产浏览器都就绪再往下做。

### 第 2 步：生成特效素材（三个脚本，顺序不能反）

**① 生成贴图（已在系统 Python 里跑过，产物在 `Tools/VFXSource/`）**

```
Tools/VFXSource/T_VFX_Glow.png       (27 KB)  弹体核心 / 拖尾
Tools/VFXSource/T_VFX_Impact.png     (44 KB)  命中爆闪
Tools/VFXSource/T_VFX_Ring.png       (41 KB)  命中冲击环
Tools/VFXSource/T_VFX_Telegraph.png  (62 KB)  贴地预警圈
```

如果你想改配色/星芒数量/软硬边缘，编辑 `Tools/GenerateBossVFXTextures.py` 后用受管 venv 重跑：
`C:/Users/wdnmd/.workbuddy/binaries/python/envs/default/Scripts/python.exe Tools/GenerateBossVFXTextures.py`

**② 编辑器内建材质** —— 窗口 → 输出日志 → 右侧 "Cmd" 下拉切到 **Python**，粘贴执行：

```python
exec(open(r"K:/selfgame/demo/Tools/SetupBossVFX.py", encoding="utf-8").read())
```

产出（路径是**约定路径**，改了名字两边都要同步）：

| 资产 | 用途 | 混合模式 |
|---|---|---|
| `/Game/Boss/VFX/MI_BossGlow.MI_BossGlow` | 弹体核心球 + 拖尾 | 加法（不排序、不互相遮挡） |
| `/Game/Boss/VFX/MI_BossImpact.MI_BossImpact` | 命中爆闪 | 加法 |
| `/Game/Boss/VFX/MI_BossRing.MI_BossRing` | 命中冲击环 | 加法 |
| `/Game/Boss/VFX/MI_BossTelegraph.MI_BossTelegraph` | 贴地预警圈 | 半透明（要透视看地面） |

> 发光体一律用**加法混合 + Unlit**：顺序无关，能规避半透明排序问题；
> 只有贴地圈需要半透明，因为它得压在几何体上看。

**③ 重建 BOSS 蒙太奇**（会**自动删除**旧的跳砸三件套）：

```python
exec(open(r"K:/selfgame/demo/Tools/SetupBossMontages.py", encoding="utf-8").read())
```

### 第 3 步：统一动画混合时间

```python
exec(open(r"K:/selfgame/demo/Tools/PolishBossAnimations.py", encoding="utf-8").read())
```

**必须在第 2 步③之后跑** —— 它会覆盖 `SetupBossMontages.py` 写进去的混合时间。脚本是幂等的，重复跑没副作用。

四档规则（想微调就改脚本里的 `PROFILES`）：

| 档位 | BlendIn / BlendOut | 覆盖动作 | 为什么 |
|---|---|---|---|
| 急停 / 受击 | 0.04 / 0.10 | 受击四向、闪避 | 被打断必须立刻反应，混太久 = 延迟吃招 |
| 攻击起手 | 0.08 / 0.18 | 挥砍、突进、弹幕施法 | 起手要快（不然读招时间变长），收招要稳 |
| 蓄力 / 循环 | 0.12 / 0.12 | 蓄力循环、狂化循环 | 循环段首尾姿势接近，混长一点更顺 |
| 死亡 / 演出 | 0.12 / 0.30 | 死亡、阶段演出 | 演出类给足时间，戛然而止很廉价 |

### 第 4 步：进 PIE 验收

见下面第 3 节清单。

> **锁定键位不用配任何东西**：中键 / Q / E 是代码里用 `IsInputKeyDown` 做边沿检测的，
> 不依赖 IMC，也不用新建 IA 资产（左Shift 那条通道会抢按键状态，所以刻意绕开 Enhanced Input）。

---

## 2. 各功能说明与调参

### 2.1 锁定视角

**操作**：鼠标**中键**锁定/解锁 · **Q** 上一个目标 · **E** 下一个目标。

**分工（这点是关键，别混）**：
- `ULockOnComponent` 管**锁谁 + 角色朝哪**：目标选取、失效判定、角色 Yaw 跟随。
- `AFightPlayerCharacter` 管**镜头怎么摆**：控制朝向、臂长、肩部偏移、俯仰限位。

之所以把镜头拆出去：镜头参数是纯表现，且要和**镜头冲击**（弹反拉近）共用同一个出口，
放在角色里才拿得到 `ApplyCameraPunch` 的状态。

**选目标规则**（`LockOnComponent.h` 上都是 `EditDefaultsOnly`，可以在 `BP_FightPlayer` 里改）：

| 参数 | 默认 | 说明 |
|---|---|---|
| `MaxLockDistance` | 1800 | 锁定上限距离 |
| `LockBreakDistance` | 2800 | 超此距离自动脱锁。**必须比上一个值大** —— 玩家边打边走位，距离持续变化，相等会"刚锁上就掉" |
| `MinLockDistance` | 60 | 太近不锁（否则镜头糊在 BOSS 身上） |
| `MaxLockAngle` | 80 | 目标必须落在镜头正前方这个夹角内 |
| `SwitchTargetMaxAngle` | 140 | 切目标时的角度（比上者宽：已经锁上了就别太挑） |
| `bBreakOnLineOfSightBlocked` | false | 默认关：柱子常挡住射线，频繁掉锁比"隔障碍锁定"更烦 |
| `bDriveOwnerRotation` | true | 锁定时由组件驱动角色 Yaw |
| `TurnSpeed` | 11 | 转向目标速度 |

**失效自动解锁**：目标死亡、目标被销毁、超出 `LockBreakDistance`、玩家死亡。

**⚠ 镜头分层的坑（已修，别改回去）**：`RestingArmLength` / `RestingSocketOffset` 是
"无锁定无冲击"的**基准**，锁定镜头和 `ApplyCameraPunch` 都从它出发。
两边如果都直接写 `TargetArmLength`，就是谁后写谁赢 —— 表现是**"弹反一下镜头偏了再也回不来"**。

**准星**：HUD 上四个角各一横线+竖线共 8 个 `UImage`。
注意槽位必须 `SetAnchors(FAnchors(0,0,0,0))` + `SetPosition(绝对像素)`
—— 用比例锚点会被锚点再拉伸一次，准星会飘。

### 2.2 跳砸 → 突进斩（ChargeSlash）

`EBossAttackType` 现在是：`MeleeSlash`(0) / `RangedBarrage`(1) / **`ChargeSlash`(2)** / `Ultimate`(3)。
`LeapSmash` 及其 `FBossLeapParams` 已整体删除。

**招式流程**：蓄势（**可弹反**，出预警条）→ `UAbilityTask_ApplyRootMotionConstantForce` **匀速突进**
（拖刀，沿途碰触即伤害）→ 前方 **±78° 扇形**收尾冲击。

**为什么不用 `LaunchCharacter` 做位移**：实际位移 = `v²/(2a)`，而 `a` 由
`BrakingDecelerationWalking`(2048) + `GroundFriction`/`BrakingFriction` 共同决定，外部不可见
→ 距离既不好估也不好调（之前 650→500→350 一路试都"不对"）。
`ApplyRootMotionConstantForce` 是 `Strength = 距离/时长`，**恰好**位移设定的厘米数，匀速且碰撞安全。

**参数表 `FBossChargeParams`**（`Boss/BossCombatData.h`）：

| 字段 | 默认 | 说明 |
|---|---|---|
| `ChargeDistance` | 520 | 突进水平距离（cm），**这就是真正前进的距离** |
| `ChargeDuration` | 0.26 | 耗时。距离/时长=速度；越短越"弹射"，越长越"碾压" |
| `MaxChargeDistance` | 1500 | 距离上限（玩家太远时截断，防冲出关卡边界） |
| `StopDistance` | 170 | 收尾时停在目标身前多远（让收尾斩像砍在刀尖上，而不是糊在玩家身上） |
| `bDamageAlongDash` | true | 拖刀：突进全程算一次判定。关掉则只有收尾有伤害 → 突进纯位移，更容易躲 |
| `ImpactRadius` | 300 | 收尾判定半径 |
| `ImpactHalfAngleDeg` | 75 | 扇形半角。**90 = 前方半圆；180 = 整圈** |
| `bImpactCanBeParried` | true | 收尾能否被弹反。红色重击类建议 false（逼闪避） |
| `KnockbackStrength` / `KnockbackUpward` | 520 / 170 | 击退 |
| `bSpawnShockwave` + `Shockwave` | false | 收尾再放一圈震荡波（二阶段/大招收尾用） |

**二阶段缩放**：`Phase2ChargeDistanceScale` / `Phase2ImpactRadiusScale`。

**大招收尾复用**：`bEndWithChargeFinisher = true` 时大招结束接突进斩参数，把 `ImpactHalfAngleDeg`
设成 ≥180 就是整圈冲击 —— 这样只需维护一套收尾手感参数。

### 2.3 弹道与命中特效

**三层视觉**（全部走约定路径加载，**取不到就优雅降级不崩**，只是没特效）：

1. **核心球** —— `UStaticMeshComponent`，材质 `/Game/Boss/VFX/MI_BossGlow`，加法混合，看着亮但不发光照人。
2. **拖尾** —— `UDemoTrailComponent`：**一个 `UInstancedStaticMeshComponent` 的 N 个实例**（不是 N 个组件），
   用 `PerInstanceCustomData3Vector` 做**头亮尾淡**。关键实现细节：
   - `SetNumCustomDataFloats(3)` **必须在 `AddInstance` 之前**（引擎禁止有实例时改通道数）
   - 按**"前进了多少厘米"采样**，不是按帧 —— 所以弹速改了拖尾疏密不变
   - 瞬移（超 `MaxSampleSnapDistance`）会丢弃历史，避免拖出一条穿过半张地图的长线
3. **点光** `UPointLightComponent`（拖尾/核心的可选发光照明）+ **命中爆点** `ADemoImpactBurst`：
   中心爆闪（`(1-t)^2.2` 亮度曲线）+ 贴地扩散环（`FRotationMatrix::MakeFromZ(Normal)` 定位并抬高防 Z-Fight）+ 点光一闪。

**性能预算**（`BossBrainComponent.cpp` 里按弹量分配）：
主弹幕 `TrailSampleCount=16` + 点光开；环形弹幕 `TrailSampleCount=6` + **点光关**；震荡波关点光。

**所有销毁路径都收敛到 `ResolveAndDestroy()`**：先 `Trail->SetTrailEnabled(false)` →
再 `SpawnBurst` → 再广播 `OnProjectileImpact` → 最后 `Destroy`。
这样不会出现"拖尾还挂在半空"或者"命中没特效"的分支遗漏。

### 2.4 动画流畅化

**两个真凶，两个修法**：

**① 硬跳 = 蒙太奇 BlendIn/BlendOut 默认为 0。**
UE 新建蒙太奇的混合时间默认就是 0，等于"姿势瞬间跳过去"。修法见第 1 步第 3 段的分档表。

**② 出招硬转向 = `SetActorRotation` 瞬间切。**
原来 `UGA_BossAttackBase::FaceTarget()` 直接把角色转过去，视觉上是"啪"一下扭头。
现在改成 `ABossCharacterBase::SetFacingTarget(WorldLocation, TurnSpeedDegPerSec=720, MaxTurnTime=0.40)`
+ Tick 里 `RInterpConstantTo`（**恒速插值**，不是指数衰减 —— 指数衰减会"开头快结尾拖尾巴"，
读起来不像"转身"）。`MaxTurnTime` 是兜底：目标高速绕圈时不会无限追着转。

`UpdateSmoothFacing` 放在 `ABossCharacterBase::Tick` 里**早于 ASC 空指针的提前 return**
—— 纯表现不该因为 GAS 没就绪就停。

**不会打架**：Brain 只在 `Telegraph` 状态转，技能只在 `Attack` 状态转，两者时间上不重叠。

---

## 3. PIE 验收清单

### 锁定视角
- [ ] 中键 → 镜头拉近 BOSS、准星出现在 BOSS 身上、角色身体转向 BOSS
- [ ] 锁定后按 **Q/E** → 准星跳到另一个目标（场上有多个敌人时）
- [ ] 锁定状态下移动 → 身体保持面向 BOSS，只有镜头跟着鼠标微调（横移）
- [ ] BOSS 死亡 / 走远超过 2800 → 自动解锁，镜头平滑回到基准臂长
- [ ] **弹反一次后再看镜头** → 拉近效果结束后应回到基准，不该"偏了回不来"
- [ ] **持续输出 10s+ 不脱锁**：锁定后镜头始终朝向 BOSS 躯干，俯角稳定不漂（不再"越漂越大最终脱锁"）
- [ ] **俯仰夹口**：锁定后俯仰被夹在 **-30°~10°**，不会一路俯冲到地面（`LockOnMinPitch/MaxPitch`）
- [ ] **突进贴脸不穿模**：BOSS 突进冲到玩家脸上时，弹簧臂不缩进 BOSS 身体（`bDoCollisionTest` + `ProbeChannel=ECC_Camera`）

### 突进斩（先单独调）
把 `BP_Boss` 上的 Brain 组件 → `Boss|Debug` → **`DebugForceAttackType = 2`**，PIE 观察：

- [ ] 蓄势段 → **金色预警条**（金=可弹反）
- [ ] 突进段 → BOSS 真位移（**胶囊体在动**，不是动画在动），沿路碰到就吃伤害
- [ ] `LogTemp` 里应有 `[Combo] 本轮实际位移 = XX cm`，XX ≈ 520
- [ ] 收尾斩 → 前方扇形范围命中（站在 BOSS 背后应该打不到）
- [ ] 站在正面按弹刀 → 能弹反（`bImpactCanBeParried=true`）

> 若"看着在动但胶囊体没动 + 播完拉回" = 根位移模式被设成了 `NoRootMotionExtraction`。
> 代码有自愈（`EnsureRootMotionExtraction`），日志里能看到
> `[RootMotion] …根位移模式 X → RootMotionFromMontagesOnly(1)`。

### 弹道
`DebugForceAttackType = 1`：

- [ ] 弹体**看得见**（核心球亮），后面拖一条**头亮尾淡**的尾巴
- [ ] 尾巴方向 = 飞行方向（能读出弹道从哪来）
- [ ] 打中墙/角色 → 有爆闪 + 贴地扩散环 + 点光一闪，然后弹体消失
- [ ] 拖尾不会残留、不会拉成一条穿过地图的长线（快速移动/瞬移时）

### 大招 / 阶段演出
`DebugForceAttackType = 3`：

- [ ] 大招收尾是整圈冲击（`ImpactHalfAngleDeg>=180`）
- [ ] 阶段转换演出动作不再"啪"地一下切进去

### 动画整体
- [ ] 挥砍 → 下一个动作之间没有明显的姿势跳变
- [ ] 受击反应够快（不该有"延迟吃招"的感觉）
- [ ] 死亡动画收尾不戛然而止

---

## 4. 改完东西之后（避免踩坑）

| 改动 | 必须同步 |
|---|---|
| 改招式名 / 加新招式 | `Boss/BossBrainComponent.cpp` 的路径 **和** `Tools/SetupBossMontages.py` 的 `MONTAGES`（两边逐字一致），然后重跑脚本；废弃的加进 `RETIRED` 列表 |
| 改特效材质名 | `Tools/SetupBossVFX.py` **和** `Combat/DemoProjectile.cpp`、`DemoTrailComponent.cpp`、`DemoImpactBurst.cpp` 顶部的 `G*MaterialPath` 常量 |
| 调战斗数值 | 只改 `Core/DemoCombatTuning.h`（推导写在文件顶部，目标战斗 4~6 分钟） |
| 加新 UCLASS | **完整关编辑器**再构建，别指望 Live Coding |

**构建**：`cd /k/selfgame/demo && "C:/Users/wdnmd/.workbuddy/binaries/python/versions/3.13.12/python.exe" Tools/run_build.py`
（目标 `demoEditor`，必须 `-NoUBA`）

---

## 5. 排错

| 现象 | 原因 / 处理 |
|---|---|
| 弹道没有特效、但弹体正常飞 | 材质没生成。重跑 `Tools/SetupBossVFX.py`，确认 `/Game/Boss/VFX/MI_BossGlow` 存在 |
| 弹道完全看不见 | 检查 `ADemoProjectile` 上的 `bEnableCoreVisual` / `bEnableTrail`；再确认 `MI_BossGlow` 的混合模式是**加法** |
| 预警圈在 BOSS 脚下看不见 | `MI_BossTelegraph` 应该是**半透明**，不是加法；另外它贴地，检查有没有 Z-Fight |
| 突进斩位移是 0 | 根位移模式错。看日志 `[RootMotion]` 那行；资产侧跑 `Tools/FixRootMotionSetup.py` |
| 突进斩位移翻倍 | 动画自带根位移和代码位移**叠加**了 —— 二者不能同时开 |
| 准星位置偏移 | HUD 槽位用了比例锚点，改成 `FAnchors(0,0,0,0)` + 绝对像素 |
| 中键/Q/E 没反应 | 这三个是 `IsInputKeyDown` 边沿检测，不走 IMC。若你改过 `FightPlayerCharacter.h` 里的 `LockOnKey` / `SwitchTargetLeftKey` / `SwitchTargetRightKey`，检查键名拼写 |
| `Unable to build while Live Coding is active` | 编辑器还开着。本轮新增过 UCLASS → 完整关掉再编 |
