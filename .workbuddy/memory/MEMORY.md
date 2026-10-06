# 项目长期记忆 — demo（UE 5.7 GAS 持剑 BOSS 战）

## 概览
- 引擎 `K:\UE_5.7`，项目 `K:\selfgame\demo`，模块 `demo`；斜俯视持剑 BOSS 战，用 **GAS**
- 交付：**完整 C++ + 分步编辑器清单**（默认不要 Python 方案，除非造资产）
- 蓝图 `BP_FightPlayer`/`BP_Boss`/`BP_FightGameMode`（GameMode 所有 Class 槽位填 BP_）；HUD `WBP_CombatHUD`/`WBP_BossBar` 是空壳，布局由 C++ `EnsureDefaultTree()` 生成
- BOSS = ParagonKhaimera（骨架 Khaimera）；玩家 = SwordAnimsetPro；武器必须 StaticMesh
- 关卡 `/Game/Fab/FightArena`（`Tools/SetupArenaLevel.py` 生成）；`GameDefaultMap`/`EditorStartupMap` 都指向它
  - ★ 这份 Fab 竞技场（103k 面）的简单碰撞原本是**一个罩住整场的凸包** → 角色被托在 AABB 顶面（Z=4175）悬空；
    已用 `Tools/FixArenaCollision.py` 改成 `CTF_USE_COMPLEX_AS_SIMPLE`，脚本改资产后**必须重启编辑器**
  - 诊断套路见技能 `ue-mesh-collision-float`：**"站在模型上却悬空" 先量脚骨世界 Z 是否 = 场景 AABB 顶**
  - BOSS `AggroRange=1600`：出生点相隔超 16m → BOSS 全程 Idle（曾因玩家/BOSS 相隔 85m "打不起来"）；
    摆位用 `Tools/MoveArenaSpawn.py`，出生点距 BOSS ~1500cm
  - UE5.7 Python：`unreal.KismetSystemLibrary` 不存在（用 `unreal.SystemLibrary`）；
    `unreal.Rotator(a,b,c)` 位置参数是 **(roll,pitch,yaw)**，一律用关键字构造；`Vector` 无 `.size()`
- 技能：`ue-action-combat-feel` / `ue-boss-vfx-pipeline` / `ue-montage-rootmotion-diagnosis` / `ue-cpp-umg-hud` / `ue-inplace-anim-foot-skate` / `ue-build-silent-child-failure`；移动动画细节见 `Docs/MovementAnimationGuide.md`

## 声音 / BGM（★ UE5.7 音频 API 与 UE4 写法不同）
- **BGM 挂在 `AFightGameMode::StartBgm()`**（`BeginPlay` 最开头调用），零配置；
  `BgmTrack`(TSoftObjectPtr，可空) → 空则回落硬编码 `DefaultBgmPath`（防 BP CDO 旧值）；
  资产 `Content/BGM/BGM_BossTheme`（SoundWave 98.5s，`Tools/ImportBgm.py` 导入，可反复跑）
- ⚠⚠ **UE5.7 循环播放唯一路径 = `USoundWave::bLooping`**：
  `CreateSound2D` 返回 **UAudioComponent**（**没有 bLooping**）；`USoundComponent` 类在 5.7
  **已删除**（`Components/SoundComponent.h` 不存在）→ 网上抄的 `SoundComp->bLooping=true` 会 C2065
- 打开 bLooping 后 `GetDuration()` 返回 **10000.0s**（= `INDEFINITELY_LOOPING_DURATION` 哨兵）= 无限循环，**不是 bug**
- `CreateSound2D` 必须 `bAutoDestroy=false`（默认 true 播完自毁→第二轮没声音）+ `bPersistAcrossLevelTransition=true`
- 改音量：BP_FightGameMode → `Demo|Audio` → BgmVolume，或运行时 `SetBgmVolume`（用 `SetVolumeMultiplier`）
- ⚠ **无头验证必须去掉 `-NoSound`**（`RunHeadlessGame.py` 默认带）→ 音频设备不初始化时
  `CreateSound2D` 返回 nullptr，会误判"代码没生效"。带音频跑看 `[BGM] ▶ 循环播放：… 循环=1`
