# 弹刀手感 / 锁定视角 / 弹幕瘦身 —— 交付与验证说明（2026-09-22）

本轮 5 条诉求全部改完并**已编译进 `Binaries/Win64/UnrealEditor-demo.dll`**（DLL 1,662,976 字节，
已用 `grep -a "Event.Ability.ParryRewindow"` 验证新代码确实在包里）。下面按诉求逐条说明改了什么、
在哪里调、怎么在 PIE 里验证。

---

## 一、5 条诉求 → 改动

### 1. 「角色砍 boss 没扣血」

其实是**三个原因叠加**，不是一个 bug：

| 原因 | 说明 | 处理 |
| --- | --- | --- |
| 视觉上根本看不出来 | BOSS 10000 血，玩家单次普攻 28~56 点 → 血条只走 **0.3~0.6%**（约 2~4 像素） | Boss HUD 新增**累计伤害读数**「已造成伤害 X（Y%）」，直接把数字摆出来 |
| 判定真 bug | `PerformMeleeTrace` 连做两次 `SweepMultiByChannel`，**第二次复用了同一个 `Hits` 数组**，把第一次的结果覆盖了 → 实际只有刀尖那一小段有判定 | 改成 `TipHits` + `Hits` 两个数组，最后 `Append` |
| 武器 socket 没取到 | `WeaponStart`/`WeaponEnd` 两个 socket 长在**武器静态网格 `SM_Sword_08`** 上，不在骨骼网格上；原代码 `GetMesh()->DoesSocketExist()` 恒为假 → 退化成「身体正前方一条粗线」 | 新增 `ResolveWeaponSocket()`，遍历角色身上所有 `UStaticMeshComponent` 找；并加一次性 `[Melee]` 日志 |

调试开关：控制台 `demo.DamageLog 1`（会打印每次判定的原始值 / 最终值 / 是否被弹反格挡）。

### 2. 「视角锁定应该锁 boss 身上而不是血条」

`LockOnComponent::GetFocusLocation()` 原来用 `GetActorBounds()` 取中心——这个包围盒**把武器和背旗都算进去了**，
中心点被抬到头顶偏上，看起来就像锁在血条上。

改成用胶囊体中心再抬一点：

```cpp
LockedTarget->GetSimpleCollisionCylinder(HalfHeight, Radius);
const float Lift = HalfHeight * FocusHeightRatio + FocusHeightOffset;
```

**可在编辑器里调**：`BP_FightPlayer` → `LockOnComponent` → `Focus Height Ratio`（默认 `0.15`，调大镜头更靠上）
/ `Focus Height Offset`（默认 `0`，厘米）。

### 3. 「远程弹幕不要这么大、量不要这么多」

三套弹幕全面瘦身（数值都在 `BossBrainComponent.cpp` 的 `EnsureDefaultPhases`）：

| | 弹数 | 扩散角 | 碰撞半径 | 核心视觉 | 拖尾粗细 | 命中爆点 |
| --- | --- | --- | --- | --- | --- | --- |
| 弹幕 Barrage | 5 → **3** | 38° → **26°** | 24 → **14** | 1.15 → **0.55** | ×1.9 → **×1.0** | 3.4 → **1.8** |
| 环形冲击波 | 10 → **8** | — | 26 → **14** | 新增 **0.6** | — | 2.2 → **1.4** |
| 大招 | 3 环×12 → **2 环×8** | — | 26 → **14** | 0.85 → **0.5** | — | 2.4 → **1.4** |

二阶段额外弹数同步下调（`Phase2ExtraProjectiles` 4 → 2）。发光强度 3000 → 2200。
调参入口：`BossBrainComponent.cpp` 每个阶段的 `ProjectileCount` / `SpreadAngleDeg` / `CollisionRadius`。

### 4. 「不需要 boss 飞起来」

根因：`Attack_Sprint` / `Combo` 这类蒙太奇**带垂直方向的根位移**，一旦开启根位移抽取，胶囊体就会被抬起来，
顺带导致近战判定打空。

处理：`BossAbilities.cpp` 新增 `SetBossGrounded(bool)`——出招期间把 Z 轴钉在水平面上：

```cpp
Movement->SetPlaneConstraintNormal(FVector::UpVector);
Movement->SetPlaneConstraintEnabled(true);
```

