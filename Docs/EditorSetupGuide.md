# BOSS 战 Demo — 编辑器配置清单（GAS 版）

C++ 已全部编译通过。下面按 **A→E** 顺序操作，A 做完就能跑起来看效果，B/C/D 是手感与内容。

---

## A. 让角色能跑能砍（必做）

### A1. 创建 6 个技能蓝图
在 `Content/FightPlayer/Abilities/` 下新建，父类分别是：

| 蓝图名 | 父类 | 需要填的内容 |
|---|---|---|
| `GA_NormalAttack` | `UGA_PlayerNormalAttack` | ComboMontages（3 段连招）、ComboDamages |
| `GA_AttackSkill` | `UGA_PlayerAttackSkill` | SkillMontage |
| `GA_Defense` | `UGA_PlayerDefense` | 无需填 |
| `GA_Parry` | `UGA_PlayerParry` | 无需填 |
| `GA_Dodge` | `UGA_PlayerDodge` | DodgeMontage 或 4 个方向 Montage |
| `GA_BossAttack` | `UGA_BossAttack` | 无需填（招式由数据资产动态指定） |

> 这些类的 `AbilityTags` 已在 C++ 里打好，不用手动配。

### A2. 创建角色 / 控制器 / GameMode 蓝图
| 蓝图名 | 父类 | 位置建议 |
|---|---|---|
| `BP_FightPlayer` | `AFightPlayerCharacter` | Content/FightPlayer/ |
| `BP_Boss` | `ABossCharacterBase` | Content/Boss/ |
| `BP_PlayerController` | `AFightPlayerController` | Content/FightPlayer/ |
| `BP_FightGameMode` | `AFightGameMode` | Content/ |
| `WBP_CombatHUD` | `UCombatHUDWidget` | Content/UI/ |
| `WBP_BossBar` | `UBossHealthBarWidget` | Content/UI/ |

### A3. 配置 BP_FightPlayer
- **Mesh**：`SK_Mannequin`，AnimBP 用 `ABP_FightPlayer`
- **Input**（分类 `Demo | Input`）：`DefaultMappingContext` = `IMC_FightPlayer`，另外 **8 个槽位逐个填**：

  | 槽位名 | 填哪个资产 | 不填的后果 |
  |---|---|---|
  | `Move Action` | `IA_Move` | 走不动 |
  | `Look Action` | `IA_Look` | 转不了视角 |
  | `Jump Action` | `IA_Jump` | 跳不了 |
  | `Run Action` | `IA_Run` | 不能跑 |
  | `Normal Attack Action` | `IA_NormalAttack` | 左键无反应 |
  | `Attack Skill Action` | `IA_AttackSkill` | 右键无反应 |
  | `Defense Action` | `IA_Defense` | 格挡/弹反无反应 |
  | `Flash Action` | `IA_Flash` | 闪避无反应 |

  > ★ **这 8 个槽位是"按键完全没反应"的头号原因。** 属性是 `EditDefaultsOnly` 指针，
  > 空着时 C++ 里对应的 `BindAction` 会被直接跳过——**不报错、不崩溃、就是没反应**。
  > 构造函数里已做兜底（按 `/Game/FightPlayer/Input/` 约定路径自动加载），
  > 但**建议仍在蓝图里显式填一遍**，这样以后挪动资产也不会失效。
  > 缺失时 PIE 的 **Output Log** 会打红色 Error 列出缺的槽位名。

- **DefaultAbilities**（继承自基类）：加入 `GA_NormalAttack`、`GA_AttackSkill`、`GA_Defense`、`GA_Parry`、`GA_Dodge`
  - 这 5 个**必须填**，否则按键有反应但技能放不出来
  - `NormalAttackAbility` 等 5 个单独属性**可以留空**，代码会自动按 AbilityTag 查找
- **Combat**：确认 `TeamId = 0`

