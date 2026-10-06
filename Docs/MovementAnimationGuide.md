# 移动动画完整流程清单（前后左右 8 向横移 locomotion）

> 目标：WASD 前后左右 + 4 个斜向，全部有对应走/跑动画；角色身体朝向镜头/锁定方向，移动是"横移(strafe)"。
> 之前只有"前后"动画，是因为角色被设成"永远转向移动方向"，按 D 时身体先转过去朝前，自然只剩前行动画。

---

## 0. 已做好的 C++ 改动（无需你操作）

`Source/demo/Player/FightPlayerCharacter.cpp` 构造函数已改为横移朝向：

```cpp
bUseControllerRotationYaw = false;
bUseControllerDesiredRotation = true;          // 身体平滑转向控制器朝向(镜头/锁定)，而非转向移动方向
if (UCharacterMovementComponent* MoveComp = GetCharacterMovement())
{
    MoveComp->bOrientRotationToMovement = false; // 关键：关闭"转向移动方向"，左右/后退动画才会触发
    MoveComp->RotationRate = FRotator(0.f, 720.f, 0.f);
}
```

> 改完要**关掉编辑器**重新编译（`demoEditor` 目标）才能生效。移动方向仍由 `HandleMove` 按控制器朝向计算，所以"移动跟随镜头"不变，只是身体不再自动追着移动方向转。

---

## 1. 资产清单（已确认你都有，路径 `Content/SwordAnimsetPro/Animations/`）

| 类型 | 资产名 | 备注 |
|------|--------|------|
| 待机 | `Idle_Anim` | 在 `In-Place/` |
| 走（8 向） | `Walk_F_Anim` `Walk_B_Anim` `Walk_L_Anim` `Walk_R_Anim` `Walk_F_L_Anim` `Walk_F_R_Anim` `Walk_B_L_Anim` `Walk_B_R_Anim` | 在 `Root-Motion/` |
| 跑（8 向） | `Run_F_Anim` `Run_B_Anim` `Run_L_Anim` `Run_R_Anim` `Run_F_L_Anim` `Run_F_R_Anim` `Run_B_L_Anim` `Run_B_R_Anim` | 在 `Root-Motion/` |

> ⚠️ **Root Motion 必须处理（重要修正）**：早期版本曾误述"ABP 默认 Root Motion Mode=From Montages Only，混合空间里的 root motion 不生效、等效原地播"。**这是错的。** 该模式只决定"是否提取 root motion 来驱动位移"，但**动画里的根骨位移照样会播出来**。结果就是 Run 每播一轮根骨前移一段、到结尾瞬间跳回原点 → 表现为"跑完拉回、不一直往前"，整体也很不自然。
>
> **正确做法（原地 in-place）**：对每个 Walk/Run 动画资产，Details → Root Motion 分类下：
> - `Root Motion Root Lock` = **`Zero`**
> - `Force Root Lock` = **勾选**
>
> 引擎 `bForceRootLock` 的设计意图就是"即使不启用 root motion 也强制锁根骨"，设为 `Zero` 后根骨每帧钉死在原点 → 循环无缝、不再拉回，位移仍由 `AddMovementInput` 驱动。16 个动画手动点太累，**一键脚本见 `Tools/FixLocomotionRootMotion.py`**（编辑器 Output Log 切 Python 粘贴运行，或 `exec(open(...).read())`）。跑完**不用重编、不用重启**，直接 PIE 验证。

---

## 2. 重建 BS_FightPlayer（2D 横移混合，Forward × Right 轴）

你的 BS 轴名已经是 `Forward` / `Right`，**保留这两个轴名**，只改范围和采样点。

1. 内容浏览器双击 `BS_FightPlayer` 打开。
2. **轴设置**（左侧 Asset Details）：
   - 轴 1 名称 `Forward`，范围 **-700 ~ 700**
   - 轴 2 名称 `Right`，范围 **-700 ~ 700**