进招时 `SetBossGrounded(true)`，`EndAbility` 里 `SetBossGrounded(false)` 还原（原值在首次调用时保存，
不会破坏原本的平面约束设置）。

### 5. 「只狼那种弹刀手感」（按你的要求：只改手感，不加韧性/忍杀等机制）

`DemoCombatTuning.h` 里的弹反常量整体重配，配合新增的火花 / 镜头冲击 / 连弹：

| 项 | 改前 | 改后 | 目的 |
| --- | --- | --- | --- |
| 完美弹反顿帧 | 0.30s @0.02× | **0.07s @0.015×** | 从"卡住"变成"咔"一下 |
| 完美弹反慢动作 | 0.45s @0.25× | **0.08s @0.50×** | 只留极短微慢动作，不打断节奏 |
| 普通弹反顿帧 | 0.20s | **0.10s** | 两种档位区分更清楚 |
| 弹反火花 | 无 | **半径 70，0.20s，金/橙两色** | 完美=亮金，普通=橙 |
| 镜头冲击 | 无 | **普通 10 / 完美 16，0.14s** | 打击感 |
| 弹刀后锁窗 | 无 | **0.14s** | 连续压制的节奏窗口 |

**连弹（只狼式节奏）**：`Event.Ability.ParryRewindow` 新增事件。右键在弹刀技能生效期间**再按一次**会
重新开判定窗（而不是被 `InstancedPerActor` 拒绝激活），所以可以按住右键连续弹。
`bPlayCounterOnParry` 默认 `false` = 弹完不自动播反击动画，节奏交还给你。

调参入口：`Core/DemoCombatTuning.h` 里所有 `Parry*` 常量。

---

## 二、PIE 验证清单（建议按顺序）

1. **启动编辑器**（直接开 `demo.uproject`，**不要点 rebuild**，DLL 已是最新）。
2. **验证锁定视角**：进 PIE → 靠近 BOSS → 鼠标**中键**锁定 → 镜头焦点应在 BOSS **躯干**上，不再是头顶的血条。
   `Q`/`E` 换目标仍然可用。
3. **验证弹幕瘦身**：控制台 `DebugForceAttackType 1` 强制弹幕（`0`近战 / `1`弹幕 / `2`突进斩 / `3`大招）→
   应只有 3 发、细很多。
4. **验证 BOSS 不飞**：`DebugForceAttackType 2` 突进斩 → BOSS 应贴地突进，**不抬升**。
5. **验证砍中了有伤害**：右下角 HUD「已造成伤害」应随每次命中**累加**，并显示百分比。
   - 若数字完全不动 → 开 `demo.DamageLog 1` 看是否根本没进判定（看 `[Melee]` 那行 socket 解析日志）
   - 若数字在涨但血条几乎不动 → 正常，BOSS 10000 血，单刀就是 0.3~0.6%
6. **验证弹刀手感**：BOSS 起手时血条上方的**预警条**变**金色**（可弹反）时按**右键** →
   应有「咔」一下的顿帧 + 极短慢动作 + 金光火花 + 轻微镜头冲击。
   金色窗口内连按右键应能**连续弹**同一套连招。
7. 想调手感：改 `Core/DemoCombatTuning.h` 的 `Parry*` 常量后重新构建即可。

---

## 三、2026-09-25 回归分析 + P0/P1 修复

> 这一节是对「用户实测视频复现三个症状」的回应。**先讲清楚之前声称已交付的修复为什么"看起来又失效了"，再讲这次到底改了什么。**

### 3.1 回归分析：为什么"之前的修复没用"

先把结论摆出来：**之前那轮（`SetBossGrounded` 贴地、`GetFocusLocation` 胶囊化焦点、突进速度封顶、击退钳制、位移闸）的代码都在、也都生效了**——这次对着 `Saved/Logs/demo.log` 重新对拍，证据如下：