- UE Python：**UPROPERTY 不一定出现在 `dir(obj)`** → 候选属性名要逐个 try，别用 `if name in dir()` 筛
- 音频导入日志里的 `BINKA decoder not found` / `Ensure condition failed: Factory` 是**工厂探测噪音**，导入其实成功了

## 编译铁律（失败先查这些）
- ★★ **用户开着编辑器时我编不出新 DLL**：`run_build.py --manual` 也会在链接步失败
  `LNK1104: 无法打开文件 Binaries\Win64\UnrealEditor-demo.dll`（DLL 被进程锁住）。
  改 C++ 前先 `tasklist` 查 UnrealEditor/LiveCodingConsole；只能请用户**关编辑器**或按 **Ctrl+Alt+F11** 走 Live Coding。
- 判"用户测的是哪一版" = 比 **DLL 时间戳 vs 编辑器启动时间**；
  若某次 build exit 0，说明那时编辑器没开，之后启动的编辑器就加载了那一版
- **编辑器开着 Live Coding → `run_build.py` exit 6**，日志 `Unable to build while Live Coding is active` 且"真实编译诊断 0 条" = **不是代码错**。解法 `python Tools/run_build.py --manual`（不用关编辑器，~32s）。⚠ 编成功≠用户测到了：DLL 进不了他正在跑的进程，**必须重启编辑器或让他按 `Ctrl+Alt+F11`**
- **查 UE 进程只能用 `tasklist`（GBK）**：`Get-Process` 在本会话看不到用户进程，会误判"编辑器没开"；材质 .uasset 被锁（Error Code 32）= 编辑器开着
- **同文件改动必须串行**（同消息多个 Edit → 静默丢改动，工具仍报 success，编译才报 C2511/C2065）。改完 Grep 回读关键符号再编译
- 给别的代码用的 `UFUNCTION`/属性必须 **public**（protected → C2248）；访问 `AttributeSet->GetXxx()` 的函数体必须在 **.cpp** 且 `#include "Core/DemoAttributeSet.h"`（否则 C2027）
- `UE_DECLARE_GAMEPLAY_TAG_EXTERN` 必须配 `UE_DEFINE_GAMEPLAY_TAG_COMMENT`（LNK2001）；`UE_LOG` 格式串不能写裸 `%`（C2338），`TEXT()` 只是字面量宏不能包变量
- **C++ 默认值压不过蓝图 CDO 旧值** → 改已有属性的默认值要换【新名字】
- `TObjectPtr<T>` 与裸 `T*` 不能混进三元（C2445）；`FVector2D` 是 `TVector2<double>`，**没有 (float,float) 构造**；const 方法里给成员赋值报 C2678（误导性强）
- 改函数签名必须同步改 .h 声明，否则 C2660 连锁误报 —— **先看第一条错误**
- **没有真资产时不要用引擎基础几何体凑造型**（白模在求职 Demo 里是减分项）。需要新外观先问用户

## 构建 / 取证环境
- 唯一通道：`cd /k/selfgame/demo && python Tools/run_build.py`（`demoEditor` + `-NoUBA`，exit 0 = 成功）；被锁时加 `--manual`（走 `Tools/ManualBuild.py`，**必须 `--force`**，cwd 必须是 `K:\UE_5.7\Engine\Source`）
- 判改动是否编入：DLL 时间戳 + `find Source -newer <dll>`；**VS F5 启动项目必须是 `demo`**（升级 VS 后 .suo 丢了要重设）
- ⚠ **Bash coreutils / shim 间歇失效**（ls/grep/find/tail 全 command not found）→ 改用 Grep/Glob/Read 或 python 一行式
- 无头实跑：`python Tools/RunHeadlessGame.py [秒数] [控制台命令]`，例 `python Tools/RunHeadlessGame.py 70 "demo.BossGuardTest 14"`（第二参数拼进 `-ExecCmds=`）；默认加载 `GameDefaultMap`
- ⚠⚠ **无头 `-NullRHI` 只有 ~6.5 FPS** → "每帧位移"统计不可信，用「整局累计位移」或 p90，别看中位数；手感/空步类必须 PIE 或录屏验收
- **造/补资产可不开编辑器**：`UnrealEditor-Cmd.exe <uproject> -run=pythonscript -script="Tools/xxx.py" -unattended -NoLiveCoding -NullRHI`（实测 33s）
- UE Python 读不到 `AnimMontage.slot_anim_tracks`/`target_skeleton`、`get_current_level_name` 不存在 → 骨架核对必须在 C++ 侧；`set_actor_location` 是 `(loc, sweep, teleport)` 三参必填