3. 清空旧采样（之前只有 Idle + 4 个 Walk），按下面坐标**逐一放置**（在混合空间画布里右键 `Add Sample`，选动画后拖动到对应坐标，或在 Details 的 Samples 数组里填 X/Y）：

| 坐标 (Forward, Right) | 动画 | 方向 |
|---|---|---|
| (0, 0) | `Idle_Anim` | 待机 |
| (400, 0) | `Walk_F_Anim` | 前 |
| (-400, 0) | `Walk_B_Anim` | 后 |
| (0, 400) | `Walk_R_Anim` | 右 |
| (0, -400) | `Walk_L_Anim` | 左 |
| (283, 283) | `Walk_F_R_Anim` | 前右 |
| (-283, 283) | `Walk_B_R_Anim` | 后右 |
| (-283, -283) | `Walk_B_L_Anim` | 后左 |
| (283, -283) | `Walk_F_L_Anim` | 前左 |
| (700, 0) | `Run_F_Anim` | 前(跑) |
| (-700, 0) | `Run_B_Anim` | 后(跑) |
| (0, 700) | `Run_R_Anim` | 右(跑) |
| (0, -700) | `Run_L_Anim` | 左(跑) |
| (495, 495) | `Run_F_R_Anim` | 前右(跑) |
| (-495, 495) | `Run_B_R_Anim` | 后右(跑) |
| (-495, -495) | `Run_B_L_Anim` | 后左(跑) |
| (495, -495) | `Run_F_L_Anim` | 前左(跑) |

> 坐标含义：Walk 环半径 = `WalkSpeed`(400)，Run 环半径 = `RunSpeed`(700)；斜向 = 半径 / √2。
> 拖完在画布里能看到一张"米字格"网格，每个点都接了动画即正确。

---

## 3. ABP_FightPlayer：加变量

My Blueprint 面板 → Variables，确认/新增：

| 变量 | 类型 |
|------|------|
| `ForwardSpeed` | Float |
| `RightSpeed` | Float |
| `IsInAir` | Bool（应已存在） |
| `Speed` | Float（已存在，本方案不依赖它，可留着） |

---

## 4. ABP EventGraph：每帧算出 ForwardSpeed / RightSpeed

打开 ABP → Event Graph。把现有的 `Event Blueprint Update Animation` 链路改成：

**执行链（白线）：**
```
Event Blueprint Update Animation
  └─► Cast To BP_FightPlayer (或 FightPlayerCharacter)
        └─► Set ForwardSpeed
              └─► Set RightSpeed
                    └─► Set IsInAir
```

**数据链：**
```
TryGetPawnOwner ─► Cast 的对象脚
Cast 的 "As BP_FightPlayer" 脚
   ├─► Get Velocity ──────────────────────┐
   ├─► Get Actor Forward Vector ─┐         │
   └─► Get Actor Right Vector ──┐│         │
                                ││         │
   (Velocity · Forward) = Dot   ◄┘┘         │
        └─► Set ForwardSpeed 的 value 脚 ◄─┘ (Velocity 也接到这里)
   (Velocity · Right)   = Dot   ◄── (Right 也接)
        └─► Set RightSpeed 的 value 脚
   同一 "As" 脚 ─► Is Falling ─► Set IsInAir 的 value 脚
```

**节点操作要点：**
- `Get Velocity`、`Get Actor Forward Vector`、`Get Actor Right Vector` 都从 **Cast 出来的角色脚**拖出。
- `Dot Product`（向量点乘）节点：把 Velocity 和 Forward 接进去得到 `ForwardSpeed`，Velocity 和 Right 接进去得到 `RightSpeed`。
- `ForwardSpeed` = 速度在"身体前方"的投影（向前走为正、后退为负）；`RightSpeed` = 在"身体右方"的投影（向右为正、向左为负）。这正好对应 BS 的 Forward/Right 轴。

---

## 5. ABP AnimGraph：用 BlendSpacePlayer，删掉状态机