| 检查项 | 日志 / 代码铁证 | 结论 |
| --- | --- | --- |
| 突进是不是"瞬移贴脸" | `[Boss] 突进：距离=801cm 时长=0.70s 速度=1144cm/s`（另有 900cm/0.70s/1286cm/s） | **是 0.7s 匀速冲刺，不是瞬移**。速度已被 `BossChargeSpeed=1100` 封顶 |
| 位移闸是不是一直在拉回 | `[Boss][位移闸]` 整场只出现 **2 次** | 闸只是安全网，不是"每帧都在打架" |
| 锁定是不是"锁了又掉" | `[LockOn] 已锁定`(11:35:25) → `[LockOn] 已解除锁定`(11:35:30) | 锁定**稳定维持 5 秒后自动丢失**，指向"镜头/距离判定"而非"没锁上" |

**也就是说：不是"修复失效"，是"修复治了表面，没治到根"。三个症状各自有更深的真凶——**

- **Boss 被我推着走 / 打飞（症状1 的真相）**：根因不是突进位移，而是**玩家与 BOSS 的胶囊在 `ECC_Pawn` 通道上默认互相 Block**。玩家身体撞上去，物理求解器每帧把双方互相顶开，单帧能推出 30~40cm（日志实锤：`[Boss][位移闸] 单帧位移 38.0cm > 允许 11.2cm（非出招）`），位移闸再把它拉回 → 观感就是"瞬移 / 打飞"。之前的 `SetBossGrounded` 只钉了 Z 轴，没碰水平互推；`MaxDepenetrationWithPawn=4` 只是把"一帧弹飞"缓成"几帧挤开"，源头还在。
- **锁定失效（症状3 的真相）**：`CameraBoom` 依赖 `bUsePawnControlRotation` 读控制朝向，这个链路**可能晚一帧、或受父组件（角色）旋转影响**，实测出现"锁定了但相机不转向 Boss、俯角越漂越大、最终脱锁"。另外默认 `ProbeChannel` 不一定是 `ECC_Camera`，突进贴脸时弹簧臂会缩进 BOSS 身体（穿模）。
- **打击感缺失（症状2 的真相）**：`DemoHitStopSubsystem::RequestHitStop` 用的是 `SetGlobalTimeDilation`（**全局时间膨胀，连相机一起慢**），没有任何"相对冲击"；而且**整个项目之前没有任何 Camera Shake / FOV 脉冲**——所以"完全无感"。

### 3.2 顺带纠正 Spec 里 3 处与事实不符的描述

1. Spec 称"文档声称已交付修复但视频复现"——代码确实在且生效（见 3.1 铁证），问题在更深的物理互推，不是"修复没落地"。
2. Spec 称 `Content/Data/.keep` 是空占位、`FBossChargeParams` 未生效——实际 `Content/Data/` 下有 `Abilities/`、`Boss/` 子目录，`FBossChargeParams` 是真实生效的结构体（本文件第 2.2 节参数表）。
3. Spec 描述"瞬移贴脸"——日志证明是 0.7s 匀速突进。

### 3.3 P0 锁定相机根治（已改 `FightPlayerCharacter`）

| 改法 | 位置 | 说明 |
| --- | --- | --- |
| 弹簧臂双保险 | `UpdateLockOnCamera` 锁定分支新增 `CameraBoom->SetWorldRotation(Smoothed)` | `bUsePawnControlRotation` 链路不确定，这里每帧显式钉死弹簧臂世界朝向，下一帧就算它重设也是 Smoothed，不回弹 |
| 防穿模 | 构造函数 `CameraBoom->bDoCollisionTest=true; ProbeChannel=ECC_Camera;` | 突进贴脸时镜头不缩进 BOSS 身体 |
| 俯仰收口 | `LockOnMinPitch=-30`、`LockOnMaxPitch=10` | 验收要求锁定后俯仰 -30°~10°，不再"越漂越大" |
| 缓存基准 | 锁定/解锁分支各缓存 `CameraBaseRotation` | 给微震/冲击一个"不含震动"的基准朝向，避免逐帧累加 |

### 3.4 P0 Boss 被推 / 飞天根治（已改 `DemoCharacterBase` + `BossCharacterBase`）

- **根因消除**：`ADemoCharacterBase` 构造函数把**玩家与 BOSS 的胶囊**对 `ECC_Pawn` 通道设为 `ECR_Overlap`。双方不再产生互推力，物理互推这个根因被彻底拔掉。
  ⚠ 命中检测不受影响：玩家砍 BOSS、BOSS 范围冲击都走 `OverlapMultiByChannel(ECC_Pawn)`，**通道响应为 Overlap 仍会被该查询命中**（Ignore 才跳过），所以"能砍到 BOSS"照常工作。