## UE 5.7 API 差异（GAS / 通用）
- `ATTRIBUTE_ACCESSORS` 仍可用；`ULegacyCameraShake` 在 **EngineCameras 插件**（include `Shakes/LegacyCameraShake.h` + Build.cs 加 `"EngineCameras"`），`UCameraShakeBase::PlaySpace` 是 private
- GE 已组件化：`GrantedTags`/`BlockAbilityTags`/`CancelAbilityTags` 废弃 → 基类 `UDemoGameplayEffectBase` 把注册延迟到 `PostInitProperties`，子类重写 `ConfigureGrantedTags/ConfigureBlockedAbilities/CanceledAbilityOnApply`
- 禁在 UObject 构造函数里 `FindOrAddComponent`（CDO 崩溃）→ 延迟到 `PostInitProperties()`
- `AbilityTags` 废弃 → 构造里 `SetAssetTags()`，读 `GetAssetTags().HasTag()`；`FInheritedTagContainer` 无 `IsEmpty()`，加 tag 用 `.Added.AddTag()`
- `FGameplayAbilitySpec::Ability` 是 `TObjectPtr` → `Spec.Ability.Get()->GetClass()`
- SetByCaller：`SpecHandle = ASC->MakeOutgoingSpec(Cls,1.f,Ctx); SpecHandle.Data->SetSetByCallerMagnitude(DemoTag::Data_Xxx,Val); ASC->ApplyGameplayEffectSpecToSelf(*SpecHandle.Data.Get());`
- `EPathFollowingRequestResult` 需 `#include "Navigation/PathFollowingComponent.h"`；`GetAllActorsOfClass` 须 `TArray<AActor*>`

## 核心系统要点
- **伤害唯一入口** `UDemoAbilitySystemLibrary::ApplyHit` → `UDemoDamageEffect` → `UDemoDamageExecution` → `PostGameplayEffectExecute`；顺序 阵营过滤→无敌帧→弹反→格挡→破防暴击。伤害门控 `bIntendedDamage = BaseDamage>0 || AttackPower>0`
- **所有资源修改走 GE**（直接 `SetHealth` 不触发 `OnHealthChanged`+clamp）；清空用 `SetNumericAttributeBase`（走 GE 会自激）
- **取消"GE 授予的状态"必须 `ASC->RemoveActiveEffectsWithGrantedTags({Tag})`**（`RemoveLooseGameplayTag` 会被写回）；LooseTag 才用 `RemoveLooseGameplayTag`
- **三条资源条硬分离**：`Poise`=被打削→破防；`Anomaly*`=BOSS 两条积累槽→失衡/击破；`Posture`=格挡消耗
- HUD 绑 `OnHitLanded` 必须 `UFUNCTION`；UMG `ProjectWorldLocationToScreen` 必须除以 `GetViewportScale()`（≈0.76）
- 命中音位置传**命中点**不是角色；命中反馈**FOV 脉冲 `PulseCameraFOV(...,bAllowWhileLocked=true)`** 是锁定下的主力（根因 `ApplyCameraPunch` 内 `if(IsLockedOn()) return;`）
- 锁定相机：`CamPos = BoomOrigin - Desired.Vector()*ArmLen + Desired.RotateVector(SocketOff)`（写 `+` 有 7~8° 偏移）；`SetUsingAbsoluteRotation(true)`+`bDoCollisionTest=false`+`PC->SetControlRotation(Smoothed)`；Turn In Place 死区 **5°**
- 体力：普攻 8/10/12/16、技能 25、空弹 5；耗尽软惩罚（2.5s、净恢复 15/s）。贪刀：连段绕回 `bOvercommitted`（后摇+0.15s、伤害×0.7）；0.2s 内二次闪避 ×0.6；误弹 0.4s 硬直+体力-15
- 键位：左键普攻连段 / 右键弹刀 / 左Shift 短按闪避长按奔跑 / Q·E 换目标 / 中键锁定
- 状态信号四通道：起手=红(快闪)/受击=白(脉冲)/失衡=绿(慢呼吸)/举盾=冷蓝(0.7Hz)，每条至少【材质光+UI】两通道
- ⚠⚠ **UMG 里"Image 不设 brush" = 一块实心色板**（不是"什么都不画"）：`ScreenFlash` 曾经没设 brush，
  于是每次 `FlashScreen()` 都是"盖一块半透明全屏矩形"而不是闪光 → 玩家两次反馈"稳心时有灰板"。
  治本：`MakeRadialFlashTexture()` 运行时生成 128×128 径向渐变（中心白、`pow(1-r,1.6)` 衰减）挂给 brush。
  ⚠ **一次事件可能同时绑多个 Handler**（弹反 + 完美弹反）→ 两层屏闪叠着闪，
  实测"被攻击→完美弹反(稳心)"= 冷蓝 0.24 紧跟金闪 0.45 = 一大片色板。
  "被攻击"路径上的屏闪已全部删掉（拼刀/普通弹反/完美弹反），只留镜头冲击 + 飘字 + 顿帧；
  剩下的只有暴击 0.16 / 完美闪避 0.32 / 普通闪避 0.16（已是中央泛光）
