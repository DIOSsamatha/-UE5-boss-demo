# 排错手册 — demo（UE 5.7 GAS BOSS 战）

> 配套文档：`Docs/EditorSetupGuide.md`（怎么配）。本文档解决"配了但没生效"。

## 排查顺序（照着走，90% 的问题 2 步定位）

1. **看 PIE 大纲的「类型」列** —— 判断运行中生成的到底是**蓝图**还是**原生 C++ 类**。
   右键该行，菜单里写的是"打开 BP_XXX"还是"打开 C++ 类"，一眼就分清。
2. **扫资产二进制** —— 确认配置真的存进 `.uasset` 了：

   ```
   python Tools/InspectAssets.py                 # 扫整个 Content
   python Tools/InspectAssets.py Content/Game    # 只扫某个目录
   ```

   原理：`.uasset` 是序列化二进制，父类 / 引用的资产 / 属性名 / 键位名都以字符串存在文件里，
   直接扫字符串就能比对"资产里到底存了什么"，不用开编辑器、不用逐个点开看。

   > 单字符键名（`W`/`A`/`S`/`D`/`Q`/`E`）必须**精确匹配 FName**：名字表里 FString 按
   > `int32 长度 + 字符 + \0` 序列化（长度含末尾的 `\0`），所以 `"W"` 存成 `02 00 00 00 57 00`。
   > 用 `b"Q" in data` 这种子串匹配会被 `GA_*` 之类的字符串误命中，工具里已按精确格式实现。

---

## 症状 1：PIE 里看不到角色（人没了）

**现象**：按 Play 只有场景，画面里没有角色；但大纲里确实有一个 Pawn 节点。

**根因**：`BP_FightGameMode` 的 **默认 Pawn 类**留成了原生 `FightPlayerCharacter`。
网格（`SK_Mannequin`）、动画、`IMC_FightPlayer`、5 个 GA 全都配在 **`BP_FightPlayer`** 里，
原生类身上什么都没有 → 生成出来是个**没有网格的角色** → 看不见，但逻辑上它存在。

**判定**：大纲里显示 `FightPlayerCharacter0`、类型列是 `FightPlayerCharacter` → 用的是原生类（❌）
　　　　大纲里显示 `BP_FightPlayer0` → 正确（✅）

**修复**：打开 `BP_FightGameMode` → Class Defaults → Details 搜 `pawn`
→ **默认 Pawn 类 = `BP_FightPlayer`** → 编译保存 → Play。

> **引申规律**：GameMode 里凡是 Class 类型的槽位，填的都应该带 `BP_` 前缀的**蓝图**。
> 填成去掉 `BP_` 的原生类，等于把蓝图里配的一切全部丢掉。

---

## 症状 2：按键完全没反应（一个键都不行）

**★ 头号原因：`BP_FightPlayer` 的 8 个 InputAction 槽位是空的。**

代码里每个按键绑定都写成 `if (MoveAction) { BindAction(...) }`。这 8 个属性是 `EditDefaultsOnly` 指针，
空着时对应的 `BindAction` 会被**直接跳过** —— **不报错、不崩溃、就是没反应**，极易被忽略。

判定方法（不开编辑器）：`python Tools/InspectAssets.py Content/FightPlayer/System`，
看 `BP_FightPlayer` 那一段的 `InputAction *` 是不是 `[缺! ]`。

**修复**：打开 `BP_FightPlayer` → Class Defaults → Details 搜 `input`（或滚到 `Demo | Input` 分类），
把 8 个槽位逐个填上：

| 槽位名 | 填哪个资产 |
|---|---|
| `Move Action` | `IA_Move` |
| `Look Action` | `IA_Look` |
| `Jump Action` | `IA_Jump` |
| `Run Action` | `IA_Run` |
| `Normal Attack Action` | `IA_NormalAttack` |
| `Attack Skill Action` | `IA_AttackSkill` |
| `Defense Action` | `IA_Defense` |
| `Flash Action` | `IA_Flash` |

> 构造函数里已加兜底（按 `/Game/FightPlayer/Input/` 约定路径自动加载），
> 兜底生效时 PIE 的 **Output Log** 会打一条 `[Fight] 已注册输入映射 IMC_FightPlayer`；
> 若仍有槽位空着，会打红色 Error 列出槽位名。**但蓝图上显式填一遍最保险**（以后挪资产不会失效）。

**其他按顺序查**：
1. `BP_FightPlayer` 的 `DefaultAbilities` 是否有 5 个 GA（缺 → 有按键但技能放不出来）
2. `BP_FightPlayer` 的 `DefaultMappingContext` 是否 = `IMC_FightPlayer`
3. `IMC_FightPlayer` 里 8 个 `IA_*` 是否**每个都绑了键**（漏一个 = 那个动作永远不触发）
   - 本项目实测：IMC 里只有 10 个键名（`W A S D E` + 空格 + 左Shift + 鼠标左右键 + 鼠标2D），
     但 8 个 Action 里 Move 要占 4 个键 → **`IA_Defense` 和 `IA_Flash` 只有一个绑了键（E），另一个是空的**
4. **需要"按住生效"的 Action 必须是 `Down` 触发器**：`IA_Defense`（按住格挡）、`IA_Run`（按住疾跑）
   - 用 `Pressed` 的话 `Started` / `Completed` 会在同一帧一起发 → 一按就结束（用 `Hold` 则要等延迟）
   - `IA_Jump` / `IA_NormalAttack` 用 `Pressed` 没问题（代码只监听 `Started`）
   - 本项目实测：IMC 里出现的触发器只有 `Pressed` / `Hold`，**没有 `Down`** → 这两项需要补