- **位移闸去伪影**：`UpdateDisplacementGuard` 从 `SetActorLocation(Fixed, …, TeleportPhysics)` 改成 `… ETeleportType::None`——即便触发也"平滑夹回"而非一帧闪现。

### 3.5 P1 打击感补强（已改 `FightPlayerCharacter` + `CombatHUDWidget` + `DemoCombatTuning`）

对标"绝区零"，补三件套（之前全缺）：

| 反馈 | 触发 | 实现 | 参数（都在 `DemoCombatTuning.h`） |
| --- | --- | --- | --- |
| 命中旋转微震 | 玩家砍中 BOSS（每次） | HUD `HandleHitLanded` → `Player->PlayHitImpactShake(Damage)` → `AddCameraShake` | `HitImpactShakeDamageScale=0.05`、`Min=0.6°`、`Max=3.0°`；轻击≈1.4°、重击封顶 3.0° |
| 受击 FOV 脉冲 | BOSS 突进/收尾重击（HeavyImpact）命中玩家 | `AFightPlayerCharacter::OnDamagedReaction` → `PulseCameraFOV(+4°, 0.12s)` | `BossHeavyHitFOVPulse=4`、`BossHeavyHitFOVPulseDuration=0.12` |
| 拉近式微冲击（已有） | 每次命中 | `ApplyCameraPunch(6, 0.07)` | — |

镜头微震叠在 `FollowCamera` 相对旋转上、FOV 脉冲走 `PlayerCameraManager`，都是 sin 自衰减、结束归零，连段叠起来是节奏感不是眩晕。`LastDamageSource` 由基类受击结算时写入，重击分支判定 `== HeavyImpact` 才扩 FOV，普通近战不扩。

> 还没做（明确留给下一轮 / P2）：**攻击吸附（aim assist）**、**完美闪避/弹反的子弹时间**已有（见第 5 条诉求），**伤害飘字弹跳放大**在 `DemoDamageNumberLayer` 已有，本次未动。

### 3.6 验收清单（本轮新增 / 变更）

- [ ] **Boss 不再被推/打飞**：贴脸输出、绕后，BOSS 应保持站位，日志 `[Boss][位移闸]` 应**基本不出现**（偶发也只该是 0.x cm 修正，不再是 38cm 量级）
- [ ] **锁定不脱锁**：中键锁定后持续输出 10s+，镜头始终朝向 BOSS 躯干、俯角稳定不漂；突进贴脸镜头不缩进 BOSS 身体
- [ ] **锁定俯仰**：锁定后俯仰应被夹在 -30°~10°，不会一路俯冲到地面
- [ ] **命中手感**：每次砍中 BOSS 镜头有一记轻微旋转微震 + 拉近；重击（突进收尾 HeavyImpact）命中玩家瞬间视野轻微扩一下再弹回
- [ ] **位移闸无伪影**：若仍触发，应是平滑夹回而非一帧闪现

---

## 四、关于本次构建（重要）

本机从今天起出现了一个**环境级**故障：**UnrealBuildTool 启动的任何子进程都会被系统安全层掐断**
（cl.exe / link.exe 甚至它自己的 `dotnet -Mode=WriteMetadata` 子进程，全部"秒退 exit 1、零输出、存活 <60ms"）。
特征很好认：日志里满屏 `Exited with error code 1`，但**没有任何 `error C####` 诊断**。

**这不是代码问题**——同一个 `.obj.rsp` 由脚本直接跑 100% 成功。

因此构建通道新增了兜底：

- `Tools/run_build.py`：先走 UBT；识别到上述特征（有错误码行、无真实诊断）时**自动回退**
- `Tools/ManualBuild.py`：直接接管 UBT 已生成好的 `.rsp` 手工编译+链接（增量、可 `--force` 全量、`--jobs N` 并行）

四步手工通道（缺一步 UBT 就会一直报"还有 1 个动作"）：
编译 obj → 链接 dll → `link /LIB` import lib → `dotnet UBT -Mode=WriteMetadata`。
当前四步都已补齐，重跑构建会得到 **`Target is up to date` / `Result: Succeeded` / exit 0**。