- ⚠ `TBulkData` 在 UE5.7 是 `Lock(LockFlags)/Unlock()`，**没有** `LockBulkData/UnlockBulkData`（C2039）
- ★★★ **"画面里出现一块板"九成是"贴地扩散环"**：`ADemoImpactBurst` 的 `RingMesh` 是引擎基础
  Plane(100cm)，最终边长 = `Radius × RingEndScale(2.6) × 2`；弹反 Radius=70 → **3.64m 的平面**，
  生成点在两人之间（离镜头 1~2m）、加法混合、HDR 强度 7.7 → 满屏糊一块 = 用户说的"灰板"。
  `bUseFlash=false` 只关中心球，**环一直开着**（这是盲点）。已给 `SpawnBurst` 加 `bUseRing`，
  弹反/拼刀传 false；其它路径（BOSS 受击/格挡/弹丸）保持原样
- ⚠ 排查"画面里有块板"先**算尺寸**：网格基准尺寸 × 缩放 × 生成点离镜头距离；近身特效的环要压到 `RingEndScale ≤ 1`
- **单 Overlay 槽仲裁**：红/绿/蓝共用 `SetOverlayMaterial`，谁后写谁赢 → 用 `RefreshBossOverlay()` 按优先级 **起手红 > 失衡绿 > 格挡蓝** 挂载，`AppliedOverlayMID` 去重
- 状态光"不贴合"多半是**过曝**不是偏移：Unlit+Additive 的 `Intensity` 写太高会被 Bloom 吞掉模型。现用 `0.15+Pulse*0.45`、Fresnel 权重 `0.06+0.94*F^2.2`

## ★ BOSS 格挡（2026-10-03 重做，实测 21%~30%）
- 判定必须在 **ApplyHit 扣血之前**：`ADemoCharacterBase::TryGuardIncomingHit()`（BOSS 重写）。放 `OnDamagedReaction`（伤害后的表现回调）里掷骰 = 血照掉、硬直照给，玩家看不出"挡了"
- 资格判定**不能看 `IsAttacking()`**（GAS `State_Attacking` 覆盖"预警→出招→后摇"整段，后摇被误禁）→ 按 **Brain 状态机**，仅 Telegraph/Attack 不挡
- 参数：`GuardRollChance=0.55` / `GuardRollCooldown=1.1s` / `GuardPityHits=3`（连续 3 次有资格没中 → 强制挡）/ `GuardHoldTime=0.8s`
- ⚠⚠ **常驻 `State_Blocking` 是坑**：`UpdateGuardStance` 曾在 Idle/Chase/Reposition 三态都挂 tag → 实测 14 次命中挡 7 次（太肉），且走 `bHeldGuardBlock` 分支**不调 TryGuardIncomingHit → 举盾蒙太奇一次都不播** → 玩家结论"BOSS 不会格挡"。
  修法：①常驻只在 `Idle` 挂；②新增**举盾窗口** `bGuardWindowActive`：概率判定成功才 `AddLooseGameplayTag(State_Blocking)`，`RestoreLocoAfterGuard()` 摘掉；`UpdateGuardStance` 在窗口期间**完全 return**（否则刚举盾就被摘）