> ⚠️ **配好 `BP_FightPlayer` ≠ 它会生效。**
> 让它真正上场的唯一开关在 **A6 的「默认 Pawn 类」**：必须在那里填 `BP_FightPlayer`。
> 只配 BP、不动 GameMode → PIE 里生成的是**原生 `FightPlayerCharacter`**（网格/动画/技能全在 BP 里，所以什么都没有）→ 表现为**画面里看不到角色**。

### A4. 给剑加两个插槽
打开 `Weapon_Sword` 骨骼，加 socket：
- `WeaponStart`（剑柄）
- `WeaponEnd`（剑尖）

**这两个名字必须完全一致**，否则命中检测会退化成"角色前方一段"。

### A5. 配置 IMC_FightPlayer 键位
| IA | 按键 | Modifiers | Trigger |
|---|---|---|---|
| IA_Move | W / A / S / D（见下表） | **必须加**（见下） | — |
| IA_Look | 鼠标 XY 2D | **Scalar，X/Y ≈ 0.15** | — |
| IA_Jump | Space | — | Down |
| IA_Run | **Left Shift** | — | **Down（必须！）** |
| IA_NormalAttack | 鼠标左键 | — | Pressed |
| IA_AttackSkill | 鼠标右键 | — | Pressed |
| IA_Defense | Q | — | **Down**（按住格挡，松开结束） |
| IA_Flash | Left Alt | — | Pressed |

#### A5.1 IA_Move 的 4 个键必须加修饰器（否则四个方向全错）

**原理**：数字键（W/A/S/D）的原始值**只写在 X 轴上**——引擎里 `KeyState.RawValueAccumulator.X = AmountDepressed`，
Y/Z 恒为 0。所以一个键不加修饰器时输出恒为 `(1, 0)`。
四个键都不加 → **W/A/S/D 输出的全都是 `(1, 0)`，等于四个键都在"向右走"**，
表现就是"A/D 不能左右移动、前后和镜头方向对不上"。

正确配法（`IA_Move` 的 Value Type 必须是 `Axis2D`）：

| 键 | Modifiers（按从上到下的顺序） | 输出 (X, Y) | 结果 |
|---|---|---|---|
| `W` | `Swizzle Input Axis Values`（Order 保持默认 `YXZ`） | (0, 1) | 前 |
| `S` | `Swizzle Input Axis Values`（YXZ）+ `Negate` | (0, -1) | 后 |
| `A` | `Negate`（X/Y/Z 默认全勾） | (-1, 0) | 左 |
| `D` | 不加任何修饰器 | (1, 0) | 右 |

操作：双击 `IMC_FightPlayer` → 展开 `IA_Move` → 选中某一行键（如 `W`）→ 右侧
**Modifiers** → 点 `+` 或右键 `Add Modifier` → 选对应修饰器。
`Swizzle Input Axis Values` 的 `Order` 默认就是 `YXZ`（交换 X 与 Y），不用改。

> 代码侧约定（`FightPlayerCharacter.cpp::HandleMove`）：
> `AddMovementInput(Forward, Axis.Y)`、`AddMovementInput(Right, Axis.X)`，
> 所以 W 必须给 **Y=+1**、D 给 **X=+1**。上表就是让 IMC 输出符合这个约定的值。

#### A5.2 IA_Look 必须加 Scalar（否则鼠标视角飞快）

鼠标的 `Mouse2D` 是**模拟轴**，输出的是每帧原始像素增量（几十到上百）。
不加修饰器直接喂给 `AddControllerYawInput` → 视角瞬间甩飞。

- 选中 `IA_Look` 行 → **Modifiers** → `Add Modifier → Scale`（`InputModifierScalar`）
- 设 `X = 0.15`、`Y = 0.15`；嫌快调到 `0.05`，嫌肉调到 `0.3`
- 代码里对俯仰做了取反（`AddControllerPitchInput(-Axis.Y)`），所以 Y 用**正值**即可，不要再填负数