5. PIE 时**先用鼠标点一下游戏视口**再按键——视口没焦点时按键会被编辑器吃掉
6. 真查不出来就看 **Output Log**：`Cast<UEnhancedInputComponent>` 失败会打 Error；
   `Missing.Num() > 0` 会列出空槽位

---

## 症状 3：移动方向不对（A/D 不能左右、前后和镜头方向对不上）

**现象**：能走，但方向乱——按 A/D 不会左右移动，按 W/S 也不是镜头的前后方向，
四个键像在做同一件事。

**根因**：`IA_Move` 的 4 个键**没有加 Modifier**。

数字键（W/A/S/D）的原始值**只写在 X 轴上**，这是引擎的固定行为：

```cpp
// PlayerInput.cpp（非模拟键分支）
case IE_Pressed:
    KeyState.RawValueAccumulator.X = Params.AmountDepressed;  // 只有 X 被赋值，Y/Z 恒为 0
```

于是 `IA_Move`（Value Type = `Axis2D`）收到的原始值恒为 **`(1, 0)`**。
代码里 `AddMovementInput(Forward, Axis.Y)` / `AddMovementInput(Right, Axis.X)`，
`Axis.Y` 永远是 0、`Axis.X` 永远是 1 → **四个键都在执行"向右移动"**。

**判定**：`python Tools/InspectAssets.py Content/FightPlayer/Input`，
看 `IMC_FightPlayer` 那段是否出现
`[注意] 没有 SwizzleAxis 修饰器` / `[注意] 没有 Negate 修饰器`。

**修复**：`IA_Move` 的 4 个键按下表加修饰器（Modifiers 按从上到下顺序生效）：

| 键 | Modifiers | 输出 (X, Y) | 结果 |
|---|---|---|---|
| `W` | `Swizzle Input Axis Values`（Order 默认 `YXZ`） | (0, 1) | 前 |
| `S` | `Swizzle Input Axis Values`（YXZ）+ `Negate` | (0, -1) | 后 |
| `A` | `Negate`（X/Y/Z 默认全勾） | (-1, 0) | 左 |
| `D` | 不加 | (1, 0) | 右 |

- 操作：双击 `IMC_FightPlayer` → 展开 `IA_Move` → 选中某一行键 → 右侧 **Modifiers** → `Add Modifier`
- `Swizzle Input Axis Values` 的 `Order` 默认就是 `YXZ`（交换 X 与 Y），**不用改**
- 前提：`IA_Move` 的 **Value Type 必须是 `Axis2D`**（本项目实测已是，和引擎模板一致）

> ⚠️ 这里有个反直觉的点：**`D` 才是"不加修饰器"的那个**，不是全部都不加。
> 曾经有一版文档写成"A/D 加 Negate、一个都不要加"就错了——四个键全不加，
> 结果就是四个方向全变成同一个方向。

**同时要改的（同一段配置里的连带问题）**：

| 项 | 现象 | 修法 |
|---|---|---|
| `IA_Look` 没有 `Scalar` | 鼠标视角**转得飞快**（原始像素增量直接喂相机） | 给 `IA_Look` 加 `Scale`，`X/Y ≈ 0.15`（代码已做俯仰取反，Y 用正值） |
| 没有 `Down` 触发器 | `IA_Defense`/`IA_Run` 按住会一按就结束 | 这两个 Action 的 Trigger 改成 `Down` |

---

## 症状 4：挥剑没动画 / 打不掉血

- **没动画**：GA 蓝图里的 Montage 字段是空的。SwordAnimsetPro 里的 `*_Anim` 是 **AnimSequence**，
  要先 `右键 → Create → Create AnimMontage` 转成 **AnimMontage** 才能填进 GA。
- **打不掉血**：剑的骨骼网格（`Weapon_Sword`）上缺 socket **`WeaponStart` / `WeaponEnd`**。
  命中检测靠这两个 socket 连成的线段，名字必须完全一致，否则退化成"角色前方一段"。
- **动画里没有受击判定**：AnimMontage 上没加 `MeleeHit` 通知 → 不会有命中窗口。

---

## 症状 5：BOSS 不动 / 不出来

- **不出来**：关卡里没拖 `BP_Boss`（`Content/*.umap` 里搜不到 `BP_Boss` 就是这个原因）
- **不动**：关卡里没有 **NavMeshBoundsVolume** 框住战斗区 → 寻路失败 → 只停在原地
  - 按 `P` 看绿色导航网格是否覆盖地面
- **不出招**：`BP_Boss` 的 `Brain` 里 `Phases` 为空 → 先建 `Content/Data/` 下的 BossAttackData / BossPhaseData

---

## 关卡要另存到 Content

用 `File → New Level` 建的关卡默认是**未保存**的（标题栏 Untitled）。
PIE 能跑，但一旦切换关卡就丢了。**先 `Ctrl+S` 存成 `Content/Arena.umap`** 再往里放东西。

（本项目 `Config/DefaultEngine.ini` 里 `GameDefaultMap` 指向引擎模板地图，
独立运行时会打开引擎地图而不是你的关卡 —— 编辑器里 PIE 用的是"当前打开的关卡"，不受影响。
以后要打包再改这个配置。）