- 架势消耗只对**玩家按住格挡**生效，避免 BOSS 概率格挡也扣架势 → 常驻破防
- 取证：控制台 `demo.BossGuardTest N` → BOSS 每 1.2s 挨一刀（无头里玩家是木桩，没它走不到格挡判定）。日志搜 `[Boss][格挡判定]`/`★格挡★`
- ★ 屏幕左上角黄色警告（VSM 非 Nanite 阴影溢出，Fab 竞技场是 57×144m 单体非 Nanite 网格）在 `DefaultEngine.ini` 两道保险关掉：
  `[/Script/Engine.Engine] bEnableOnScreenDebugMessages=False` + `[SystemSettings] r.Shadow.Virtual.AllowScreenOverflowMessages=0`；
  **当场**生效用控制台 `DisableAllScreenMessages`。引擎这块包在 `#if !UE_BUILD_SHIPPING` 里 → 正式包本来不显示

## ★★ BOSS 动画：单节点 + DefaultSlot 蒙太奇（架构性定案）
- **渲染规则（读引擎源码）**：`FAnimNode_SingleNode::Evaluate_AnyThread` 只对 **Proxy->CurrentAsset** 求值。CurrentAsset 是序列→只输出该序列；是蒙太奇→走 `SlotEvaluatePose(ActiveMontageSlot)`。`ActiveMontageSlot` 只在 `UAnimSingleNodeInstance::SetAnimationAsset()` 对【蒙太奇资产】注册
  ⇒ **CurrentAsset 还是序列时，`Montage_Play` 播的蒙太奇一个像素都不渲染**（实例在跑、时间在走，没人求值）
- ★★★ **行走/待机必须自己成为 CurrentAsset**：用 `MeshComp->PlayAnimation(Montage, true)`（内部 = `SetAnimationAsset`），**绝不能 `AnimInstance->Montage_Play`**（不碰 CurrentAsset → 地面平移）
  资产 `Boss_Idle_Montage` / `Boss_Walk_{Fwd,Bwd,Left,Right}_Montage`（`Tools/SetupBossWalkMontages.py`，全挂 `DefaultSlot`，骨架必须 = Mesh 的 Skeleton）
- ★★★ **招式播完必须立刻交还底层**：`RestartMontage()` → `Montage_Play(..., bStopAllMontages=**true**)` 会连同行走蒙太奇一起 Stop；那一帧 CurrentAsset 挂着已结束的招式 + 行走已停 → 姿势冻在最后一帧 = 又变平移。
  修法：`bStaleMontageAsset = bCurAssetIsMontage && !bOtherMontagePlayingNow && (CurAsset != DesiredMontage)` → 加入 `bNeedApplyLoco` 立即重挂（修后"已停→重挂"从每 70s 七条降到 **0**）
- **循环链接在实例上不在资产上**：`PlayAnimation` 内部先 `SetMontageLoop(...,false)` 清链接 → 必须在它**之后**调 `SN->SetMontageLoop(Montage,true)` 补回来
- **倍速 = 实际移速 / 该方向动画原生地面速度**。Khaimera 四方向原生速度不同：`Jog_Fwd 354 / Bwd 325 / Strafe_Left 364 / Strafe_Right 370`（`Tools/MeasureWalkSpeedAxisAware.py`，按主轴自适应；固定测 Y 会得出横移 12cm/s 的荒谬值）。已拆 `BossJogReferenceSpeed{,Bwd,Left,Right}` + `ComputeLocoPlayRateForDir()`
  - 基准偏大→脚往前滑；偏小→脚往后追着滑，**两种都像滑步**；用 `Strafe_*` 不要 `Jog_Left/Right`（后者是侧身往前跑）
  - **倍速下限 0.75**；**倍速只给 Jog，Idle 恒 1.0**（待机套速度 = 慢动作）