> **左 Shift 双语义（短按闪避 / 长按奔跑）**
> 代码已改成：按下左 Shift 启动计时器，超过 `RunHoldThreshold`（默认 0.25 秒）仍按住 → 进入奔跑；
> 在阈值前松开 → 触发闪避（`UGA_PlayerDodge`）。
> **这个机制依赖 `IA_Run` 的触发器是 `Down`**：
> - `Down` → 按下时发 `Started`、松开时发 `Completed`，长短按才能区分 ✅
> - `Pressed` → `Completed` 在按下同一帧立即发出，结果变成"每次按 Shift 都闪避、永远跑不起来" ❌
> - `Hold` 也可以（行为接近 Down），但 `Down` 最直观
>
> 检查/修改：双击 `IMC_FightPlayer` → 选中 `IA_Run` 这一行 → 右侧 **Triggers** 列表里确认是 `Down`；
> 若是 `Pressed` 就删掉换成 `Down`（右键 `IA_Run` 行 → `Triggers` → `Add Trigger → Down`）。
>
> `IA_Defense`（Q）同理必须是 `Down`，否则按住格挡会一按就结束。

### A6. 关卡 & GameMode（★ 最容易漏的一步）

**1) 项目设置** → `Maps & Modes` → `Default GameMode` = `BP_FightGameMode`

**2) 打开 `BP_FightGameMode` → Class Defaults**，Details 顶部搜索框输入 `pawn`：

| 属性 | 填什么 | 说明 |
|---|---|---|
| **默认 Pawn 类** | **`BP_FightPlayer`** ★★★ | 必须是**蓝图**。留成原生的 `FightPlayerCharacter` → 角色没网格，**看不见人** |
| `HUD Widget Class` | `WBP_CombatHUD` | 分类 `Demo \| UI` |
| `Boss Bar Widget Class` | `WBP_BossBar` | 分类 `Demo \| UI` |

> **怎么验证默认 Pawn 类对不对**：PIE 运行时看 **大纲（Outliner）** 里那个 Pawn 行——
> - 显示 `FightPlayerCharacter0`、类型列 `FightPlayerCharacter`、右键"打开 C++ 类" → ❌ 用的是原生类
> - 显示 `BP_FightPlayer0`（或 `BP_FightPlayer_C_0`）→ ✅ 正确

**3) 关卡**
- 新建/另存成 `Content/Arena`（别直接改引擎模板地图）
- 拖入 **`BP_Boss`**
- 放一个 **NavMeshBoundsVolume** 框住战斗区域，按 `P` 确认绿色导航网格覆盖地面
- 放一个 **PlayerStart**（没有的话 GameMode 会在原点兜底生成一个）

> **⚠️ 常见坑①：别点到引擎的 `HUD Class`**
> GameMode 的 Details 面板里有两个名字很像的属性：
> - `HUD Class`（引擎自带，类型 `AHUD`）→ 只列出 `None / AbilitySystemDebugHUD / HUD`，**永远选不到 WBP_CombatHUD**
> - `HUD Widget Class`（我们的，分类 `Demo | UI`，类型 `UserWidget`）→ **这才是要填的**
>
> 找法：Details 顶部搜索框输入 `widget`，或滚到 `Demo | UI` 分类。
> 若列表里仍无 `WBP_CombatHUD`，说明它父类不对 → 打开该 WBP，`File → Reparent Blueprint` 选 `CombatHUDWidget`（WBP_BossBar 选 `BossHealthBarWidget`）。

> **⚠️ 常见坑②：原生类 vs 蓝图**
> 凡是"我配了但没生效"的情况，先想一件事：**运行中生成的到底是蓝图还是原生 C++ 类？**
> GameMode 里凡是 Class 类型的槽位，填的都应该是 `BP_xxx`，不是去掉 `BP_` 的那个原生类。

到这一步：能跑、能跳、能挥剑、能砍中 BOSS 掉血。

---

## B. 动画通知（决定手感，最花时间）

对每个攻击/闪避 Montage，在时间轴上加：