1. 选中那个**空的 State Machine** 节点（就是你之前看到"没动画"的元凶，里面没有有效转换），按 `Delete` 删掉。
2. 你的 ABP 里已经有一个 **Blend Space Player** 节点（指向 BS_FightPlayer）。若已删除找不到，右键空白处 → `Animation` → `Blend Space Player`，在 Details 里 `Blend Space = BS_FightPlayer`。
3. 在该 BlendSpacePlayer 节点上：
   - 找到 **`Forward`** 输入脚（对应 BS 的 Forward 轴）→ 拖出 → `Promote to Variable` → 选 `ForwardSpeed`
   - 找到 **`Right`** 输入脚 → 拖出 → `Promote to Variable` → 选 `RightSpeed`
4. 连线：`BlendSpacePlayer` 的输出 Pose → `Output Pose`。
   - 想保留跳跃分支：插入 `Blend Poses by bool`（Active Value = `IsInAir`），True 脚接 `Jump_Loop_Anim`，False 脚接 `BlendSpacePlayer`，再 → `Output Pose`。
5. **Compile** ABP。

---

## 6. 挂载确认（应已正确，复查一遍）

`BP_FightPlayer` → 选中 **Mesh** 组件 → Details → Animation：
- `Anim Class = ABP_FightPlayer`
- `Animation Mode = Use Animation Blueprint`

---

## 7. 编译 + 验证

1. **关闭编辑器**（否则 dll 被占用，编不过）。
2. 双击 `BuildDemo.bat`（已修正为编译 `demoEditor` 目标），或 VS 里编 `demoEditor`。
3. 开编辑器 → PIE：
   - 按住 **W** → 前行动画；**S** → 后退动画；**A** → 左移；**D** → 右移；斜向组合 → 斜向动画。
   - 长按 **左 Shift** → 切到对应方向的跑动画（RunSpeed=700，走到 Run 环）。
   - 打开 ABP 调试器，把 Debug Object 选成角色，确认 `ForwardSpeed` / `RightSpeed` 在移动时实时变化（前移时 ForwardSpeed≈±400，右移时 RightSpeed≈±400）。

---

## 8. 排错表

| 现象 | 原因 | 处理 |
|------|------|------|
| 仍然只有"前"动画，左右/后退没反应 | C++ 没重编或 `bOrientRotationToMovement` 仍为 true | 确认关编辑器重编；搜代码确认 `bOrientRotationToMovement = false` |
| 角色移动但身体不转向、动画方向对但人一直面朝镜头 | 正常（横移本就如此） | 无需处理 |
| 角色乱转 / 抽搐 | `bUseControllerDesiredRotation` 与 `bUseControllerRotationYaw` 同时 true | 确认 yaw 为 false |
| 跑/走时动画"拉回"、不连续、整体不自然 | Walk/Run 是 root-motion 动画，根骨位移每轮播完跳回原点 | 对每个动画设 `Root Motion Root Lock = Zero` + 勾 `Force Root Lock`（见上方"Root Motion 必须处理"）；或跑 `Tools/FixLocomotionRootMotion.py` |
| 动画有但脚底打滑（漂移感） | `WalkSpeed`/`RunSpeed` 与动画自身 root-motion 速度不一致，或**还没做上面那步**（root motion 仍在位移） | 先做 Root Lock=Zero；再按动画实际速度微调 `WalkSpeed`/`RunSpeed`（Walk≈动画 root motion 速度，Run 同理）使脚速匹配 |
| 某些方向姿势跳变 | BS 该坐标点没放动画 / 坐标填错 | 回第 2 步核对米字格 |
| Speed 一直是 0 | EventGraph 没接对 | 看第 4 步，用调试器看 ForwardSpeed/RightSpeed 读数 |

---

## 9. 一句话总结

> 之前是"转向移动方向"导致只播前行动画；现在改成**横移朝向 + 2D 横移混合空间（Forward×Right 轴，8 向走/跑采样）**，ABP 每帧用 `Velocity` 点乘身体前/右向量算出 `ForwardSpeed`/`RightSpeed` 喂给混合空间——前后左右斜向全齐。