- **换向必须限频+死区**：`LocoDirSwitchAngleDeg=50`、`LocoDirSwitchCooldown=0.75s`（≥ 一个步态周期 0.667s）；`PlayAnimation` 会清零播放时间，每帧重选 = 步态粉碎。⚠ 死区比较基准必须是**当前正在播的动画对应方向**（`PlayingLocoLocalDir`）
- **方向换算必须用 Mesh 空间**（BP_Boss Mesh 带 Yaw=-90°）：用 `MeshComp->GetComponentTransform()`
- **Mesh 必须 `VisibilityBasedAnimTickOption = AlwaysTickPoseAndRefreshBones`（枚举 0）**：`AlwaysTickPose(1)` 只求值姿势、**不把骨骼变换写回组件** → 未渲染时腿不动、模型整体平移
- **单节点拒绝加法动画**（画面定格）：用 `UAnimationAsset::IsValidAdditive()` 判定，别靠文件名猜。`*_MSA` 基本都是加法；Paragon `Idle_*` 多为加法，用 `Idle_NonAdditive`/`HS_Idle`
- **移动判定唯一可靠依据 = 实测世界坐标差**（`LastLocoSampleLoc`）：`GetVelocity()` 对 `RequestDirectMove` 驱动的 AI 恒为 0；移动意图窗口 0.15s 也不可靠
- **判断"动画在不在动"必须看脚骨 Mesh 空间位移**：`foot_l/foot_r` → `calf_l/calf_r` → `ball_l` → `pelvis`（用 `hand_r` 得假阴性）
- ⚠ 无头 `-NullRHI` 下 `ShouldTickPose=0`、"骨骼位移 0.0cm"是正常的，别当"动画没动"的证据；函数里的 `static float` 日志节流**跨实例共享**，要用实例成员
- **动画类问题一律先跑日志再改代码**（看 `[Boss][anim自检]`，一行给出单节点资产/时间Δ/倍速/脚骨位移）—— **读代码推演猜数值已连错多轮**

## ★ BOSS 移动 / 状态
- **❌ `AddMovementInput` 对 AI 角色无效**；✅ `MoveComp->RequestDirectMove(Dir*MaxWalkSpeed, true)`
- **⚠ 没 NavMesh 时 `MoveToActor` 静默失败** → 用 `bNavPathUnavailable` 记住后一律直驱，别每帧 MoveToActor
  ⚠ **直驱不会绕路** → 遇到 >`MaxStepHeight`（UE 默认 **45cm**）的台阶就顶在前面磨蹭；
  场景竞技场实测卡在 **106cm** 的台阶上_distance 8074 一直不变 → 抬到 `BossMaxStepHeight=110`
  （**必须 BeginPlay 里设**，CDO 存旧组件值会把构造函数的值压回去）
- **`[站位]` 日志是这类问题的唯一取证手段**：`AFightGameMode::Tick` 每 1s 打一行
  双方坐标 + 踩地 + 距离 + 高度差 → 一眼分清"悬空/卡台阶/仇恨范围外"。
  ⚠ 距离 1 万+ 且不变 = 超出 `AggroRange`(1600) 或被地形卡住；高度差能看出"站在另一层台面上"
- **朝向：`MoveComp->bUseControllerDesiredRotation=false`**，朝向只由 Brain 管（否则 ControlRotation 与 `SetActorRotation` 互相拉扯 = 滑步）；Chase 走位分支末尾也要 `TrackTargetDuringCombat(DeltaTime, ChaseTrackYawRate=4.0, false)`
- **★★★ "一顿一顿的地面平移"第一真凶 = 硬直占比过高**：实测 Stagger 占 31% → 移动"走一截停一下"。⚠ **先看状态分布占比，再查移动**（我曾连查几轮"移动指令只有 10 次"，方向全错）。修法 `StaggerCooldown=2.2s` + `LastStaggerTime`，冷却内播受击动画但不定身 → 31%→0%
- 追到交战距离会钉住站到下次出招 → 出招间隔用 `StrafeAroundTarget()` 绕走位提升 Jog 占比
- **移动输入锁必须用引用计数**（`int32 MoveInputLockCount` + `Lock/UnlockMoveInput`）：bool 在多技能重叠时互相误解锁
  - ⚠⚠ 换计数后必查**泄漏**：bool 时代重复 Lock 幂等，计数时代每次 +1。本项目踩过 `LockMovement()` 被 `PlayCurrentCombo()` 每段连招调一次（最多 4 次）→ 锁 4 减 1 = 动不了。现用 `MovementLockHeld` 记累计次数、`RestoreMovement` 一次全减；移速也只压一次（`bSpeedReduced`，否则 `0.3^4` 也动不了）
  - ⚠⚠ 更隐蔽：**加锁的解锁回调被 `EndTask` 杀掉**（锁层数涨到 15）→ `EndTask(ActiveRecover)` **之前**先 `RestoreMovement()`
  - 判据：搜加锁函数名 → 数调用点 → 确认每个都有配对解锁；给可累加状态加带数值的告警日志（`[MoveLock] 锁层数 = N`）