| 通知 | 类型 | 放在哪 |
|---|---|---|
| `MeleeHit` | NotifyState | 剑真正扫过的那一段 |
| `Combo` | NotifyState | 招式后段（这段内按攻击会接下一段） |
| `HitStop` | Notify | 刀砍中的那一帧 |
| `Invincible` | NotifyState | 闪避中段（无敌帧） |
| `Parry` | NotifyState | 防御起手前几帧（弹反窗口） |

建议参数：`HitStop` Duration 0.08 / TimeDilation 0.02；`Invincible` 约 0.3s；`Parry` 约 0.2s。

`ABP_FightPlayer` 里需要读取的（用 Cast 到 `BP_FightPlayer` 后取）：
`GetHealth / GetStamina / GetPoise`、`IsDead()`、
`State.Blocking / State.Stagger / State.Attacking` 这几个 ASC Tag。

---

## C. BOSS 招式与阶段

新建 DataAsset（`右键 → Miscellaneous → Data Asset`）：

**`UBossAttackData`**（一个招式一个资产）
```
AttackName      : 三连斩
Montage         : AM_Boss_Combo
Damage          : 18
PoiseDamage     : 12
TelegraphTime   : 0.6   ← 起手预警，玩家读招的时间
RecoverTime     : 0.8   ← 后摇，玩家反击窗口
IdealRange      : 250   ← 这个招式在什么距离用
RangeTolerance  : 120
bCanBeParried   : true  ← 红色重击设 false，逼玩家闪避
Weight          : 1.0
Cooldown        : 3.0
```

建议先做 4 个：三连斩（近）、横扫（中）、突刺（远）、蓄力重击（近，`bCanBeParried=false`）。

**`UBossPhaseData`**（一个阶段一个资产）
```
HealthRatioThreshold : 1.0 / 0.6 / 0.3   ← 三个资产分别填
Attacks              : 该阶段招式池
AttackIntervalMin/Max: 1.2 / 2.5
TelegraphTimeScale   : 二阶段改 0.8（起手更快）
```

在 `BP_Boss` 的 **Brain → Phases** 里按 1.0 / 0.6 / 0.3 顺序填三个阶段资产，
`BossAttackAbility` 填 `GA_BossAttack`。

---

## D. 手感调参位置

| 想调什么 | 改哪 |
|---|---|
| 攻防伤害公式 | `DemoDamageExecution.cpp` |
| 格挡减伤比例 / 弹反判定 | `DemoAbilitySystemLibrary.cpp` 的 `ApplyHit` |
| 无敌帧时长 | `UGE_InvincibleEffect`（或 Montage 上的 `Invincible` 通知长度） |
| 硬直 / 破防时长 | `UGE_StaggerEffect` / `UGE_BreakEffect` |
| 韧性恢复速度 | `UGE_PoiseRecoveryEffect`、角色 `BreakDuration` |
| 挥剑判定粗细 | 角色 `WeaponTraceRadius` |
| BOSS 出招节奏 | `UBossPhaseData` 的 AttackInterval |

---

## E. 扩展状态效果（你后面要的重点）

现在已有：燃烧、中毒、流血、易伤、减速、攻击提升、防御提升。

**加一个新效果只需两步**（以"冰冻"为例）：
1. `DemoGameplayTags.h/.cpp` 加一个 `Debuff_Freeze`
2. 复制 `UGE_SlowEffect` 改为 `UGE_FreezeEffect`，改 `GrantedTags` 和 Modifier（例如 MoveSpeed ×0.1）

代码里**不用改任何战斗逻辑** —— `DemoDamageExecution` 已经会读目标身上的 Tag 做增伤/减伤，
你要做的只是在 `Execute_Implementation` 里加一行 `if (TargetTags->HasTag(DemoTag::Debuff_Freeze)) { ... }`。

施加方式（蓝图或 C++ 都行）：
```cpp
UDemoAbilitySystemLibrary::ApplyEffectToSelf(Target, UGE_FreezeEffect::StaticClass());
```

---

## 编译

双击项目根目录 `BuildDemo.bat`（**编辑器要先关**，否则 dll 被占用）。
日志写入 `build_log.txt`，脚本会自动筛出 Result 和 error 行。