- **倒地/破防冻结**：`UpdateDeathFreeze` 记 `DeathAnchorLocation/Yaw`，每帧 `StopMovementImmediately+DisableMovement+钉回锚点`；
  `UpdateBreakFreeze`（破防）**进入时存 MovementMode → 强制 `MOVE_None`**，起身原样还回去
  （只清速度+钉位置不够：移动组件内部仍认为在下落，下一帧 FindFloor 会拽它 → 原地抖）
  - ⚠ MovementMode **只在进入破防那一帧存**：`LaunchCharacter` 在 MOVE_None 下会改成 MOVE_Falling，每帧都存就永远还原成 Falling
  - 调试 `demo.BossKill 1`（死后每 0.5s 推 120cm）/ **`demo.BossBreak 1`**（打空韧性进破防 + 跪地期间每 0.4s 推 120cm）→ 日志搜 `[Boss][破防冻结]`，判据是 `移动模式=0` 且 `本帧被推离=0.0cm`
  - ⚠ 破防必须走 `UDemoAbilitySystemLibrary::ApplyPoiseDamageOnly`（GE→Execution→AttributeSet 判定链）；**直接 `SetPoise()` 数字会变但永远不会破防**
- BOSS 贴地：`SetPlaneConstraintNormal(UpVector)` + `SetPlaneConstraintOrigin(当前位置)`（`BossAbilities` 里用完必须还原保存值）

## BOSS 招式系统
- **出招是固定背板不是随机**：`PickAttack` 按 `Phase->AttackPattern` 顺序循环取 → 加新招必须同时改 `Attacks` 池【和】`AttackPattern`；背板取招 `bIgnoreCooldown=true`、遍历整轮、游标兜底推进
- 蒙太奇是资产，加新招后必须跑 `Tools/SetupBossKhaimera.py`（否则"这一拍站着不动"）；该脚本 `APPLY_BP_MESH` 必须保持 False
- `EBossAttackType`：`MeleeSlash(0)` / `ChargeSlash(2)` / `Ultimate(3)`；"招式多样化"瓶颈在源动画（`Melee_A/Fast/Slow` 是同一挥击三速度）

## 近战判定无伤害的排查顺序
1. `WeaponStart/WeaponEnd` 插槽是否存在（BOSS 通常两个都缺 → 退化成"手骨+外推"两点 → 挥砍中甩掉贴身玩家）。修法：缺失时沿**手臂骨链**（forearm/lowerarm/upperarm/clavicle）+ 刀身中段布多点（`ADemoCharacterBase::ExtraTracePoints`，append 进 `SamplePoints`）
2. 命中窗口是否被开成 **0 帧**（`SetupMeleeHitWindow` 开/关窗是两个 WaitDelay 任务竞争）→ 强制 `MinWindow=0.12s`
3. 日志 `[Melee] …判定线段 刀柄() 刀尖() 长度=Ncm 半径=R`；⚠ 单点判定必漏（挥砍中手骨离身体距离变化极大）
4. **命中 Pawn 必须 `OverlapMultiByChannel(ECC_Pawn)`**（Sweep 会静默漏检）

## 通用教训
- **多收尾路径的技能，任何一条"让路"都要有兜底**（弹反 `UGA_PlayerParry` 收尾互让 → EndAbility 永不执行 = 玩家失去输入）
- **"配置写了但没人读"是静默 bug**（改配置项后 grep 确认有读取点）。本项目实例：`ABossCharacterBase` 构造里硬编码 `BalanceMaxPoise=320`，tuning 的 `DemoBalance::BossMaxPoise` 从未被读 → 改名 `BalancePoiseCap` 并在构造函数赋 `= DemoBalance::BossMaxPoise`
- **"依赖 AI 寻路却没放 NavMesh"是静默 bug**
