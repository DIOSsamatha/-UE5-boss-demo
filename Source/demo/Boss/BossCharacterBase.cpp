#include "Boss/BossCharacterBase.h"
#include "Boss/BossBrainComponent.h"
#include "Abilities/BossAbilities.h"
#include "Combat/DemoProjectile.h"
#include "Combat/DemoImpactBurst.h"
#include "Core/DemoAbilitySystemLibrary.h"
#include "Core/DemoAttributeSet.h"
#include "Core/DemoCombatTuning.h"
#include "Core/DemoGameplayTags.h"

#include "AbilitySystemComponent.h"
#include "Animation/AnimationAsset.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Animation/AnimTypes.h"
#include "Components/CapsuleComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Sound/SoundBase.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "DrawDebugHelpers.h"
#include "Engine/OverlapResult.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "TimerManager.h"
#include "HAL/IConsoleManager.h"

// 加法动画体检：单节点实例【拒绝加法动画】（引擎只打一行 LogAnimation 警告就完事），
// 画面上表现为定格/怪异。定义在文件后半部分，这里必须【靠前声明】——
// BeginPlay 里加载受击蒙太奇时（很靠前）就要用它做过滤。
static bool IsAdditiveAnim(const UObject* Asset);

// ★ 调试开关（控制台里改，不用重编译）：模拟玩家攻击命中 BOSS。
//   demo.BossGuardTest 6  → 每 1.2 秒模拟一次玩家砍中 BOSS，共 6 次，打完自动归零
//   存在的理由见 BossCharacterBase.h 里 TickGuardSelfTest 的注释：
//   无头实跑时玩家是木桩，BOSS 一下都挨不到，格挡判定永远走不到。
static TAutoConsoleVariable<int32> CVarBossGuardTest(
	TEXT("demo.BossGuardTest"),
	0,
	TEXT("模拟玩家攻击命中 BOSS 的次数（用于验证 BOSS 格挡是否真的会触发）。0 = 关闭；\n")
	TEXT("设为 N（>0）= 之后每 1.2 秒模拟一次玩家普攻命中，共 N 次，打完自动归零。\n")
	TEXT("日志里搜 [Boss][格挡判定] / ★格挡★ 即可看到判定痕迹。"),
	ECVF_Cheat);

// ★ 调试开关：一条命令把 BOSS 打死（走完整条受击致死流程，不是直接调 OnDeathReaction）。
//   demo.BossKill 1
//   存在的理由：验证"倒地之后还会不会移动"需要一条可重复、能自动触发致死路径的手段；
//   无头实跑里 BOSS 血量很厚，等它自然死要好几分钟。
static TAutoConsoleVariable<int32> CVarBossKill(
	TEXT("demo.BossKill"),
	0,
	TEXT("设为 1 = 让 BOSS 立刻吃一发必杀伤害走完致死流程（用于验证倒地后是否还会移动）。\n")
	TEXT("每次进入 BOSS 的 Tick 最多触发一次。日志里搜 [Boss][死亡冻结] 看位移统计。"),
	ECVF_Cheat);

// ★ 调试开关：把 BOSS 韧性打空进破防跪地，并在跪地期间【持续从背后猛推】，
//   专门用来验证"破防倒地期间一动不动"。
//   demo.BossBreak 1
//   为什么需要它：
//     · 无头实跑里 BOSS 韧性上限 720，demo.BossGuardTest 每 1.2s 一刀、每刀只有 15 点
//       韧性、还有约 30% 被格挡 → 要几十刀才等得到破防，根本测不完；
//     · 更关键：光看"没被推动"不能证明锁住了，因为无头场景里根本没有外力。
//       必须【主动注入外力】再测净位移，否则"没动"只是因为"没人推它"。
//   ⚠ 破防必须走 UDemoAbilitySystemLibrary::ApplyPoiseDamageOnly（GE → Execution →
//     AttributeSet 的"韧性清零 → 破防"判定链）。直接 SetPoise() 数字会变但永远不会破防
//     （见 DemoAbilitySystemLibrary.h 里那条注释）。
static TAutoConsoleVariable<int32> CVarBossBreak(
	TEXT("demo.BossBreak"),
	0,
	TEXT("设为 1 = 立刻打空 BOSS 韧性进破防跪地；跪地期间每 0.4s 强行把它挪 120cm + 给移动组件一个直驱请求。\n")
	TEXT("判据：日志 [Boss][破防冻结] 里『移动模式』恒为 0（MOVE_None）、『注入累计』≈『已拉回』。"),
	ECVF_Cheat);

ABossCharacterBase::ABossCharacterBase()
{
	Brain = CreateDefaultSubobject<UBossBrainComponent>(TEXT("Brain"));

	// 与玩家敌对
	TeamId = 1;

	// ---------------- 平衡数值：BOSS 档 ----------------
	// 基类的 Balance* 默认值是"玩家档"，BOSS 在这里整体换成 BOSS 档。
	// 放在构造函数里 = 写进 CDO，BP_Boss 没配过这些属性名（它们是新的）就一定生效。
	// 推导过程（为什么是 10000 血）见 Core/DemoCombatTuning.h 顶部。
	BalanceMaxHealth = 10000.f;   // 全表推导的核心：玩家净 DPS≈40 → 约 4~5 分钟打完

	// ⚠⚠ 韧性上限必须【读平衡表】，不能在这里硬编码（2026-10-04）
	//   原来写的是 `BalanceMaxPoise = 320.f;` 硬编码，于是：
	//     · Core/DemoCombatTuning.h 里的 DemoBalance::BossMaxPoise
	//       （2026-09-30 上调到 520、2026-10-04 再上调到 720）压根没有任何代码去读；
	//     · 两次"调高破防阈值"改的是 tuning 表，实测日志里 BOSS 韧性仍然是 320。
	//   现在让 tuning 成为唯一真源：改表 → 编译 → 生效。
	BalancePoiseCap = DemoBalance::BossMaxPoise;  // 720 → 玩家普攻每段削韧 14，约 51 段破防一次

	BalanceMaxStamina = 999.f;    // BOSS 不做耐力管理，给个大值免得被别的系统扣空
	BalanceAttackPower = 0.f;     // 伤害全写在招式基础值里 → 招式表上的数字就是实伤
	BalanceDefensePower = 0.f;    // 0 = 玩家伤害不打折，方便对着表调
	BalanceMoveSpeed = 380.f;

	// BOSS 是唯一拥有「双异常条」的角色：冲击槽(橙) + 元素槽(青)。
	// 条满 → 失衡(UNSTABLE，绿轮廓) → 元素槽再满 → 击破(BROKEN，失去格挡)。
	bHasAnomalyBars = true;

	// 头顶浮动血条：用户明确要求删掉 BOSS 头顶的血条（改用屏幕顶部的 WBP_BossBar 大血条）。
	// 普通小怪仍可保留各自头顶条；BOSS 这里直接关掉头顶名条 + 名字。
	bShowHealthNameplate = false;
	NameplateOffset = FVector(0.f, 0.f, 165.f);
	// ⚠ 改显示名要走【BeginPlay 强制】而不是只改这里的构造函数默认值：
	//   DisplayName 是 UPROPERTY，BP_Boss 的 CDO 里存着 "Sword Boss"，
	//   会盖掉这里的新默认值（改了 C++ 但游戏里没变，本项目第三次踩这个坑）。
	//   真正生效的赋值在 BeginPlay 里（见 EnsureBossDisplayName）。
	DisplayName = FText::FromString(TEXT("双斧暴徒"));

	// BOSS 体型更大：胶囊加粗加高，武器扫掠范围也相应放大
	if (UCapsuleComponent* Capsule = GetCapsuleComponent())
	{
		Capsule->InitCapsuleSize(60.f, 130.f);
	}
	WeaponTraceRadius = BossWeaponTraceRadius;

	if (UCharacterMovementComponent* MoveComp = GetCharacterMovement())
	{
		// 朝向完全交给 Brain 手动控制（TrackTargetDuringCombat / RotateTowards / 突进技能），
		// 移动组件【不要】自动转朝向：
		//   · bOrientRotationToMovement=false  —— 不朝"移动速度方向"自动转身（会和我们手动拧身体冲突）。
		//   · bUseControllerDesiredRotation=false —— 关键！AI 控制器设了
		//       bSetControlRotationFromPawnOrientation=false 且全工程【从不】调 SetControlRotation，
		//       所以它的 ControlRotation 永远是 Possess 时的旧值。若这里为 true，移动组件每帧物理
		//       都把 BOSS 往那个【过时的朝向】拽，和 Brain 每帧把它拧向玩家的操作互相拉扯 →
		//       BOSS 实际朝向和移动方向对不上，前方 Jog 播着、胶囊却斜着/横着溜 → 画面就是"滑步/走路不自然"。
		//       关掉它，朝向只由 Brain 决定，这个问题立刻消失。
		MoveComp->bOrientRotationToMovement = false;
		MoveComp->bUseControllerDesiredRotation = false;
		MoveComp->RotationRate = FRotator(0.f, 360.f, 0.f);

		// 与玩家的胶囊体互相穿透时"温柔分开"，而不是一帧弹开 32cm。
		// UE 默认 MaxDepenetrationWithPawn = 32cm 且是【瞬间位移】：
		// 一帧 32cm ≈ 1900cm/s，玩家眼里就是"BOSS 被我撞飞 / 我被 BOSS 撞飞"。
		//
		// ⚠ 但这个值也【不能压得太低】—— 原来压到 4cm 就走了另一个极端：
		//   分离速度只有 4cm/帧，玩家持续往 BOSS 身上挤时胶囊根本推不开，
		//   两个模型的网格会一直嵌套着 → 表现就是"贴上去就卡模、叠在一起"。
		//   14 是折中：分离够快（≈840cm/s，几帧就推开，看不出重叠），
		//   又远低于默认 32 那种"一帧弹飞"的暴力感。
		MoveComp->MaxDepenetrationWithPawn = 14.f;
	}

	// 起手预警的点光：常驻但强度为 0 ——
	// 运行时开关"强度 + 可见性"，比在起手瞬间动态创建组件稳（不会有组件创建的一帧卡顿，
	// 也不怕技能被打断时漏掉销毁）。
	TelegraphFlashLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("TelegraphFlashLight"));
	if (TelegraphFlashLight)
	{
		TelegraphFlashLight->SetupAttachment(GetMesh());
		// ⚠⚠⚠ 必须挂在【Mesh】上，不能挂 RootComponent（2026-10-02 第二十一轮修复）
		//
		// 原来挂在胶囊体（RootComponent）上、偏移写死 Z=110：
		//   BOSS 的 Mesh 有【Yaw = -90° 相对旋转】而且每帧还会被 EnsureMeshCapsuleAlignment
		//   沿胶囊 Z 校正过 → Mesh 的实际世界高度和胶囊不是一回事 →
		//   预警红光和模型【错位】，看起来就是"红光和模型不重合"。
		//
		// 挂到 Mesh 上、相对偏移归零 = 光源永远在模型原点（脚下），
		// 再配合 UpdateFlashLightPositions() 每帧按【当前姿势的骨骼包围盒】校正，
		// 保证红光真正罩住躯干（见 UpdateFlashLightPositions 的实现）。
		TelegraphFlashLight->SetRelativeLocation(FVector::ZeroVector);
		TelegraphFlashLight->SetLightColor(FLinearColor(1.f, 0.08f, 0.05f, 1.f));
		TelegraphFlashLight->SetIntensity(0.f);
		TelegraphFlashLight->SetAttenuationRadius(700.f);
		// 只是染色，不投影：起手只有 0.5~1.0s，开阴影既看不到又白花性能
		TelegraphFlashLight->SetCastShadows(false);
		TelegraphFlashLight->SetVisibility(false);
	}

	// 护体光：常驻、强度 0，只在举盾时点亮。
	// 冷蓝 = 防御，和起手预警的红、受击的白、失衡的绿严格区分开 ——
	// 四种颜色对应四种状态，玩家扫一眼 BOSS 身上的光就知道它现在是什么状态，不用读 UI。
	GuardLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("GuardLight"));
	if (GuardLight)
	{
		GuardLight->SetupAttachment(GetRootComponent());
		GuardLight->SetRelativeLocation(FVector(0.f, 0.f, 95.f));
		GuardLight->SetLightColor(FLinearColor(0.45f, 0.72f, 1.f, 1.f));
		GuardLight->SetIntensity(0.f);
		GuardLight->SetAttenuationRadius(GuardLightRadius);
		GuardLight->SetCastShadows(false);
		GuardLight->SetVisibility(false);
	}

	// 受击闪白的点光：常驻、强度 0，运行时开关。独立于预警红光（不同颜色、不同槽位）。
	// 作用：BOSS 在霸体 / 出招期间被命中时不会僵直，但每次都会"啪"地闪一下白光，
	// 给玩家明确的"这一刀砍进去了"的视觉确认 —— 这是"砍到 BOSS 的感觉"缺失的最大补丁。
	HitFlashLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("HitFlashLight"));
	if (HitFlashLight)
	{
		// ⚠ 同 TelegraphFlashLight：挂 Mesh 而非胶囊，位置每帧按骨骼包围盒校正（见 UpdateFlashLightPositions）
		HitFlashLight->SetupAttachment(GetMesh());
		HitFlashLight->SetRelativeLocation(FVector::ZeroVector);
		HitFlashLight->SetLightColor(FLinearColor(0.9f, 0.95f, 1.0f, 1.f)); // 偏冷的白
		HitFlashLight->SetIntensity(0.f);
		HitFlashLight->SetAttenuationRadius(HitFlashLightRadius);
		HitFlashLight->SetCastShadows(false);
		HitFlashLight->SetVisibility(false);
	}
}

// ------------------------------------------------------------------ 显示名
//
// DisplayName 是 UPROPERTY，BP_Boss 的 CDO 里存着 "Sword Boss"，
// 会盖掉构造函数里的新默认值（改了 C++ 但游戏里没变，本项目第三次踩这个坑）。
// 所以真正生效的赋值放在这里（BeginPlay 里），而不是只改构造函数的默认值。
// 想让美术/策划在蓝图里自由改名字的话，把下面这行删掉即可。
void ABossCharacterBase::EnsureBossDisplayName()
{
	// 蓝图里配了别的名字就尊重蓝图；只有"还是默认值"或"空"时才强制用 C++ 的新名字。
	const FString Current = DisplayName.IsEmpty() ? FString() : DisplayName.ToString();
	if (Current.IsEmpty() || Current == TEXT("Sword Boss") || Current == TEXT("BOSS"))
	{
		DisplayName = FText::FromString(TEXT("双斧暴徒"));
	}
	UE_LOG(LogDemoCombat, Warning, TEXT("[Boss] 显示名 = 「%s」"), *DisplayName.ToString());
}

void ABossCharacterBase::BeginPlay()
{
	// 蓝图里完全没配属性时给一套可用默认值。
	// 必须在 Super::BeginPlay() 之前 —— 基类会用 MoveSpeed 去设 MaxWalkSpeed。
	EnsureDefaultAttributes();

	Super::BeginPlay();

	// ⚠ 位移采样基准点必须先对齐（2026-10-03）
	// LastLocoSampleLoc 的默认值是 ZeroVector，而 BOSS 出生点在 (-520,-1110) 附近 →
	// 第一帧算出来的"本帧位移"其实是整段出生坐标：实测 1225.8cm / 折算 51482cm/s，
	// 于是第一帧的播放倍速被顶到上限（2.50），蒙太奇从一个莫名其妙的位置起播。
	// 这里在第一次采样前先把基准钉到当前坐标，后面每一帧的差值才是真正的"这一帧走了多远"。
	LastLocoSampleLoc = GetActorLocation();
	bLocoSampleAdvanced = false;
	MoveDeltaThisFrame = FVector::ZeroVector;

	// 基础动画：强制单节点模式 + 待机循环，彻底绕开坏掉的 Khaimera_AnimBlueprint
	SetupBossAnimation();

	// 显示名（"Sword Boss" → "双斧暴徒"）。必须在 BeginPlay 赋值才能盖过蓝图 CDO。
	EnsureBossDisplayName();

	// ★ 2026-10-06：抬高"能迈上多高的台阶"。
	//   实测（FightArena，无头日志 [站位]）：BOSS 从 Y=-4706 一路走到 Y≈-3072 就卡住不动，
	//   而玩家站在 Y=+5000、高 195cm 的另一层台面上 —— 中间那道台阶超过 UE 默认的
	//   MaxStepHeight=45cm，胶囊体爬不上去；Brain 又是"无导航直驱"（不会绕路），
	//   于是 BOSS 顶在台阶前磨蹭，玩家永远等不到它过来。
	//   110cm 的理由：竞技场里有台阶/缘石/矮台，只有 45cm 时 BOSS 会被各种装饰卡住；
	//   又不能给太大，否则能爬墙。可在 BP 的 Demo|Movement 里按场景调。
	//   ⚠ 必须写在 BeginPlay：BP_Boss 的 CDO 里存着旧的组件值，构造函数里设会被压回去。
	if (UCharacterMovementComponent* MoveComp = GetCharacterMovement())
	{
		MoveComp->MaxStepHeight = FMath::Max(BossMaxStepHeight, 45.f);
		UE_LOG(LogDemoCombat, Log, TEXT("[Boss] MaxStepHeight = %.0fcm（默认 45）"),
			MoveComp->MaxStepHeight);
	}


	// ⚠⚠⚠ 行走蒙太奇（2026-10-02 第十六轮）：序列 → 蒙太奇，修复"移动时没有动画"
	// 必须在 SetupBossAnimation 之后加载：它要靠 SetupBossAnimation 设好的单节点模式。
	// 这几个蒙太奇由 Tools/SetupBossWalkMontages.py 从 Khaimera 的 Jog 序列生成。
	ResolveMontage(BossIdleMontageAsset,   TEXT("/Game/Boss/Animation/Boss_Idle_Montage.Boss_Idle_Montage"));
	ResolveMontage(BossWalkFwdMontage,    TEXT("/Game/Boss/Animation/Boss_Walk_Fwd_Montage.Boss_Walk_Fwd_Montage"));
	ResolveMontage(BossWalkBwdMontage,    TEXT("/Game/Boss/Animation/Boss_Walk_Bwd_Montage.Boss_Walk_Bwd_Montage"));
	ResolveMontage(BossWalkLeftMontage,   TEXT("/Game/Boss/Animation/Boss_Walk_Left_Montage.Boss_Walk_Left_Montage"));
	ResolveMontage(BossWalkRightMontage,  TEXT("/Game/Boss/Animation/Boss_Walk_Right_Montage.Boss_Walk_Right_Montage"));
	{
		// ⚠ 骨架必须是 Khaimera：蒙太奇的 target_skeleton 与 Mesh 不一致时 Montage_Play 会静默失败。
		const USkeletalMeshComponent* MeshForCheck = GetMesh();
		const USkeleton* MeshSkel = MeshForCheck ? MeshForCheck->GetSkeletalMeshAsset()
			? MeshForCheck->GetSkeletalMeshAsset()->GetSkeleton() : nullptr : nullptr;
		int nOk = 0;
		auto Chk = [&](const UAnimMontage* M, const TCHAR* Label) {
			if (!M) { return; }
			++nOk;
			USkeleton* MS = M->GetSkeleton();
			if (MeshSkel && MS && MS != MeshSkel)
			{
				UE_LOG(LogDemoCombat, Warning,
					TEXT("[Boss][动画] ⚠ 行走蒙太奇【%s】骨架与 Mesh 不一致（%s vs %s）→ 播不出来，必须用 Tools/SetupBossWalkMontages.py 重新生成"),
					Label, *MS->GetName(), *MeshSkel->GetName());
			}

			// ⚠⚠ 槽位 + 段落体检（2026-10-03）
			//
			// 槽位：单节点只渲染 CurrentAsset 那个蒙太奇的 SlotAnimTracks[0].SlotName
			//   对应的那一条槽。行走/攻击/受击必须【同名】才能互相分层，
			//   本项目统一用 DefaultSlot。槽名不对 = 走路时腿不动 / 出招看不见，
			//   而且不会有任何报错（引擎只是"求值出来没人用"），所以必须开局点出来。
			//
			// 段落：Montage 能循环的前提是有 CompositeSection（SetMontageLoop 靠它把
			//   末段接回首段）。没有段落 → 走一遍就停在最后一帧 → 又变成"脚下平移"。
			const FName SlotName = M->SlotAnimTracks.Num() > 0
				? M->SlotAnimTracks[0].SlotName : NAME_None;
			if (SlotName != FName(TEXT("DefaultSlot")))
			{
				UE_LOG(LogDemoCombat, Error,
					TEXT("[Boss][动画] ⚠ 行走蒙太奇【%s】的首个 Slot 是「%s」而不是 DefaultSlot → 单节点下它永远不会被渲染（表现 = 腿不动、整体平移）"),
					Label, *SlotName.ToString());
			}
			if (M->CompositeSections.Num() == 0)
			{
				UE_LOG(LogDemoCombat, Warning,
					TEXT("[Boss][动画] 行走蒙太奇【%s】没有 CompositeSection → 无法自动循环（已由 ApplyLocomotionForFrame 的重挂逻辑兜底）"),
					Label);
			}
		};
		Chk(BossIdleMontageAsset, TEXT("Idle"));
		Chk(BossWalkFwdMontage, TEXT("WalkFwd"));
		Chk(BossWalkBwdMontage, TEXT("WalkBwd"));
		Chk(BossWalkLeftMontage, TEXT("WalkLeft"));
		Chk(BossWalkRightMontage, TEXT("WalkRight"));
		UE_LOG(LogDemoCombat, Warning, TEXT("[Boss][动画] 行走蒙太奇就绪 %d/5 个（缺失的会自动回退到序列方案）"), nOk);

		// 行走蒙太奇通道是否可用：至少"待机 + 前冲"都在，才能保证任何时刻都有腿在动。
		bUseMontageLocomotion = (BossIdleMontageAsset != nullptr) && (BossWalkFwdMontage != nullptr);
		UE_LOG(LogDemoCombat, Warning,
			TEXT("[Boss][动画] 行走通道 = %s（蒙太奇 / 序列回退）"),
			bUseMontageLocomotion ? TEXT("★蒙太奇★") : TEXT("序列(蒙太奇缺失)"));
	}

	// 破防时播虚弱表现（韧性清零由 AttributeSet 广播，这里接表现）
	OnPoiseBroken.AddDynamic(this, &ABossCharacterBase::HandleBossPoiseBroken);

	// 表现用蒙太奇：蓝图槽位为空时按约定路径兜底加载（脚本生成的位置）
	ResolveMontage(HitReactMontage, TEXT("/Game/Boss/Animation/Boss_Hit_F_Montage.Boss_Hit_F_Montage"));
	ResolveMontage(WeakMontage, TEXT("/Game/Boss/Animation/Boss_Weak_In_Montage.Boss_Weak_In_Montage"));
	ResolveMontage(WeakLoopMontage, TEXT("/Game/Boss/Animation/Boss_Weak_Loop_Montage.Boss_Weak_Loop_Montage"));
	ResolveMontage(WeakRecoverMontage, TEXT("/Game/Boss/Animation/Boss_Weak_Recover_Montage.Boss_Weak_Recover_Montage"));
	ResolveMontage(StaggerMontage, TEXT("/Game/Boss/Animation/Boss_Stagger_Montage.Boss_Stagger_Montage"));
	ResolveMontage(DeathMontage, TEXT("/Game/Boss/Animation/Boss_Death_Montage.Boss_Death_Montage"));
	ResolveMontage(GuardMontage, TEXT("/Game/Boss/Animation/Boss_Guard_Idle_Montage.Boss_Guard_Idle_Montage"));

	// Mesh ↔ 胶囊体对齐：先立即校正一次，再定期复查。
	// 为什么不每帧做：包围盒查询不算便宜，而"网格偏移错位"是配置/动画层面的问题，
	// 1s 一次足够兜住；同时避免和 Tick 里的位移安全阀抢 Z。
	EnsureMeshCapsuleAlignment();
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(
			MeshAlignTimerHandle, this, &ABossCharacterBase::EnsureMeshCapsuleAlignment,
			FMath::Max(MeshAlignCheckInterval, 0.2f), /*bLoop=*/true);
	}

	// 四向受击：一张表把方向映射到蒙太奇。没配方向表也能跑（退回 HitReactMontage）。
	if (HitReactMontages.Num() == 0)
	{
		struct FHitDirMap { EDemoHitDirection Dir; const TCHAR* Path; };
		const FHitDirMap Map[] =
		{
			{ EDemoHitDirection::Front, TEXT("/Game/Boss/Animation/Boss_Hit_F_Montage.Boss_Hit_F_Montage") },
			{ EDemoHitDirection::Back,  TEXT("/Game/Boss/Animation/Boss_Hit_B_Montage.Boss_Hit_B_Montage") },
			{ EDemoHitDirection::Left,  TEXT("/Game/Boss/Animation/Boss_Hit_L_Montage.Boss_Hit_L_Montage") },
			{ EDemoHitDirection::Right, TEXT("/Game/Boss/Animation/Boss_Hit_R_Montage.Boss_Hit_R_Montage") },
		};
		for (const FHitDirMap& Entry : Map)
		{
			TObjectPtr<UAnimMontage> Loaded = Cast<UAnimMontage>(StaticLoadObject(UAnimMontage::StaticClass(), nullptr, Entry.Path));
			// ⚠⚠ 加法动画直接丢弃（2026-10-02）
			// 单节点实例【拒绝加法动画】：引擎只打一行 LogAnimation 警告就完事，
			// 画面上表现为"打中 BOSS 时姿势定格/怪异"（用户截图里明确出现）。
			// Paragon Khaimera 包里大量 Idle_*/Hit_* 是加法动画，必须体检后过滤。
			if (Loaded && IsAdditiveAnim(Loaded))
			{
				UE_LOG(LogDemoCombat, Warning,
					TEXT("[Boss][动画体检] 受击蒙太奇 %s 是【加法动画】→ 单节点拒绝播放，打中时会定格。已丢弃该方向。"),
					*Loaded->GetName());
				Loaded = nullptr;
			}
			if (Loaded)
			{
				HitReactMontages.Add(Entry.Dir, Loaded);
			}
		}
	}

	// 起手红光的覆盖材质：蓝图槽位为空时按约定路径兜底加载。
	// 取不到就留空 —— 红光还有"点光 + 胸口爆闪"两层兜着，不会整个失效。
	if (!TelegraphFlashMaterial)
	{
		TelegraphFlashMaterial = Cast<UMaterialInterface>(StaticLoadObject(
			UMaterialInterface::StaticClass(), nullptr,
			TEXT("/Game/Boss/VFX/MI_BossFlash.MI_BossFlash")));
	}
	if (TelegraphFlashMaterial)
	{
		// 用 MID 才能在运行时改 Intensity / Flash 这两个标量参数
		TelegraphFlashMID = UMaterialInstanceDynamic::Create(TelegraphFlashMaterial, this);
	}

	// 举盾冷蓝轮廓：同样按约定路径兜底（Tools/SetupBossVFX.py 生成 MI_BossGuard）。
	// 拿不到就只有护体光 + 被挡火花，仍然看得出"它在防御"。
	if (!GuardOverlayMaterial)
	{
		GuardOverlayMaterial = Cast<UMaterialInterface>(StaticLoadObject(
			UMaterialInterface::StaticClass(), nullptr,
			TEXT("/Game/Boss/VFX/MI_BossGuard.MI_BossGuard")));
	}
	if (GuardOverlayMaterial)
	{
		GuardMID = UMaterialInstanceDynamic::Create(GuardOverlayMaterial, this);
	}

	// 格挡音效：缺资源就静音（不 warn 刷屏，视觉通道已经够读）
	if (!BlockSound)
	{
		BlockSound = Cast<USoundBase>(StaticLoadObject(
			USoundBase::StaticClass(), nullptr,
			TEXT("/Game/Boss/SFX/SC_BossBlock.SC_BossBlock")));
	}

	// 失衡绿轮廓的覆盖材质：同样按约定路径兜底（Tools/SetupBossVFX.py 生成）。
	// 拿不到就只留 HUD 标签那一条通道，不挂材质 —— 宁可少一层表现，也不要把 BOSS 涂成黑块。
	if (!UnstableOverlayMaterial)
	{
		UnstableOverlayMaterial = Cast<UMaterialInterface>(StaticLoadObject(
			UMaterialInterface::StaticClass(), nullptr,
			TEXT("/Game/Boss/VFX/MI_BossUnstable.MI_BossUnstable")));
	}
	if (UnstableOverlayMaterial)
	{
		UnstableMID = UMaterialInstanceDynamic::Create(UnstableOverlayMaterial, this);
	}
	else
	{
		UE_LOG(LogDemoCombat, Warning,
			TEXT("[Boss] 未找到 MI_BossUnstable —— 失衡只有 HUD 标签一层表现。跑一次 Tools/SetupBossVFX.py 即可生成。"));
	}

	// 保障 4 类招式技能都已授予（BP_Boss 的 DefaultAbilities 漏配也能打）
	EnsureBossAbilitiesGranted();

	// 根位移模式自愈：BOSS 用的是和玩家同一个 ABP，一旦它被设成 NoRootMotionExtraction，
	// 出招位移就会"留在 Mesh 上 + 播完弹回"，胶囊体反而不动（相机也就不跟）。
	EnsureRootMotionExtraction(TEXT("BeginPlay"));

	UE_LOG(LogDemoCombat, Log,
		TEXT("[Boss] %s 就绪：血量=%.0f/%.0f 韧性=%.0f 胶囊半径=%.0f 武器扫掠=%.0f 受击蒙太奇=%d 向"),
		*GetName(), GetHealth(), GetMaxHealth(), GetMaxPoise(),
		GetCapsuleComponent() ? GetCapsuleComponent()->GetScaledCapsuleRadius() : 0.f,
		WeaponTraceRadius, HitReactMontages.Num());

	// ---------------- 招式动画体检 ----------------
	// 逐招检查"这一拍到底有没有动作动画"，把结果一次性列成清单。
	// ⚠ 为什么必须有它：之前只体检了举盾/受击/虚弱那几个表现蒙太奇，
	//   【攻击类招式一个都没查】——于是"某招其实没动画"这种问题只能靠肉眼在游戏里发现。
	//   查两件事：
	//     ① 蒙太奇资产在不在（不在 = 该招退化成空动作，站着不动）
	//     ② 是不是加法动画（是 = 单节点直接拒绝播放，画面定格/怪异）
	AuditAttackMontages();
}

// 加法动画体检函数在文件后半部分定义（IsAdditiveAnim），文件前半部分的多个函数都要用它，
// 所以在这里（靠前）先声明一次。
static bool IsAdditiveAnim(const UObject* Asset);

void ABossCharacterBase::AuditAttackMontages()
{
	UBossBrainComponent* BrainComp = GetBrain();
	if (!BrainComp)
	{
		return;
	}

	TSet<FName> Seen;
	int32 Total = 0, Missing = 0, AdditiveCount = 0;

	for (const UBossPhaseData* Phase : BrainComp->Phases)
	{
		if (!Phase)
		{
			continue;
		}
		for (const UBossAttackData* Attack : Phase->Attacks)
		{
			if (!Attack || Seen.Contains(Attack->AttackName))
			{
				continue;
			}
			Seen.Add(Attack->AttackName);
			++Total;

			if (!Attack->Montage)
			{
				++Missing;
				UE_LOG(LogDemoCombat, Error,
					TEXT("[Boss][招式体检] 「%s」【没有蒙太奇资产】→ 这一拍会是空动作（BOSS 站着不动）。")
					TEXT("检查 /Game/Boss/Animation/ 下有没有对应蒙太奇，或跑一次 Tools/SetupBossKhaimera.py"),
					*Attack->AttackName.ToString());
				continue;
			}

			if (IsAdditiveAnim(Attack->Montage))
			{
				++AdditiveCount;
				UE_LOG(LogDemoCombat, Error,
					TEXT("[Boss][招式体检] 「%s」的蒙太奇 %s 是【加法动画】→ 单节点拒绝播放，画面会定格/怪异。")
					TEXT("必须换成非加法素材重做该蒙太奇"),
					*Attack->AttackName.ToString(), *Attack->Montage->GetName());
			}
		}
	}

	UE_LOG(LogDemoCombat, Log,
		TEXT("[Boss][招式体检] 共 %d 招：有动画 %d，缺资产 %d，加法动画(播不出) %d"),
		Total, Total - Missing - AdditiveCount, Missing, AdditiveCount);
}

void ABossCharacterBase::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// 死亡冻结必须排在【所有】表现/位移之前：一旦 BOSS 倒下，
	// 后面那些"转向 / 位移闸 / 贴地"每帧都得先被钉死，否则尸体还会滑。
	UpdateDeathFreeze(DeltaTime);

	// 破防跪地同理（但它会恢复，所以只锁位置不清 MovementMode）
	UpdateBreakFreeze(DeltaTime);

	// 平滑转向放在最前面，且早于下面那个 ASC 空指针的提前 return ——
	// 它是纯表现，不该因为 GAS 没就绪就停掉
	UpdateSmoothFacing(DeltaTime);

	// 调试钩子：模拟玩家攻击命中（demo.BossGuardTest N）。默认 0 = 什么都不做。
	TickGuardSelfTest(DeltaTime);

	// 调试钩子：demo.BossKill 1 —— 让 BOSS 走完整条受击致死流程，好验证"倒地后不再移动"。
	if (!bKillTestFired && CVarBossKill.GetValueOnGameThread() != 0 && !IsDead())
	{
		bKillTestFired = true;
		AActor* Killer = GetCombatTarget();
		if (!Killer)
		{
			Killer = UGameplayStatics::GetPlayerPawn(this, 0);
		}
		UE_LOG(LogDemoCombat, Warning, TEXT("[Boss][死亡冻结] 调试击杀触发（来源=%s）"),
			Killer ? *Killer->GetName() : TEXT("无"));
		if (Killer && Killer != this)
		{
			// 走 ApplyHit 而不是直接调 OnDeathReaction：这样"死亡"是与实战完全一样的路径，
			// 测出来的才是玩家真正会看到的行为。
			UDemoAbilitySystemLibrary::ApplyHit(
				Killer, this,
				/*BaseDamage=*/999999.f,
				/*PoiseDamage=*/0.f,
				/*bCanBeParried=*/false,
				/*bCanBeBlocked=*/false,
				Killer->GetActorLocation(),
				/*bTriggerHitStop=*/false);
		}
	}

	// 调试钩子：demo.BossBreak 1 —— 走真实 GE 管线把韧性打空进破防跪地，
	// 随后在跪地期间每 0.4s 从背后推一次（LaunchCharacter 380cm/s，去掉摩擦大约走 100cm+）。
	// 这是"倒地不能移动"的**加压测试**：没有外力时"没动"说明不了问题，
	// 必须先证明推力真的作用在 BOSS 身上，再看它的净位移是不是 0。
	if (CVarBossBreak.GetValueOnGameThread() != 0 && !IsDead())
	{
		if (!bBreakTestFired)
		{
			bBreakTestFired = true;
			AActor* Killer = GetCombatTarget();
			if (!Killer)
			{
				Killer = UGameplayStatics::GetPlayerPawn(this, 0);
			}
			if (Killer && Killer != this)
			{
				// 先摘常驻格挡标签：还举着盾时这一刀会被格挡掉，韧性根本不掉
				if (UAbilitySystemComponent* ASC = GetAbilitySystemComponent())
				{
					ASC->RemoveLooseGameplayTag(DemoTag::State_Blocking);
				}
				const float PoiseLeft = GetPoise();
				UDemoAbilitySystemLibrary::ApplyPoiseDamageOnly(Killer, this, PoiseLeft + 50.f);
				UE_LOG(LogDemoCombat, Warning,
					TEXT("[Boss][破防冻结] 调试破防触发：剩余韧性 %.0f → 施加 %.0f 点（来源=%s）"),
					PoiseLeft, PoiseLeft + 50.f, *Killer->GetName());
			}
			else
			{
				UE_LOG(LogDemoCombat, Warning, TEXT("[Boss][破防冻结] 调试破防找不到玩家，跳过"));
			}
		}
		else if (IsPoiseBroken())
		{
			BreakTestPushTimer += DeltaTime;
			if (BreakTestPushTimer >= 0.4f)
			{
				BreakTestPushTimer = 0.f;
				++BreakTestPushCount;

				FVector PushDir = FVector::ZeroVector;
				if (const AActor* Target = GetCombatTarget())
				{
					PushDir = GetActorLocation() - Target->GetActorLocation();
					PushDir.Z = 0.f;
					PushDir = PushDir.GetSafeNormal();
				}
				if (PushDir.IsNearlyZero())
				{
					PushDir = FVector(0.8f, 0.6f, 0.f).GetSafeNormal();
				}

				// 两种外力一起上，覆盖真实的两种"被推"来源：
				//   ① AddActorWorldOffset = 别的动作/根位移直接把它挪走（弹反击退、突进收尾）
				//   ② RequestDirectMove   = 移动组件自己的直驱请求（大脑残留、击退速度）
				// 和死亡压力测试（UpdateDeathFreeze 里那段）用同一套写法。
				AddActorWorldOffset(PushDir * 120.f, /*bSweep=*/false, nullptr, ETeleportType::None);
				if (UCharacterMovementComponent* MoveComp = GetCharacterMovement())
				{
					MoveComp->RequestDirectMove(PushDir * 400.f, /*bForceMaxSpeed=*/true);
				}
				BreakTestInjectedCm += 120.f;
			}
		}
	}

	// 底层待机 / 行走循环：按移动速度在单节点下切换（攻击/受击蒙太奇盖在上面，这里不打扰）
	UpdateBossLocomotion(DeltaTime);

	// ⚠⚠ 每帧把预警红光 / 受击白光的灯位【对准模型真正的躯干中心】（2026-10-02 第二十一轮）
	// 之前灯是挂在 Mesh 组件原点、位置从不更新的，而 Mesh 原点在【脚底】——
	// 于是红光只照亮 BOSS 的脚，画面上就是"红光和模型错位"。
	// UpdateFlashLightPositions 这个函数此前只写在注释里、根本没实现。
	UpdateFlashLightPositions(DeltaTime);

	// 让 BOSS 在移动时忽略玩家这个 Actor：根治"玩家撞上去把 BOSS 推着走 / 打飞"。
	// UE 的角色移动组件即使胶囊设成 Overlap，每帧仍会做"去穿插"把重叠双方互相顶开；
	// 只有显式 IgnoreActorWhenMoving 才能彻底让 BOSS 不被玩家的碰撞位移影响。
	// 玩家自己仍被 BOSS 挡住（玩家不忽略 BOSS），不会穿模。
	// ⚠ 命中检测走的是 OverlapMultiByChannel(ECC_Pawn) 独立查询，和这个 Ignore 无关，砍 BOSS 照常。
	UpdatePlayerPushImmunity();

	// ⚠⚠⚠ 预警红光 / 受击闪白的灯位与半径，统一由 UpdateFlashLightPositions() 一处负责
	//（上面刚调过）。这里【曾经】还并行存在第二套匿名 lambda 同步逻辑：
	//   它用 Mesh->Bounds.Origin 当中心、并把半径 clamp 到 [TelegraphFlashRadius(700), ×3]，
	//   也就是最少 7 米的红光 —— 于是 BOSS 周围一圈地面全被染红，
	//   画面上就是"红光不贴合模型、像一大坨光罩在人物外面"。
	//   两套逻辑还会互相覆盖：谁在后面谁生效，行为完全取决于代码顺序，非常脆弱。
	// 现在只保留 UpdateFlashLightPositions 一处（半径按躯干尺寸贴身算）。

	// 网格贴地：把 Mesh 拉到底面（每帧、限速、用真实骨骼最低点）
	UpdateMeshGroundAlignment();

	// 被动反应（受击 / 虚弱 / 硬直）蒙太奇的根位移闸：这些动作不许推着 BOSS 走
	UpdateReactionRootMotionGuard();

	// 位移安全阀：不管推力从哪来，BOSS 一帧位移不许超过自己的合法速度
	UpdateDisplacementGuard(DeltaTime);

	// 起手预警红光：同样是纯表现
	UpdateTelegraphFlash(DeltaTime);

	// 失衡绿轮廓：异常条打满期间的持续表现（状态信号规范的第二条通道）
	UpdateUnstableVisual(DeltaTime);

	// 动画自检（节流到 0.5s 一条）
	DebugDumpAnimState(DeltaTime);

	// 默认格挡：按大脑状态维护举盾（中立才挡，出招/后摇/硬直/击破不挡 = 玩家的输出窗口）
	UpdateGuardStance();

	// 举盾表现：冷蓝轮廓 + 护体光（举盾是"状态"，必须用持续表现而不是瞬间特效）
	UpdateGuardVisual(DeltaTime);

	// 受击闪白：每次被命中后的一帧级白光脉冲，逐帧衰减到 0 熄灭
	UpdateHitFlash(DeltaTime);

	UAbilitySystemComponent* ASC = GetAbilitySystemComponent();
	if (!ASC)
	{
		return;
	}

	const bool bBrokenNow = ASC->HasMatchingGameplayTag(DemoTag::State_Broken);
	const bool bDeadNow = ASC->HasMatchingGameplayTag(DemoTag::State_Dead);

	// ---------------- 破防（虚弱）表现 ----------------
	// 进入：播虚弱蒙太奇。持续：蒙太奇被别的东西顶掉时补播循环段。
	// 退出：播恢复动作。
	if (bBrokenNow && !bWasBroken)
	{
		bWasBroken = true;
		PlayReactionMontage(WeakMontage ? WeakMontage : StaggerMontage);
	}
	else if (!bBrokenNow && bWasBroken)
	{
		bWasBroken = false;
		PlayReactionMontage(WeakRecoverMontage);
	}
	else if (bBrokenNow && WeakLoopMontage)
	{
		UAnimInstance* AnimInstance = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr;
		if (AnimInstance && !AnimInstance->IsAnyMontagePlaying())
		{
			// 虚弱循环：被打断 / 播完就补上，让 BOSS 一直保持"站不稳"的姿态
			PlayReactionMontage(WeakLoopMontage);
		}
	}

	if (bDeadNow && !bWasDead)
	{
		bWasDead = true;
	}

	// ---------------- 调试绘制 ----------------
	if (bDrawDebugCombat)
	{
		if (AActor* Target = GetCombatTarget())
		{
			DrawDebugLine(GetWorld(), GetActorLocation(), Target->GetActorLocation(), FColor::Yellow, false, -1.f, 0, 1.5f);
		}

		const UCharacterMovementComponent* MoveComp = GetCharacterMovement();
		if (MoveComp && MoveComp->IsFalling())
		{
			// 空中：提示"人离地了"
			DrawDebugSphere(GetWorld(), GetActorLocation(), 60.f, 12, FColor::Orange, false, -1.f, 0, 1.f);
		}

		DrawDebugString(GetWorld(), GetActorLocation() + FVector(0.f, 0.f, 140.f),
			HasSuperArmor() ? TEXT("BOSS [霸体]") : TEXT("BOSS"), nullptr, FColor::Red, 0.f, true);
	}
}

// ------------------------------------------------------------------ 平滑转向
void ABossCharacterBase::SetFacingTarget(const FVector& WorldLocation, float TurnSpeedDegPerSec, float MaxTurnTime)
{
	FacingTargetLocation = WorldLocation;
	FacingTurnSpeed = FMath::Max(TurnSpeedDegPerSec, 1.f);
	FacingTimeRemaining = FMath::Max(MaxTurnTime, 0.f);
	bHasFacingTarget = true;
}

void ABossCharacterBase::ClearFacingTarget()
{
	bHasFacingTarget = false;
	FacingTimeRemaining = 0.f;
}

void ABossCharacterBase::UpdateSmoothFacing(float DeltaTime)
{
	// 倒地之后不再转身：尸体被"修正朝向"看着最假（UpdateDeathFreeze 也钉了朝向，这里是双保险）
	if (IsDead())
	{
		return;
	}

	// 破防跪地也不转身：跪着转圈比不转更怪
	if (IsPoiseBroken())
	{
		return;
	}

	if (!bHasFacingTarget)
	{
		return;
	}

	FacingTimeRemaining -= DeltaTime;

	FVector Dir = FacingTargetLocation - GetActorLocation();
	Dir.Z = 0.f;

	if (Dir.IsNearlyZero())
	{
		ClearFacingTarget();
		return;
	}

	const FRotator Current = GetActorRotation();
	const FRotator Desired = FRotationMatrix::MakeFromX(Dir.GetSafeNormal()).Rotator();

	// RInterpConstantTo = 恒定角速度插值：
	// 用 RInterpTo（指数衰减）会在快到位时明显减速，看起来像"犹豫了一下"；
	// 招式起手的转向要干脆，所以选恒速。
	const FRotator Next = FMath::RInterpConstantTo(Current, Desired, DeltaTime, FacingTurnSpeed);
	SetActorRotation(FRotator(Current.Pitch, Next.Yaw, Current.Roll));

	// 到位（或超时）就收工：超时是为了防止目标在高速绕圈时 BOSS 无限追着转
	const bool bAligned = FMath::Abs(FMath::FindDeltaAngleDegrees(Current.Yaw, Desired.Yaw)) < 1.5f;
	if (bAligned || FacingTimeRemaining <= 0.f)
	{
		ClearFacingTarget();
	}
}

// ------------------------------------------------------------------ 技能分发
TSubclassOf<UGameplayAbility> ABossCharacterBase::GetAbilityClassForAttackType(EBossAttackType AttackType) const
{
	// 1) 蓝图里配的槽位优先（想换实现 / 挂蓝图默认值就走这里）
	if (const TSubclassOf<UGameplayAbility>* Found = AttackAbilityClasses.Find(AttackType))
	{
		if (*Found)
		{
			return *Found;
		}
	}

	// 2) 约定路径的蓝图子类（Tools/SetupBossMontages.py 会建这 4 个 BP 并挂到 BP_Boss 上）
	//    ⚠ 用 StaticLoadClass 而不是 LoadClass<T>()：后者在 5.7 的模板解析有歧义
	const TCHAR* BlueprintPath = nullptr;
	switch (AttackType)
	{
	case EBossAttackType::MeleeSlash:		BlueprintPath = TEXT("/Game/Boss/Abilities/GA_BossAttack.GA_BossAttack_C"); break;
	case EBossAttackType::RangedBarrage:	BlueprintPath = TEXT("/Game/Boss/Abilities/GA_BossRangedAttack.GA_BossRangedAttack_C"); break;
	case EBossAttackType::ChargeSlash:		BlueprintPath = TEXT("/Game/Boss/Abilities/GA_BossChargeSlash.GA_BossChargeSlash_C"); break;
	case EBossAttackType::Ultimate:			BlueprintPath = TEXT("/Game/Boss/Abilities/GA_BossUltimate.GA_BossUltimate_C"); break;
	default: break;
	}

	if (BlueprintPath)
	{
		if (UClass* Loaded = StaticLoadClass(UGameplayAbility::StaticClass(), nullptr, BlueprintPath))
		{
			return Loaded;
		}
	}

	// 3) 最后兜底：直接用原生 C++ 技能类。
	//    必须放在最后的理由：前两级都依赖资产，而这一级永远可用 ——
	//    决定了"零配置也能打"，也让 Boss 在没跑过资产脚本时依然可测。
	switch (AttackType)
	{
	case EBossAttackType::MeleeSlash:		return UGA_BossAttack::StaticClass();
	case EBossAttackType::RangedBarrage:	return UGA_BossRangedAttack::StaticClass();
	case EBossAttackType::ChargeSlash:		return UGA_BossChargeSlash::StaticClass();
	case EBossAttackType::Ultimate:			return UGA_BossUltimate::StaticClass();
	default:								return nullptr;
	}
}

void ABossCharacterBase::SetPendingAttackData(UBossAttackData* InAttack)
{
	PendingAttackData = InAttack;
}

void ABossCharacterBase::EnsureBossAbilitiesGranted()
{
	UAbilitySystemComponent* ASC = GetAbilitySystemComponent();
	if (!ASC)
	{
		return;
	}

	static const EBossAttackType AllTypes[] =
	{
		EBossAttackType::MeleeSlash,
		EBossAttackType::RangedBarrage,
		EBossAttackType::ChargeSlash,
		EBossAttackType::Ultimate,
	};

	int32 Granted = 0;
	for (const EBossAttackType Type : AllTypes)
	{
		const TSubclassOf<UGameplayAbility> AbilityClass = GetAbilityClassForAttackType(Type);
		if (!AbilityClass)
		{
			continue;
		}

		// 已经授予过就跳过（默认能力列表里配了的话这里不会重复）
		if (ASC->FindAbilitySpecFromClass(AbilityClass))
		{
			continue;
		}

		ASC->GiveAbility(FGameplayAbilitySpec(AbilityClass, 1, INDEX_NONE, this));
		++Granted;
	}

	if (Granted > 0)
	{
		UE_LOG(LogDemoCombat, Warning, TEXT("[Boss] 自动补授 %d 个招式技能（BP_Boss 的 DefaultAbilities 可以留空）"), Granted);
	}
}

float ABossCharacterBase::GetHealthRatio() const
{
	const float MaxHealth = GetMaxHealth();
	return MaxHealth > 0.f ? GetHealth() / MaxHealth : 1.f;
}

UAnimMontage* ABossCharacterBase::ResolveMontage(TObjectPtr<UAnimMontage>& Slot, const TCHAR* ConventionalPath)
{
	if (Slot)
	{
		return Slot.Get();
	}
	if (!ConventionalPath)
	{
		return nullptr;
	}

	UAnimMontage* Loaded = Cast<UAnimMontage>(StaticLoadObject(UAnimMontage::StaticClass(), nullptr, ConventionalPath));
	if (Loaded)
	{
		Slot = Loaded;
	}
	return Loaded;
}

void ABossCharacterBase::PlayBossMontage(UAnimMontage* Montage, float PlayRate, float RootMotionScale,
	FOnBossMontageFinished Callback)
{
	if (!Montage)
	{
		return;
	}

	USkeletalMeshComponent* MeshComp = GetMesh();
	if (!MeshComp)
	{
		return;
	}

	// 先清掉上一段可能还在跑的"播完"定时器（切段时 StopBossMontage 也会清）
	if (UWorld* W = GetWorld())
	{
		W->GetTimerManager().ClearTimer(BossMontageFinishedTimer);
	}

	// 根位移通道：出招前确保是"从蒙太奇提取"，否则挥砍 / 突进的根位移会被丢掉
	EnsureRootMotionExtraction(TEXT("Boss 播放蒙太奇"));

	// ★ 单节点模式关键修复：只有把蒙太奇设为当前资产（CurrentAsset）才会被渲染。
	//   之前走 Montage_Play，而单节点 proxy 的 UpdateAnimationNode 根本不处理 MontageInstances，
	//   攻击蒙太奇既不可见、其进度也不推进 → "播完"回调永不触发 → 能力卡死在 Attack 状态。
	//   改用 PlayAnimation 设为 CurrentAsset：既可见、单节点也会推进并播完。
	if (UAnimSingleNodeInstance* SN = MeshComp->GetSingleNodeInstance())
	{
		SN->SetPlayRate(PlayRate);
	}
	if (ACharacter* Char = Cast<ACharacter>(this))
	{
		Char->SetAnimRootMotionTranslationScale(RootMotionScale);
	}

	ActivePlayedMontage = Montage;
	ActiveMontageFinishedCallback = Callback;
	bMontageFinishCallbackArmed = Callback.IsBound();

	MeshComp->PlayAnimation(Montage, /*bLoop=*/false);

	// 单节点没有 Montage_Play 的"播完"回调，用定时器按蒙太奇时长触发，
	// 把"蒙太奇结束"回传给当前能力（驱动 OnMontageCompleted / 大招多段推进）。
	if (bMontageFinishCallbackArmed && GetWorld())
	{
		const float Duration = FMath::Max(Montage->GetPlayLength() / FMath::Max(PlayRate, 0.05f), 0.05f);
		GetWorld()->GetTimerManager().SetTimer(BossMontageFinishedTimer, this,
			&ABossCharacterBase::HandleBossMontageFinished, Duration, /*bLoop=*/false);
	}
}

void ABossCharacterBase::HandleBossMontageFinished()
{
	bMontageFinishCallbackArmed = false;
	const FOnBossMontageFinished CB = ActiveMontageFinishedCallback;
	ActiveMontageFinishedCallback.Unbind();
	const TWeakObjectPtr<UAnimMontage> Finished = ActivePlayedMontage;
	ActivePlayedMontage = nullptr;
	if (CB.IsBound())
	{
		CB.ExecuteIfBound(Finished.Get());
	}
}

void ABossCharacterBase::StopBossMontage()
{
	if (UWorld* W = GetWorld())
	{
		W->GetTimerManager().ClearTimer(BossMontageFinishedTimer);
	}
	bMontageFinishCallbackArmed = false;
	ActiveMontageFinishedCallback.Unbind();
	ActivePlayedMontage = nullptr;
}

AActor* ABossCharacterBase::GetCombatTarget() const
{
	if (Brain)
	{
		if (AActor* BrainTarget = Brain->GetTarget())
		{
			return BrainTarget;
		}
	}
	return UGameplayStatics::GetPlayerPawn(GetWorld(), 0);
}

bool ABossCharacterBase::IsAttacking() const
{
	const UAbilitySystemComponent* ASC = GetAbilitySystemComponent();
	return ASC && ASC->HasMatchingGameplayTag(DemoTag::State_Attacking);
}

bool ABossCharacterBase::IsStrikeActive() const
{
	// ⚠ 以 Brain 的状态机为准（而不是只看 GAS 标签）——
	// GAS 标签 State_Attacking 覆盖了"预警→出招→后摇"整段，
	// 而这里只关心【真正挥出那一击】的那一小段。
	//
	// 只认 Attack：预警红光（Telegraph）不算，那段时间应该继续跟随玩家移动。
	// Recover（后摇）也不算：招式已打完、位移也结束了，让 BOSS 重新接管移动更自然。
	if (const UBossBrainComponent* BrainComp = GetBrain())
	{
		return BrainComp->GetState() == EBossState::Attack;
	}
	// Brain 还没就绪时退回旧判定，保证不会因为拿不到状态就完全站死不动
	return IsAttacking();
}

bool ABossCharacterBase::HasSuperArmor() const
{
	if (!bSuperArmorWhileAttacking)
	{
		return false;
	}
	return IsAttacking();
}

// ------------------------------------------------------------------ 阶段切换
void ABossCharacterBase::EnterPhase(int32 PhaseIndex, bool bIsFinalPhase, TSubclassOf<UGameplayEffect> EnrageEffect)
{
	UAbilitySystemComponent* ASC = GetAbilitySystemComponent();
	if (!ASC)
	{
		return;
	}

	// 二阶段标记：动画蓝图 / 材质 / 音效都能据此切表现（例如刀刃发红）
	if (PhaseIndex > 0)
	{
		ASC->AddLooseGameplayTag(DemoTag::State_Boss_Phase2, 1);
	}

	if (EnrageEffect)
	{
		UDemoAbilitySystemLibrary::ApplyEffectToSelf(this, EnrageEffect);
	}

	UE_LOG(LogDemoCombat, Warning, TEXT("[Boss] 进入阶段 %d（%s）%s"),
		PhaseIndex, bIsFinalPhase ? TEXT("最终阶段") : TEXT("普通阶段"),
		EnrageEffect ? TEXT("+ 已施加狂化增益") : TEXT(""));

	OnBossPhaseEntered(PhaseIndex, bIsFinalPhase);
}

// ------------------------------------------------------------------ 范围冲击
int32 ABossCharacterBase::ApplyAreaImpact(float Radius, float HalfAngleDeg, const FVector& ImpactDirection,
	float Damage, float PoiseDamage,
	bool bCanBeParried, bool bCanBeBlocked,
	float KnockbackStrength, float KnockbackUpward,
	EDemoDamageSource DamageSource, bool bDrawDebug)
{
	if (Radius <= 0.f)
	{
		return 0;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return 0;
	}

	const FVector Origin = GetActorLocation();

	// 扇形过滤：把半角换算成 cos 阈值，逐个目标比点积。
	// 用 cos 而不是 atan2 是为了省掉每个目标一次反三角 —— 这里可能一帧跑几十次。
	// HalfAngleDeg <= 0 或 >= 180 都当作"整圈"，防呆。
	const bool bUseArc = (HalfAngleDeg > 0.f && HalfAngleDeg < 180.f);
	FVector Dir2D = FVector(ImpactDirection.X, ImpactDirection.Y, 0.f).GetSafeNormal();
	if (Dir2D.IsNearlyZero())
	{
		Dir2D = GetActorForwardVector().GetSafeNormal2D();
	}
	const float ArcCos = FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(HalfAngleDeg, 0.f, 180.f)));

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(BossAreaImpact), false, this);

	TArray<FOverlapResult> Overlaps;
	World->OverlapMultiByChannel(Overlaps, Origin, FQuat::Identity, ECC_Pawn,
		FCollisionShape::MakeSphere(Radius), QueryParams);

	TSet<AActor*> Handled;
	int32 HitCount = 0;

	for (const FOverlapResult& Overlap : Overlaps)
	{
		AActor* Other = Overlap.GetActor();
		if (!Other || Other == this || Handled.Contains(Other))
		{
			continue;
		}
		if (!IsHostileTo(Other))
		{
			continue;
		}

		const ADemoCharacterBase* TargetChar = Cast<ADemoCharacterBase>(Other);
		if (TargetChar && TargetChar->IsDead())
		{
			continue;
		}

		// ---- 扇形过滤 ----
		if (bUseArc)
		{
			const FVector ToTarget = FVector(Other->GetActorLocation().X - Origin.X,
				Other->GetActorLocation().Y - Origin.Y, 0.f);
			const FVector ToTargetDir = ToTarget.GetSafeNormal();
			if (!ToTargetDir.IsNearlyZero() && FVector::DotProduct(Dir2D, ToTargetDir) < ArcCos)
			{
				continue;
			}
		}

		Handled.Add(Other);

		// 统一伤害入口：无敌帧（闪避）/ 格挡 / 破防暴击 / 敌我过滤全部自动生效
		UDemoAbilitySystemLibrary::ApplyHit(this, Other, Damage, PoiseDamage,
			bCanBeParried, bCanBeBlocked, Origin, true, DamageSource);
		++HitCount;

		// 击退：把玩家从 BOSS 身边推开
		//
		// ⚠ 这里是"别再把玩家打飞"的最后一道闸：
		//   1) 上抛硬夹成 0 —— 只要人被掀离地面，空中没有地面摩擦，
		//      水平速度就会一路带着人飞出去，这是"砍飞"的真正放大器；
		//   2) 水平速度夹到 BossMaxKnockbackSpeed —— 招式表填多大都不会失控；
		//   3) 目标已经在空中（跳跃 / 被别的东西顶飞）时干脆不推，
		//      免得把一次普通击退加成"空中长距离抛飞"。
		const float ClampedH = FMath::Min(KnockbackStrength, DemoBalance::BossMaxKnockbackSpeed);
		const float ClampedV = FMath::Min(KnockbackUpward, DemoBalance::BossMaxKnockbackUpward);
		if (ClampedH > 0.f || ClampedV > 0.f)
		{
			if (ACharacter* TargetCharacter = Cast<ACharacter>(Other))
			{
				const UCharacterMovementComponent* TargetMove = TargetCharacter->GetCharacterMovement();
				if (!TargetMove || !TargetMove->IsFalling())
				{
					FVector PushDir = (Other->GetActorLocation() - Origin).GetSafeNormal2D();
					if (PushDir.IsNearlyZero())
					{
						PushDir = GetActorForwardVector().GetSafeNormal2D();
					}

					// bXYOverride=true（只覆盖水平）+ bZOverride=false（不动竖直）：
					// 玩家在地面时被"震开一下"就停住，不会被顶上天。
					const FVector Launch = PushDir * ClampedH + FVector(0.f, 0.f, ClampedV);
					TargetCharacter->LaunchCharacter(Launch, true, false);
				}
			}
		}
	}

	if (bDrawDebug || bDrawDebugCombat)
	{
		DrawDebugSphere(World, Origin, Radius, 24, HitCount > 0 ? FColor::Red : FColor::Green, false, 1.5f, 0, 2.f);

		// 把扇形也画出来：调 HalfAngleDeg 时能直接看到"哪里能打到、哪里能躲"
		if (bUseArc)
		{
			const FVector LeftDir = Dir2D.RotateAngleAxis(HalfAngleDeg, FVector::UpVector);
			const FVector RightDir = Dir2D.RotateAngleAxis(-HalfAngleDeg, FVector::UpVector);
			DrawDebugLine(World, Origin, Origin + LeftDir * Radius, FColor::Orange, false, 1.5f, 0, 2.f);
			DrawDebugLine(World, Origin, Origin + RightDir * Radius, FColor::Orange, false, 1.5f, 0, 2.f);
		}
	}

	UE_LOG(LogDemoCombat, Log, TEXT("[Boss] 范围冲击：半径=%.0f 半角=%.0f 伤害=%.0f 命中=%d 个目标"),
		Radius, bUseArc ? HalfAngleDeg : 180.f, Damage, HitCount);

	// 蓝图表现：尘土、震屏、Decal 冲击波
	OnBossLandedImpact(Origin, Radius);

	return HitCount;
}

void ABossCharacterBase::Landed(const FHitResult& Hit)
{
	Super::Landed(Hit);

	// 广播物理落地事件。
	// ⚠ 现在没有任何 C++ 技能订阅它（原来的跳砸用它在落地瞬间做判定，而那招已经换成突进斩了）。
	//   保留是因为它对蓝图仍然有用 —— 想做"落地扬尘 / 落地震屏"直接在 BP_Boss 里绑这个委托即可，
	//   不需要为了听一个事件去改 C++。
	OnBossLanded.Broadcast(Hit);
}

// ------------------------------------------------------------------ 受击 / 死亡
void ABossCharacterBase::SpawnBossHitSpark() const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// ⚠ 2026-09-29：这团火花默认关掉了（bBossImpactBurstEnabled = false）。
	// 它的材质 MI_BossImpact（父材质 M_BossSpriteAdd）在 PCD3D_SM6 上编译失败 →
	// 回退默认材质 → 屏幕上就是"BOSS 头顶挂着一个灰球"，用户明确要求去掉。
	// 命中反馈改由『受击白光 HitFlashLight + 伤害飘字 + HUD』承担。
	if (!bBossImpactBurstEnabled)
	{
		return;
	}

	// 每次命中都在胸口高度爆一团火花：之前 OnBossSuperArmorHit / OnBossHitReact 是纯蓝图事件，
	// BP_Boss 一旦没实现就零反馈，玩家感觉"打不中 / 打了没反应"。
	// 这里在 C++ 里兜一层，保证伤害确实进去了（也顺带修了"攻击不到 boss"的体感问题）。
	const FVector Loc = GetActorLocation() + FVector(0.f, 0.f, 80.f);
	ADemoImpactBurst::SpawnBurst(World, Loc, FVector::UpVector,
		FLinearColor(1.f, 0.85f, 0.4f, 1.f), 40.f, 0.25f, true);
}

void ABossCharacterBase::OnDamagedReaction(EDemoHitDirection Direction, float Damage)
{
	if (IsDead())
	{
		return;
	}

	// 是否被这一下挡住了：挡住的命中不算"真挨打"，不能闪白/僵直/播受击蒙太奇，
	// 否则举盾姿态会被受击动作顶掉，BOSS 看起来在"格挡→挨打→格挡"地抖。
	//
	// ⚠⚠ 两个来源都必须算进来（2026-10-03 修正）：
	//   ① State.Blocking 标签 —— BOSS 的"常驻举盾"（UpdateGuardStance）走这条；
	//   ② bGuardedThisHit —— 概率格挡，由 ApplyHit 在【扣血之前】调用
	//      TryGuardIncomingHit 判出来，不会经过 ① 的标签通道。
	//   漏掉 ② 的后果：概率格挡明明成功了（伤害 ×0.25 已生效），这里却仍当成"真挨打"，
	//   照样闪白 + 硬直 + 播受击蒙太奇 → 玩家看到的是"挨打"，完全读不出"它挡住了"。
	//   （这正是第一版"日志里有格挡、画面上看不出格挡"的原因。）
	//   ⚠ 用时间戳做一次超时保护：万一这一下最后没走到伤害事件（例如被无敌帧吃掉），
	//     标记也不会一直挂着去污染下一次命中。
	const double NowGuard = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	const bool bGuardedThisHitNow = bGuardedThisHit && (NowGuard - LastBlockTime) <= 0.25;
	bGuardedThisHit = false;   // 一次性事件，读完即清
	const bool bBlocking = bGuardedThisHitNow || (GetAbilitySystemComponent()
		&& GetAbilitySystemComponent()->HasMatchingGameplayTag(DemoTag::State_Blocking));

	// ---- 复仇计数：被玩家打满 N 下，强制还手一次 ----
	// 计数在每个 BOSS 出招开始时（UGA_BossAttackBase::ActivateAbility）清零，
	// 这里数的是"上一次出招之后"又被揍了多少下。
	// 达到阈值时若 BOSS 正忙（出招 / 起手 / 硬直），先不强制，等它一空闲的下一次受击再触发，
	// 避免打断正在进行的动作。
	// ⚠ 被格挡的命中不计入（那条压力走 OnBlockedIncomingHit 的 BlockedStreak），
	// 两条计数器语义分开：HitsSinceLastAttack = "真打进去了"，BlockedStreak = "一直被挡"。
	if (bEnableRevengeCounter && Brain && !bBlocking)
	{
		++HitsSinceLastAttack;
		if (HitsSinceLastAttack >= MaxHitsBeforeRevenge && !IsAttacking())
		{
			if (Brain->RequestRevengeAttack())
			{
				HitsSinceLastAttack = 0;
			}
		}
	}

	// 被格挡：吸收这一下 —— 不闪白、不僵直、不播受击蒙太奇，保持举盾姿态。
	// "被挡"的反馈完全交给 OnBlockedIncomingHit（冷蓝火花 + 护体光爆闪 + BlockedStreak 反制）。
	if (bBlocking)
	{
		return;
	}

	// 受击闪白：放在"真挨打"路径的开头（被挡不走到这里，不会误闪白）。
	// 这是让玩家"感觉到砍到了"的关键 —— 否则霸体时 BOSS 只爆个小火花、不僵直，
	// 玩家就觉得"伤害在掉但没砍进去"。
	TriggerHitFlash();

	// 霸体：出招期间只闪一下火花，不打断动作。
	// 否则玩家无脑普攻就能把 BOSS 的连招全部点掉，BOSS 一点压迫感都没有。
	if (HasSuperArmor())
	{
		SpawnBossHitSpark();
		OnBossSuperArmorHit(Damage);
		return;
	}

	// 破防虚弱期间由虚弱循环接管，不插普通受击（否则会不停打断虚弱姿态）
	if (bWasBroken)
	{
		SpawnBossHitSpark();
		OnBossHitReact(Direction, Damage);
		return;
	}

	UAnimMontage* Montage = nullptr;
	if (TObjectPtr<UAnimMontage>* Found = HitReactMontages.Find(Direction))
	{
		Montage = Found->Get();
	}
	if (!Montage)
	{
		Montage = HitReactMontage;
	}

	// ---- 短硬直（用户要的"被攻击要有僵直"）----
	// 守卫 1：BOSS 正处于"已 committing 的招式"（起手 / 出招）时不施加 —— 这期间只能靠打空韧性破防来打断，
	//          普通攻击点不掉，保留 BOSS 的压迫感；否则会被玩家无脑普攻无限打断。
	// 守卫 2：已经是 Stagger 状态就跳过，避免被连击把硬直无限刷新成永控（见下方说明）。
	// 只有在 BOSS 处于"可被攻击的空档"（Chase / Idle / Reposition / Recover）时，命中才打出短硬直：
	//   → 施加 BossHitstun（State.Stagger）→ Brain 冻结 BOSS → 硬直结束回 Chase
	//   → 再经过 AttackInterval 的出招间隔才再次出招 = "僵直 CD 后才攻击"。
	const bool bCommitted = IsAttacking() || (Brain && Brain->IsTelegraphing());
	const bool bAlreadyStaggered = GetAbilitySystemComponent()
		&& GetAbilitySystemComponent()->HasMatchingGameplayTag(DemoTag::State_Stagger);

	// ★★ 轻/重分级（打破"每下命中都定身"的刻板观感）：
	//   轻击（连段小伤害）< 阈值 → 只"踉跄"一下（更短硬直 + 较快收招），像在"啃血"；
	//   重击（技能/高伤）≥ 阈值 → 整段硬直，BOSS 被真正打断、给出输出窗口。
	//   玩家普攻连段 28~56、技能 120，阈值 70（BossLightHitDamageThreshold）能干净分开二者。
	const bool bLight = Damage < BossLightHitDamageThreshold;
	const float HitstunScale = bLight ? BossLightHitstunScale : 1.f;
	const float ReactPlayRate = bLight ? 1.2f : 1.f;

	// ⚠⚠⚠ 2026-10-03：硬直【总时长】也要有冷却，否则"每刀都吃一点硬直"会累积成
	//   BOSS 一半时间都在 Stagger（实测心跳状态占比：Stagger 31%，和 Chase 一样多！）
	//   → 每被打一次就定身 0.28s → 移动一截一停 → 行走动画永远走不完一个步态
	//   → 玩家看到的就是【一顿一顿的地面平移 / 太空步】。
	// 动作游戏里"硬直"是【偶尔的奖励窗口】，不是常态。
	// 现在：重击（真正的输出窗口）也遵守硬直冷却，冷却内只播受击动画、不定身。
	const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;

	// ---------- BOSS 格挡：判定已经不在这个回调里了 ----------
	// 见 ABossCharacterBase::TryGuardIncomingHit（以及 DemoCharacterBase.h 的注释）。
	//
	// ⚠⚠⚠ 2026-10-03 第二次重做：原来在这里掷骰子，是【结构性错误】——
	//   本函数是"伤害已经扣完、受击事件已经炸开"之后的【表现回调】，
	//   在这里判格挡只能补播一个动画：
	//     · 血照掉（ApplyHit 的 25% 减伤早就结算完了）；
	//     · 硬直/削韧/异常条照涨（那些在 ApplyHit 里就发了）；
	//     · 连"被格挡"标签都没有 → HUD、DamageNumberLayer 全都按普通命中显示。
	//   实测：一局触发 1 次，玩家完全看不出 BOSS 会格挡。
	//   正解 = 把骰子挪到 ApplyHit 扣血【之前】（TryGuardIncomingHit），
	//   这里只负责"如果已经判成格挡了就别再当挨打处理"（上面的 bBlocking）。
	//
	// 走到这里 = 这一下没被挡住，按正常受击继续。

	const bool bStaggerOffCooldown = (Now - LastStaggerTime) >= StaggerCooldown;
	if (!bCommitted && !bAlreadyStaggered && bStaggerOffCooldown)
	{
		UDemoAbilitySystemLibrary::ApplyBossHitstun(this, HitstunScale);
		LastStaggerTime = Now;
	}

	// 受击硬直：可被攻击的间隙被命中 → 播四向受击蒙太奇（BeginPlay 已按约定路径加载）
	// ⚠ 走 PlayReactionMontage 而不是 PlayCombatMontage：
	//   受击蒙太奇的源动画天生带根位移，照常播会把 BOSS 推着滑一大截
	//   （"我砍一刀它就退一步"，玩家能把 BOSS 推着满场走）。动作照播，人不许动。
	//   轻击给稍快的收招速率，让"踉跄"和"硬直"在视觉上也能区分开。
	PlayReactionMontage(Montage, ReactPlayRate);
	SpawnBossHitSpark();
	OnBossHitReact(Direction, Damage);
}

void ABossCharacterBase::HandleBossPoiseBroken()
{
	// 破防 = BOSS 的"停手窗口"：这一段它不许出招、也不许走位，
	// 玩家才有"我把它的韧性打空了"的实感、也才有安全输出的空间。
	//
	// ⚠ 之前这里【只打了一行日志】，没有任何停手处理 ——
	//   结果 BOSS 韧性清零后照样追着玩家砍，玩家完全感受不到破防这件事发生了。
	//   破防动作（虚弱蒙太奇）由 Tick 的状态边沿负责，这里只管"不许动"。
	UE_LOG(LogDemoCombat, Warning,
		TEXT("[Boss] 韧性被打空 → 进入破防虚弱（%.1fs 处决窗口：期间不出招、不走位）"),
		DemoBalance::BossPoiseBreakWeakDuration);

	UAbilitySystemComponent* ASC = GetAbilitySystemComponent();
	if (!ASC)
	{
		return;
	}

	// 1) 挂硬直标签 → Brain 的 Tick 会据此进入 Stagger 状态。
	//    那个状态既不请求移动、也不选招，一直等到标签消失才回 Chase。
	//    复用它而不是在 Brain 里另加一套"破防分支"，是为了让"不能动"只有一处实现，
	//    不会出现"状态机说停了但移动还在跑"这种两边不同步。
	ASC->AddLooseGameplayTag(DemoTag::State_Stagger);

	// 2) 取消正在进行的攻击技能：破防了还把刀挥完，玩家会觉得被偷袭
	FGameplayTagContainer CancelTags;
	CancelTags.AddTag(DemoTag::Ability_Attack_Normal);
	CancelTags.AddTag(DemoTag::Ability_Attack_Skill);
	ASC->CancelAbilities(&CancelTags);

	// 3) 立刻刹住位移 —— 尤其是突进途中被破防，否则会带着冲刺惯性滑出去一大截
	if (UCharacterMovementComponent* MoveComp = GetCharacterMovement())
	{
		MoveComp->StopMovementImmediately();
	}

	// 4) 定时解除：窗口结束摘掉硬直标签，Brain 自动回到 Chase
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(PoiseBreakRecoverTimerHandle, this,
			&ABossCharacterBase::RecoverFromPoiseBreak,
			DemoBalance::BossPoiseBreakWeakDuration, /*bLoop=*/false);
	}
}

void ABossCharacterBase::RecoverFromPoiseBreak()
{
	UAbilitySystemComponent* ASC = GetAbilitySystemComponent();
	if (ASC)
	{
		ASC->RemoveLooseGameplayTag(DemoTag::State_Stagger);
	}
}

void ABossCharacterBase::OnDeathReaction()
{
	// 1) 关掉大脑：不再出招、不再走位
	if (Brain)
	{
		Brain->SetComponentTickEnabled(false);
	}

	if (UCharacterMovementComponent* MoveComp = GetCharacterMovement())
	{
		MoveComp->StopMovementImmediately();
		MoveComp->DisableMovement();
	}

	// 2) 死亡动画：屏蔽动画自带的根位移，否则尸体会被动画推着滑出去。
	//    改走单节点直接播死亡序列（不循环 → 停在最后一帧的死亡姿势），
	//    不再用蒙太奇（蒙太奇播完会回落到待机，BOSS 会"站起来"）。
	if (UAnimInstance* AnimInstance = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr)
	{
		AnimInstance->SetRootMotionMode(ERootMotionMode::IgnoreRootMotion);
	}
	if (USkeletalMeshComponent* MeshComp = GetMesh())
	{
		if (!DeathSequence)
		{
			DeathSequence = Cast<UAnimSequence>(StaticLoadObject(
				UAnimSequence::StaticClass(), nullptr,
				TEXT("/Game/ParagonKhaimera/Characters/Heroes/Khaimera/Animations/Death_A.Death_A")));
		}
		if (DeathSequence)
		{
			MeshComp->PlayAnimation(DeathSequence, /*bLoop=*/false);
		}
		else
		{
			PlayMontageOnMesh(DeathMontage); // 兜底：拿不到底层序列就仍用蒙太奇
		}
	}

	// 3) 清掉场上自己的弹幕：人死了弹幕还在飞会很奇怪
	if (bClearProjectilesOnDeath)
	{
		if (UWorld* World = GetWorld())
		{
			TArray<AActor*> ToDestroy;
			for (TActorIterator<ADemoProjectile> It(World); It; ++It)
			{
				if (It->GetOwner() == this)
				{
					ToDestroy.Add(*It);
				}
			}
			for (AActor* Projectile : ToDestroy)
			{
				if (IsValid(Projectile))
				{
					Projectile->Destroy();
				}
			}
		}
	}

	// 4) 关闭碰撞：尸体不再挡路、也不该再被弹幕结算
	if (UCapsuleComponent* Capsule = GetCapsuleComponent())
	{
		Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}

	if (DeathDestroyDelay > 0.f)
	{
		SetLifeSpan(DeathDestroyDelay);
	}

	UE_LOG(LogDemoCombat, Warning, TEXT("[Boss] %s 已被击败"), *GetName());
}

// ------------------------------------------------------------------ 动画
void ABossCharacterBase::EnsureRootMotionExtraction(const FString& Reason)
{
	UAnimInstance* AnimInstance = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr;
	if (!AnimInstance)
	{
		return;
	}

	// 判据改成"不等于期望值"而不是"等于 NoRootMotionExtraction"：
	// 受击蒙太奇播放期间会临时把模式设成 IgnoreRootMotion（防止动画推着 BOSS 走），
	// 只认 NoRootMotionExtraction 的话，这种情况修不回来 ——
	// 下一招的位移就会被上一次受击留下的闸吃掉。
	if (AnimInstance->RootMotionMode != ERootMotionMode::RootMotionFromMontagesOnly)
	{
		const int32 OldMode = static_cast<int32>(AnimInstance->RootMotionMode);
		AnimInstance->SetRootMotionMode(ERootMotionMode::RootMotionFromMontagesOnly);
		bRootMotionVerified = true;
		bReactionRootMotionSuppressed = false;

		UE_LOG(LogDemoCombat, Warning,
			TEXT("[Boss][RootMotion] %s：根位移模式 %d(NoRootMotionExtraction) → 1(RootMotionFromMontagesOnly)"),
			*Reason, OldMode);
	}
}

void ABossCharacterBase::PlayCombatMontage(UAnimMontage* Montage, float PlayRate)
{
	if (!Montage)
	{
		return;
	}

	// 出招前先保证根位移通道是通的（挥砍 / 突进斩都靠它做真位移）
	EnsureRootMotionExtraction(TEXT("出招"));
	PlayMontageOnMesh(Montage, PlayRate);
}

// ------------------------------------------------------------------ 被动反应蒙太奇
bool ABossCharacterBase::IsReactionMontage(const UAnimMontage* Montage) const
{
	if (!Montage)
	{
		return false;
	}

	if (Montage == HitReactMontage || Montage == WeakMontage || Montage == WeakLoopMontage
		|| Montage == WeakRecoverMontage || Montage == StaggerMontage)
	{
		return true;
	}

	for (const TPair<EDemoHitDirection, TObjectPtr<UAnimMontage>>& Pair : HitReactMontages)
	{
		if (Pair.Value == Montage)
		{
			return true;
		}
	}

	return false;
}

// ==================================================================================
// BOSS 格挡（2026-10-03 重做）
//
// 需求："boss 也可以格挡角色的攻击，不要太频繁，要有蒙太奇动画"。
//
// 三段式：判定 → 结算 → 表现，顺序不能乱。
//   ①【判定】TryGuardIncomingHit：由 UDemoAbilitySystemLibrary::ApplyHit 在
//     【扣血之前】调用。返回 true 这一下就算"被格挡"（伤害 ×0.25、扣攻方架势、
//     发 OnBlockedIncomingHit）。
//   ②【结算】全部由 ApplyHit 现有的格挡分支完成，这里不碰。
//   ③【表现】OnBossBlockPlayerAttack：举盾蒙太奇 + 冷蓝护体光 + 火花（火花在
//     OnBlockedIncomingHit 里）。举盾只保持 GuardHoldTime 秒，然后交还给行走。
//
// 频率控制 = 概率 × 冷却 ×【保底】。保底是这一版加的：
//   0.35 的概率在真实遭遇战里可能连着几次都不中，玩家会得出"BOSS 不会格挡"的结论。
//   连续 GuardPityHits 次有资格却没中 → 下一次强制中。
// ==================================================================================
bool ABossCharacterBase::TryGuardIncomingHit(AActor* Attacker, float IncomingDamage, bool bCanBeBlocked)
{
	// 每次有命中来都先清标记：它是"本一下"的一次性结果，不能跨命中残留。
	bGuardedThisHit = false;

	if (!bCanBeBlocked || IsDead())
	{
		return false;
	}

	// 不可格挡的攻击（大招 / 处决）一律不挡
	const UAbilitySystemComponent* ASC = GetAbilitySystemComponent();
	if (!ASC)
	{
		return false;
	}

	// 只在"BOSS 空闲"时格挡：出招 / 起手 / 硬直 / 破防虚弱期间不挡。
	//   出招中挡 → 叠在招式上会让它卡住，玩家怎么打都是"无效"；
	//   硬直中挡 → 会吃掉玩家辛苦打出来的输出窗口。
	//
	// ⚠ 2026-10-04：判定【不再用 IsAttacking()】。
	//   IsAttacking() 读的是 GAS 的 State_Attacking，而那个标签覆盖"预警→出招→后摇"整段，
	//   于是后摇（招式已打完、位移也结束）明明是最该能防的时候，却被一起禁掉了。
	//   实测 40 次模拟命中里有 20 次"无资格"，总体格挡率只有 22% —— 体感就是"一味挨打"。
	//   现在改成按 Brain 状态机判：只有【起手预警】和【真正在挥那一击】不挡，
	//   后摇 / 追击 / 走位 / 待机都允许（博弈感在，但玩家不再只能干看着）。
	const bool bCommitted = [&]() -> bool
	{
		if (const UBossBrainComponent* BrainComp = GetBrain())
		{
			const EBossState St = BrainComp->GetState();
			return St == EBossState::Telegraph || St == EBossState::Attack;
		}
		// Brain 没就绪时退回旧判定，保证不会因为拿不到状态就完全失去防御
		return IsAttacking() || (Brain && Brain->IsTelegraphing());
	}();
	const bool bAlreadyStaggered = ASC->HasMatchingGameplayTag(DemoTag::State_Stagger);
	const bool bBroken = ASC->HasMatchingGameplayTag(DemoTag::State_Broken);

	const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	const float CooldownLeft = FMath::Max(0.f, GuardRollCooldown - static_cast<float>(Now - LastBlockTime));

	const bool bEligible = !bCommitted && !bAlreadyStaggered && !bBroken && (CooldownLeft <= 0.f);

	bool bBlock = false;
	if (bEligible)
	{
		++GuardEligibleHits;
		const bool bPity = (GuardPityHits > 0) && (GuardEligibleHits >= GuardPityHits);
		bBlock = bPity || (GuardRollChance > 0.f && FMath::FRand() <= GuardRollChance);
	}

	// ⚠⚠ 诊断日志（2026-10-03 第二版）：必须是 Log 级。
	//   第一版写在 Verbose，等于没写 —— 用户把整份日志发过来也看不到任何格挡判定痕迹，
	//   只能在"概率太低 / 冷却没到 / 一直在出招 / 蒙太奇播不出来"之间瞎猜。
	//   玩家一次攻击才出一条，不会刷屏。
	UE_LOG(LogDemoCombat, Log,
		TEXT("[Boss][格挡判定] 概率=%.2f 冷却剩余=%.2fs 已committed=%d 已硬直=%d 已破防=%d 连续未挡=%d/%d → %s"),
		GuardRollChance, CooldownLeft,
		bCommitted ? 1 : 0, bAlreadyStaggered ? 1 : 0, bBroken ? 1 : 0,
		GuardEligibleHits, GuardPityHits,
		bBlock ? TEXT("★格挡★") : (bEligible ? TEXT("不格挡") : TEXT("无资格(出招/硬直/冷却中)")));

	if (!bBlock)
	{
		return false;
	}

	LastBlockTime = Now;
	GuardEligibleHits = 0;
	bGuardedThisHit = true;

	// ★ 举盾窗口打开：这 GuardHoldTime 秒里 State_Blocking 真的挂着。
	//   语义 = "盾已经举起来了"，所以窗口内后续命中一律算被挡（伤害 ×0.25 + 扣攻方架势），
	//   这是玩家能理解的因果关系：举着盾的时候砍不动，收盾了才砍得动。
	//   ⚠ 必须是 LooseTag（不是 GE 授予的），摘的时候才能用 RemoveLooseGameplayTag。
	bGuardWindowActive = true;
	if (UAbilitySystemComponent* MutableASC = GetAbilitySystemComponent())
	{
		MutableASC->AddLooseGameplayTag(DemoTag::State_Blocking);
	}

	OnBossBlockPlayerAttack();
	return true;
}

void ABossCharacterBase::OnBossBlockPlayerAttack()
{
	// 举盾蒙太奇：优先用指定资产，否则用已存在的 Boss_Guard_Idle_Montage
	UAnimMontage* Montage = BlockMontage;
	if (!Montage)
	{
		Montage = GuardMontage;   // BeginPlay 已按约定路径加载 Boss_Guard_Idle_Montage
	}

	// 护体光亮起（冷蓝 = 防御，与预警红/受击白/失衡绿区分开）
	if (GuardLight)
	{
		GuardLight->SetVisibility(true);
		GuardLight->SetIntensity(GuardLightIntensity);
		// 定时熄灭（独立计时器，不依赖技能）
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().SetTimer(
				GuardFlashOffHandle, this, &ABossCharacterBase::TurnOffGuardFlash,
				FMath::Max(0.25f, GuardFlashDuration), /*bLoop=*/false);
		}
	}

	// ⚠ 诊断（2026-10-03）：格挡蒙太奇播不出来时，要能一眼区分三种原因：
	//   ① 资产没加载到 → Montage 为空
	//   ② 骨架不匹配 → 静默失败（最常见！SwordAnimsetPro vs Khaimera）
	//   ③ 播了但立刻被别的蒙太奇顶掉
	if (!Montage)
	{
		UE_LOG(LogDemoCombat, Warning,
			TEXT("[Boss] ★格挡★ ⚠ 蒙太奇缺失！BlockMontage 与 GuardMontage 都是空 → 不会播任何动画"));
	}
	else
	{
		const USkeletalMeshComponent* MeshChk = GetMesh();
		const USkeleton* MeshSkelChk = (MeshChk && MeshChk->GetSkeletalMeshAsset())
			? MeshChk->GetSkeletalMeshAsset()->GetSkeleton() : nullptr;
		const USkeleton* MontageSkel = Montage->GetSkeleton();
		const bool bSkelOk = (MeshSkelChk == nullptr) || (MontageSkel == MeshSkelChk);

		// ⚠ 用 PlayMontageOnMesh（= PlayAnimation/SetAnimationAsset）而不是 AnimInstance->Montage_Play。
		//   单节点模式下【只有 CurrentAsset 是带 Slot 的蒙太奇时才会渲染 Slot 里的东西】，
		//   而 Montage_Play 不会改 CurrentAsset（见 ApplyLocomotionForFrame 的完整说明）。
		//   举盾要"抢到画面"，就必须自己成为 CurrentAsset。
		PlayReactionMontage(Montage, 1.f);

		const UAnimSingleNodeInstance* SN = MeshChk ? MeshChk->GetSingleNodeInstance() : nullptr;
		const bool bPlaying = SN && (SN->GetAnimationAsset() == Montage);
		UE_LOG(LogDemoCombat, Log,
			TEXT("[Boss] ★格挡★ 播蒙太奇「%s」时长=%.2fs 骨架匹配=%d 已成为当前资产=%d%s"),
			*Montage->GetName(), Montage->GetPlayLength(), bSkelOk ? 1 : 0, bPlaying ? 1 : 0,
			bSkelOk ? TEXT("") : TEXT("  ⚠ 骨架不一致，播不出来"));
	}

	// 举盾只保持 GuardHoldTime 秒（举盾蒙太奇本身 3.3s）。
	// 到点把 CurrentLocoMontage 置空 → 下一次 Tick 的 loco 逻辑会自动把
	// 待机/行走蒙太奇重新挂成 CurrentAsset，BOSS 不会举着盾发呆三秒。
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(
			GuardRestoreHandle, this, &ABossCharacterBase::RestoreLocoAfterGuard,
			FMath::Max(0.15f, GuardHoldTime), /*bLoop=*/false);
	}

	UE_LOG(LogDemoCombat, Log,
		TEXT("[Boss] ★格挡★ 挡下玩家一下（伤害 ×0.25，格挡冷却 %.1fs，举盾保持 %.2fs）"),
		GuardRollCooldown, GuardHoldTime);
}

void ABossCharacterBase::RestoreLocoAfterGuard()
{
	// ⚠⚠ 只把 CurrentLocoMontage 置空是【不够的】，必须真的把举盾蒙太奇停掉。
	//
	// 光置空的推演后果：举盾蒙太奇 3.33s，它还在 MontageInstances 里正常播着 →
	// UpdateBossLocomotion 判定"还有【别的】蒙太奇在播"(bOtherMontagePlayingNow=true)
	// → 拒绝重挂底层行走蒙太奇 → BOSS 会保持举盾姿势整整 3.3 秒（一边平移一边举盾）。
	//   表现就是"它一挡就举着盾不撒手"，比不会格挡更怪。
	//
	// 所以这里显式 Montage_Stop（0.15s 淡出），把画面交还给行走。
	// 之后具体播待机还是哪个方向的行走，交给 UpdateBossLocomotion 判断（最稳）。
	if (USkeletalMeshComponent* MeshPtr = GetMesh())
	{
		if (UAnimInstance* AI = MeshPtr->GetAnimInstance())
		{
			if (GuardMontage)
			{
				AI->Montage_Stop(0.15f, GuardMontage.Get());
			}
			if (BlockMontage && BlockMontage != GuardMontage)
			{
				AI->Montage_Stop(0.15f, BlockMontage.Get());
			}
		}
	}

	// 置空 = 让 loco 逻辑下一帧无条件重新挂一次（不再依赖"上一个是不是它"）
	CurrentLocoMontage = nullptr;

	// ★ 举盾窗口关闭：盾收了，State_Blocking 必须摘掉，
	//   否则 BOSS 会一直挂着"在格挡"的标签 → 之后所有命中又变成常驻挡（回到老问题）。
	if (bGuardWindowActive)
	{
		bGuardWindowActive = false;
		if (UAbilitySystemComponent* ASC2 = GetAbilitySystemComponent())
		{
			ASC2->RemoveLooseGameplayTag(DemoTag::State_Blocking);
		}
	}
}

// ------------------------------------------------------------------ 格挡自检钩子
void ABossCharacterBase::TickGuardSelfTest(float DeltaTime)
{
	// ⚠ 不回写 CVar：IConsoleVariable::Set 有模板重载、和 Set(int32, FSetContext) 容易撞歧义，
	//   而且把开关写回 0 会让"再设一次同值"失效。改用成员计数，CVar 只当"装填指令"。
	const int32 Wanted = CVarBossGuardTest.GetValueOnGameThread();
	if (Wanted <= 0)
	{
		GuardTestArmed = 0;
		GuardTestTimer = 0.f;
		return;
	}
	if (GuardTestArmed != Wanted)
	{
		// 控制台刚设了新值 → 重新装填
		GuardTestArmed = Wanted;
		GuardTestHitsLeft = Wanted;
		GuardTestTimer = 0.f;
	}
	if (GuardTestHitsLeft <= 0)
	{
		return;
	}

	GuardTestTimer += DeltaTime;
	if (GuardTestTimer < 1.2f)
	{
		return;
	}
	GuardTestTimer = 0.f;

	const int32 HitIndex = (Wanted - GuardTestHitsLeft) + 1;
	--GuardTestHitsLeft;

	AActor* Player = GetCombatTarget();
	if (!Player)
	{
		for (TActorIterator<APawn> It(GetWorld()); It; ++It)
		{
			if (APawn* P = *It)
			{
				if (P->IsPlayerControlled() || (P != this && P->GetInstigatorController()))
				{
					Player = P;
					break;
				}
			}
		}
	}
	if (!Player || Player == this)
	{
		UE_LOG(LogDemoCombat, Warning,
			TEXT("[Boss][格挡自检] 找不到玩家，无法模拟命中"));
		return;
	}

	const FVector HitFrom = Player->GetActorLocation();
	const EDemoHitResultType R = UDemoAbilitySystemLibrary::ApplyHit(
		Player, this,
		/*BaseDamage=*/60.f,
		/*PoiseDamage=*/15.f,
		/*bCanBeParried=*/true,
		/*bCanBeBlocked=*/true,
		HitFrom,
		/*bTriggerHitStop=*/false,
		EDemoDamageSource::Melee);

	UE_LOG(LogDemoCombat, Log,
		TEXT("[Boss][格挡自检] 模拟玩家命中 #%d（剩余 %d）→ 结算结果=%d（0=普通 1=格挡 2=弹反 3=闪避）当前血量=%.0f"),
		HitIndex, GuardTestHitsLeft, static_cast<int32>(R),
		GetAbilitySystemComponent() ? GetAbilitySystemComponent()->GetNumericAttribute(UDemoAttributeSet::GetHealthAttribute()) : -1.f);
}

void ABossCharacterBase::TurnOffGuardFlash()
{
	if (GuardLight)
	{
		// 强度归零但保持可见（护体光的"常驻"语义是"可以随时点亮"）
		GuardLight->SetIntensity(0.f);
	}
}

void ABossCharacterBase::PlayReactionMontage(UAnimMontage* Montage, float PlayRate)
{
	if (!Montage)
	{
		return;
	}

	// 受击 / 虚弱 / 硬直这些都是"被打出来的被动反应"，它们的源动画来自
	// SwordAnimsetPro 的 Root-Motion 目录，天生带根位移。
	// 照常播 = 每挨一刀 BOSS 就被动画推着滑一截（玩家能把 BOSS 推着满场走），
	// 所以播放前先把根位移关掉：动作照播，胶囊体不许动。
	//
	// 出招中（霸体）不会走到这里 —— 那时走的是技能自己的 PlayMontageTask，
	// 真位移必须保留，否则突进斩 / 挥砍会变成"原地空挥"。
	UAnimInstance* AnimInstance = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr;
	if (AnimInstance && !IsAttacking())
	{
		AnimInstance->SetRootMotionMode(ERootMotionMode::IgnoreRootMotion);
		bReactionRootMotionSuppressed = true;
	}

	PlayMontageOnMesh(Montage, PlayRate);
}

void ABossCharacterBase::UpdateReactionRootMotionGuard()
{
	UAnimInstance* AnimInstance = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr;
	if (!AnimInstance)
	{
		return;
	}

	// 当前是否在播"被动反应"蒙太奇
	bool bPlayingReaction = false;
	if (UAnimMontage* Active = AnimInstance->GetCurrentActiveMontage())
	{
		bPlayingReaction = IsReactionMontage(Active);
	}

	// 出招中一律放行：真位移优先，绝不能因为一次受击把整招的位移吃掉
	const bool bShouldSuppress = bPlayingReaction && !IsAttacking();

	if (bShouldSuppress == bReactionRootMotionSuppressed)
	{
		return;
	}

	bReactionRootMotionSuppressed = bShouldSuppress;
	AnimInstance->SetRootMotionMode(bShouldSuppress
		? ERootMotionMode::IgnoreRootMotion
		: ERootMotionMode::RootMotionFromMontagesOnly);
}

// ------------------------------------------------------------------ Mesh ↔ 胶囊体对齐防御校正
//
// ⚠⚠⚠ 2026-10-12 决定性修复（用户录屏证实的"移动时模型不动、在地面平移"）
//
// 现象：BOSS 整体悬空 —— 脚底离地一大截，影子在远处，视觉上"模型不动、贴着地面平移"。
//
// 原实现有两个致命问题：
//   ① 只在 BeginPlay 调一次，用的是【参考姿势】的 Bounds。
//      动画把姿势换成行走/攻击后最低点就变了，但没人再复查 → 越动越悬空。
//   ② 用 `AddWorldOffset` 修正 —— 它是"在当前世界位置再加一点"、**不写回相对位移**，
//      每次调用都在上次结果上继续累加 → 网格一点点漂走，永远对不齐。
//
// 现在的做法：
//   · 每帧在动画求值之后调用（见 Tick）；
//   · 用【当前动画姿势】的真实骨骼最低点（脚骨），而不是参考姿势 Bounds；
//   · 用 SetRelativeLocation 写回相对位移（幂等，重复调用不会累加漂移）；
//   · 只做一次偏移量计算（把 Mesh 拉到底面），不做逐帧累加。
void ABossCharacterBase::EnsureMeshCapsuleAlignment()
{
	USkeletalMeshComponent* MeshComp = GetMesh();
	UCapsuleComponent* Capsule = GetComponentByClass<UCapsuleComponent>();
	if (!MeshComp || !Capsule)
	{
		return;
	}
	// 没有网格资产时 Bounds 是无意义的默认值，直接跳过（否则会把偏移改坏）
	if (!MeshComp->GetSkeletalMeshAsset())
	{
		return;
	}

	const float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
	if (HalfHeight <= KINDA_SMALL_NUMBER)
	{
		return;
	}

	// ⚠ 用【当前姿势】的真实骨骼最低点，而不是 MeshComp->Bounds（那是参考姿势的包围盒）。
	//   行走/攻击姿势的最低点和参考姿势能差几十厘米，用 Bounds 会算出一个"永远对不上"的偏移。
	//   脚骨（foot_l/foot_r/calf_l/calf_r）就是支撑腿，取它们的最低点即可。
	float MeshBottomZ = TNumericLimits<float>::Max();
	{
		static const FName FootBones[] = { TEXT("foot_l"), TEXT("foot_r"), TEXT("calf_l"), TEXT("calf_r") };
		bool bFoundAny = false;
		for (const FName BoneName : FootBones)
		{
			const int32 BoneIdx = MeshComp->GetBoneIndex(BoneName);
			if (BoneIdx == INDEX_NONE)
			{
				continue;
			}
			// GetBoneLocation(EBoneSpaces::WorldSpace) = 这一帧【动画姿势】下该骨的世界坐标
			const float BoneZ = MeshComp->GetBoneLocation(BoneName, EBoneSpaces::WorldSpace).Z;
			MeshBottomZ = FMath::Min(MeshBottomZ, BoneZ);
			bFoundAny = true;
		}
		// 骨骼名对不上（换骨架）→ 回退到 Bounds，至少别把偏移算成 0
		if (!bFoundAny)
		{
			const FBoxSphereBounds Bounds = MeshComp->Bounds;
			MeshBottomZ = Bounds.Origin.Z - Bounds.BoxExtent.Z;
		}
	}

	const float CapsuleBottomZ = Capsule->GetComponentLocation().Z - HalfHeight;
	const float Delta = MeshBottomZ - CapsuleBottomZ;

	// 已经在容差内就完全不动，避免每帧来回微调导致的抖动
	if (FMath::Abs(Delta) <= MeshAlignmentTolerance)
	{
		return;
	}

	// 每帧都收敛，但要【限速】：
	//   直接一次性拉过去会在刚出生/传送/换姿势的瞬间猛地一弹（看起来更糟），
	//   限速后是"快速但平滑"地贴地。
	const float DeltaTime = GetWorld() ? GetWorld()->GetDeltaSeconds() : 0.f;
	const float MaxStep = MeshAlignmentMaxSpeed * DeltaTime;
	const float Clamped = FMath::Clamp(Delta, -MaxStep, MaxStep);
	if (FMath::Abs(Clamped) <= KINDA_SMALL_NUMBER)
	{
		return;
	}

	// 只沿"胶囊的本地 Z"补偿：胶囊的 Z 就是角色的上方向，比世界 Z 更稳
	//（角色被贴地约束/倾斜时也不会把网格怼歪）。
	//
	// ⚠ 必须用【相对位移】SetRelativeLocation，不能用 AddWorldOffset：
	//   AddWorldOffset 不写回相对位移，每次调用都在上次结果上继续累加 → 网格漂走。
	//   SetRelativeLocation 写的是"相对父组件的固定偏移"，重复调用是幂等的。
	const FVector Up = Capsule->GetUpVector();
	const FVector RelLoc = MeshComp->GetRelativeLocation();
	MeshComp->SetRelativeLocation(RelLoc - Clamped * Up);
}

// 每帧贴地修正的包装：把 Mesh 拉到底面。
// 单独一个函数是为了让 Tick 里只出现一行调用，注释集中在这里。
// 为什么必须每帧：动画姿势一变，最低点就变；只校准一次的话 BOSS 会越动越悬空
//（用户录屏里"移动时模型不动、贴着地面平移"就是这个）。
void ABossCharacterBase::UpdateMeshGroundAlignment()
{
	EnsureMeshCapsuleAlignment();
}

// ------------------------------------------------------------------ 位移安全阀
void ABossCharacterBase::UpdateDisplacementGuard(float DeltaTime)
{
	const FVector Now = GetActorLocation();

	// ⚠⚠ 死亡后这个闸【必须彻底让路】（2026-10-04）
	//
	// 下面按"非出招 = 走路速度 × 余量"（约 600cm/s）放行 —— 活着的时候这是防穿帮的兜底，
	// 死了之后它就变成了"尸体还能滑"的元凶：任何外力（弹反击退、残留 RequestDirectMove、
	// 玩家碰撞）推一下，这个闸不但不拦，还会用 SetActorLocation 把尸体一路"夹"着往前送。
	// 死亡位置由 UpdateDeathFreeze 钉死，这里只负责把锚点刷成当前位置。
	if (IsDead())
	{
		GuardAnchorLocation = Now;
		bGuardAnchorValid = true;
		return;
	}

	if (!bDisplacementGuardEnabled || DeltaTime <= 0.f)
	{
		GuardAnchorLocation = Now;
		bGuardAnchorValid = true;
		return;
	}

	if (!bGuardAnchorValid)
	{
		GuardAnchorLocation = Now;
		bGuardAnchorValid = true;
		return;
	}

	const UCharacterMovementComponent* MoveComp = GetCharacterMovement();
	const bool bAttacking = IsAttacking();

	// 当前这招是不是"突进类"（唯一允许高速位移的招式）。
	// 只有突进斩/Pounce 需要跑起来冲，其余招式都是站桩 —— 站桩招式却在高速平移，
	// 就是"模型整体滑过去、腿不动"的直接原因（见下面 Attack 分支的注释）。
	bool bChargeAttack = false;
	if (bAttacking && Brain)
	{
		if (const UBossAttackData* Atk = Brain->GetCurrentAttack())
		{
			bChargeAttack = (Atk->AttackType == EBossAttackType::ChargeSlash);
		}
	}

	float AllowedSpeed;
	if (MoveComp && MoveComp->IsFalling())
	{
		// 坠落：重力速度远大于任何"合法移动速度"，夹了会让 BOSS 悬在空中
		AllowedSpeed = GuardFallingMaxSpeed;
	}
	else if (bAttacking && bChargeAttack)
	{
		// 只有【突进类】招式允许高速位移（突进斩 1100cm/s）
		AllowedSpeed = GuardAttackMaxSpeed;
	}
	else if (bAttacking)
	{
		// ⚠⚠⚠ 非突进招式（挥砍/横扫/转身/投掷）必须按【走路速度】夹（2026-10-02 决定性修复）
		//
		// 之前所有出招都放行 1500cm/s（走路速度的 4 倍），于是：
		//   出招期间 UpdateBossLocomotion 因 IsAnyMontagePlaying() 直接 return（不播行走动画），
		//   而这段位移闸却放行 4 倍速 → 玩家看到"整体模型在地面上平移、腿一动不动"。
		//   实测整局 Jog_Fwd 只占 8.5%、Idle 57%，其余全是攻击蒙太奇 —— 正是这个现象的统计侧证。
		// 现在非突进招式只能以走路量级位移，突进的快移交给 IsChargeAttack() 那条分支。
		AllowedSpeed = FMath::Max(MoveComp ? MoveComp->GetMaxSpeed() : 400.f, 200.f) * GuardPassiveSpeedMargin;
	}
	else
	{
		AllowedSpeed = FMath::Max(MoveComp ? MoveComp->GetMaxSpeed() : 400.f, 200.f) * GuardPassiveSpeedMargin;
	}

	// +1.5cm 的死区：斜坡上的微小抖动不该被当成"被推"
	const float Allowed = AllowedSpeed * DeltaTime + 1.5f;

	const FVector Delta = Now - GuardAnchorLocation;
	const float Dist = Delta.Size();

	if (Dist <= Allowed)
	{
		GuardAnchorLocation = Now;
		return;
	}

	// 这一帧只能走到 Allowed 这么远 —— 多出来的部分无论来自什么都不要
	const FVector Fixed = GuardAnchorLocation + (Delta / Dist) * Allowed;
	// 用普通 SetActorLocation（不传 TeleportPhysics）：硬性拉回超额位移，
	// 但不再触发"瞬移"式的物理跳变。根因 Pawn↔Pawn 已设 Overlap，正常情况下本闸不会再触发；
	// 即便触发，也是平滑地"夹回"而非一帧闪现。
	SetActorLocation(Fixed, /*bSweep=*/false, nullptr, ETeleportType::None);

	GuardLogCooldown -= DeltaTime;
	if (GuardLogCooldown <= 0.f)
	{
		GuardLogCooldown = 0.5f;
		UE_LOG(LogDemoCombat, Warning,
			TEXT("[Boss][位移闸] 单帧位移 %.1fcm > 允许 %.1fcm（%s，允许速度 %.0fcm/s）→ 已拉回；方向=%s 竖直分量=%.1fcm"),
			Dist, Allowed, bAttacking ? TEXT("出招中") : TEXT("非出招"), AllowedSpeed,
			*Delta.GetSafeNormal().ToString(), Delta.Z);
	}

	GuardAnchorLocation = Fixed;
}

// ------------------------------------------------------------------ 受击闪白
void ABossCharacterBase::TriggerHitFlash()
{
	if (!bHitFlashEnabled || !HitFlashLight)
	{
		return;
	}

	// 每次命中刷新剩余时间（不叠加亮度，避免连段越闪越亮），点亮白光。
	HitFlashRemaining = HitFlashDuration;
	HitFlashLight->SetAttenuationRadius(HitFlashLightRadius);
	HitFlashLight->SetIntensity(HitFlashLightIntensity);
	if (!HitFlashLight->IsVisible())
	{
		HitFlashLight->SetVisibility(true);
	}
}

void ABossCharacterBase::UpdateHitFlash(float DeltaTime)
{
	if (!HitFlashLight)
	{
		return;
	}

	if (HitFlashRemaining > 0.f)
	{
		HitFlashRemaining -= DeltaTime;

		// 线性衰减（也可以用 sin 但一帧级的脉冲线性就够干脆），到 0 熄灭。
		const float Env = FMath::Clamp(HitFlashRemaining / FMath::Max(HitFlashDuration, KINDA_SMALL_NUMBER), 0.f, 1.f);
		HitFlashLight->SetIntensity(HitFlashLightIntensity * Env);
	}
	else if (HitFlashLight->IsVisible())
	{
		HitFlashLight->SetIntensity(0.f);
		HitFlashLight->SetVisibility(false);
	}
}

// ------------------------------------------------------------------ 默认格挡
void ABossCharacterBase::UpdateGuardStance()
{
	UAbilitySystemComponent* ASC = GetAbilitySystemComponent();
	if (!ASC || !Brain)
	{
		return;
	}

	const EBossState State = Brain->GetState();

	// ⚠⚠ 举盾窗口期间本函数【完全不插手】（2026-10-03 实测订正）：
	//   窗口里的 State_Blocking 是 TryGuardIncomingHit 判定成功后自己挂的，
	//   这里若再按 bGuardActive 的边沿逻辑摘一次，刚举起的盾会立刻被收掉。
	if (bGuardWindowActive)
	{
		bGuardActive = false;
		return;
	}

	// ⚠⚠⚠ 常驻举盾只在【站着不动(Idle)】时挂（2026-10-03 实测订正）
	//
	// 原来写的是 Idle / Chase / Reposition 三态都挂 —— 而 BOSS 在战斗中绝大多数时间
	// 正是 Chase（追击）→ 于是 State_Blocking 几乎常驻 → 实测 10 次玩家命中挡掉 7 次。
	//   ① 违反需求"不要太频繁"：玩家几乎打不进伤害，体感是"BOSS 太肉"而不是"会格挡"；
	//   ② 更要命的是这些格挡全走 ApplyHit 的 bHeldGuardBlock 分支，
	//      【根本不会调用 TryGuardIncomingHit】→ 举盾蒙太奇一次都不播 →
	//      玩家看不到任何"它挡了"的表现，只能得出"BOSS 不会格挡"。
	// 现在追击/走位期间的格挡完全交给概率判定（有蒙太奇、有护体光），看得见。
	const bool bShouldGuard = bGuardByDefault && !IsDead()
		&& (State == EBossState::Idle)
		&& !ASC->HasMatchingGameplayTag(DemoTag::State_Broken)
		&& !ASC->HasMatchingGameplayTag(DemoTag::State_Stagger);

	// tag 只在状态边沿切换，避免每帧重复 Add/Remove
	if (bShouldGuard != bGuardActive)
	{
		bGuardActive = bShouldGuard;
		if (bShouldGuard)
		{
			ASC->AddLooseGameplayTag(DemoTag::State_Blocking);
		}
		else
		{
			ASC->RemoveLooseGameplayTag(DemoTag::State_Blocking);
			// 举盾中断 = 压制结束，计数清零（否则下次一挡就立刻反制，玩家会觉得莫名其妙）
			BlockedStreak = 0;
		}
	}

	// 举盾姿态：只有"站着(Idle)举盾"才播全身循环蒙太奇（Block_Hold）；
	// 移动中(Chase/Reposition)不抢腿的位移，那时靠轮廓+护体光表达"在防御"。
	// 下面这段"没在播就重播"是双保险：既兜住"被别的蒙太奇顶掉"，也兜住
	// "蒙太奇资产的循环开关没生效、放完一遍就停"的情况 —— 无论如何举盾期间姿态一直在。
	if (bGuardActive)
	{
		const bool bWantPose = (State == EBossState::Idle) && GuardMontage != nullptr;
		UAnimInstance* AnimInstance = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr;
		const bool bStillPlaying = AnimInstance && GuardMontage
			&& AnimInstance->Montage_IsPlaying(GuardMontage);
		if (bWantPose && !bStillPlaying)
		{
			PlayGuardPose();
		}
		else if (!bWantPose && bGuardPosePlaying)
		{
			StopGuardPose();
		}
	}
	else if (bGuardPosePlaying)
	{
		StopGuardPose();
	}
}

// 加法动画体检函数在文件后半部分定义（第 ~1696 行），这里先声明，方便前面的函数使用。
static bool IsAdditiveAnim(const UObject* Asset);

void ABossCharacterBase::PlayGuardPose()
{
	if (!GuardMontage)
	{
		bGuardPosePlaying = false;
		return;
	}

	USkeletalMeshComponent* MeshComp = GetMesh();
	if (!MeshComp || !MeshComp->GetAnimInstance())
	{
		return;
	}

	// 已经在播就不重播（重播会把进度清零，姿势会跳一下）
	UAnimSingleNodeInstance* SingleNode = MeshComp->GetSingleNodeInstance();
	if (SingleNode && SingleNode->GetAnimationAsset() == GuardMontage)
	{
		bGuardPosePlaying = true;
		return;
	}

	// ⚠ 加法动画会被单节点直接拒绝（画面定格），比"不播举盾姿势"还糟 —— 所以先体检一次。
	// 实测 Boss_Guard_Idle_Montage 就是加法的（引擎警告：Setting an additive animation ... is not allowed）。
	if (IsAdditiveAnim(GuardMontage))
	{
		// 只打一次，避免每帧刷屏；举盾的表现交给护体光（UpdateGuardVisual）承担
		if (!bGuardAdditiveWarned)
		{
			bGuardAdditiveWarned = true;
			UE_LOG(LogDemoCombat, Error,
				TEXT("[Boss][动画] 举盾蒙太奇 %s 是【加法动画】，单节点拒绝播放 → 跳过它，举盾改用护体光表现（画面不会定格）"),
				*GuardMontage->GetName());
		}
		// 保持底层待机/行走，不要停在怪异的姿势上
		return;
	}

		// BOSS 在移动 → 不要举盾姿势。
		// ⚠⚠⚠ 2026-10-02 第二十轮修复（这是"永远只播前冲"的最后一块拼图）
		// 举盾姿势走的是 `MeshComp->PlayAnimation(GuardMontage, true)`，
		// 它会把【单节点的 CurrentAsset 抢过去】，于是：
		//   ① 行走蒙太奇被顶掉（Montage_Play 的结果被单节点覆盖 → 画面只剩举盾）
		//   ② 实测日志：所有"速度=380"的采样，蒙太奇那一栏全是 Boss_Guard_Idle_Montage
		//      → 移动时根本没有行走动画，只有一个举盾的循环蒙太奇。
		// 所以 BOSS 一旦在动就必须放弃举盾姿势（走路动画优先）。
	const bool bMovingNow = DebugPhysMoving || DebugMoveIntent;
	if (bMovingNow)
	{
		StopGuardPose();
		return;
	}

	// ⚠ 不能走 AnimInstance->Montage_Play：单节点下只有"被设为 CurrentAsset"的资产才会渲染，
	//   而且底层待机/行走改成播蒙太奇后不再注册蒙太奇槽位，Montage_Play 会静默不可见
	//   （和攻击用同一套通道，见 PlayCombatMontage 的注释）。
	//   举盾是循环姿势，所以 bLoop=true。
	MeshComp->PlayAnimation(GuardMontage, /*bLoop=*/true);
	bGuardPosePlaying = true;
}

void ABossCharacterBase::StopGuardPose()
{
	if (bGuardPosePlaying)
	{
		bGuardPosePlaying = false;

		// ⚠⚠ 收刀时必须【真的停掉】举盾蒙太奇（2026-10-03 修正）。
		//
		// 原来只把 bGuardPosePlaying 置 false 就完事，注释写着"下一帧 UpdateBossLocomotion
		// 发现 CurrentAsset 是盾牌蒙太奇就会重新播回待机/行走" —— 这个假设是错的：
		//   举盾姿势走的是 PlayAnimation（= 抢走 CurrentAsset），而 loco 判"要不要重挂"
		//   看的是【目标有没有变】，目标没变它就不重挂 → 于是 BOSS 会【举着盾平移】。
		//   （这正是 Memory 里"所有速度=380 的采样蒙太奇那栏全是 Boss_Guard_Idle_Montage"那条日志的成因。）
		// 现在：停掉蒙太奇 + 置空 CurrentLocoMontage，双管齐下，下一帧必定重挂行走。
		// ⚠ 只在"刚刚还在举盾"的这一次执行（外层有 bGuardPosePlaying 守卫），
		//   不会每帧重置步态。
		if (USkeletalMeshComponent* MeshPtr = GetMesh())
		{
			if (UAnimInstance* AI = MeshPtr->GetAnimInstance())
			{
				if (GuardMontage)
				{
					AI->Montage_Stop(0.1f, GuardMontage.Get());
				}
			}
		}
		CurrentLocoMontage = nullptr;
	}
}

void ABossCharacterBase::UpdateGuardVisual(float DeltaTime)
{
	USkeletalMeshComponent* MeshPtr = GetMesh();

	// ---- 护体光 ----
	if (GuardLight)
	{
		if (bGuardActive)
		{
			GuardVisualTime += DeltaTime;

			// 极慢的呼吸（0.7Hz）：举盾是"持续状态"，闪快了会被误读成起手预警
			const float Pulse = 0.75f + 0.25f * FMath::Sin(static_cast<float>(GuardVisualTime * 0.7f * UE_TWO_PI));

			// 被挡的瞬间额外爆一下亮（0.25s 衰减），给"叮"的一下视觉落点
			float Boost = 1.f;
			if (GuardImpactFlashRemaining > 0.f)
			{
				GuardImpactFlashRemaining -= DeltaTime;
				Boost = 1.f + 3.f * FMath::Clamp(GuardImpactFlashRemaining / 0.25f, 0.f, 1.f);
			}

			GuardLight->SetAttenuationRadius(GuardLightRadius);
			GuardLight->SetIntensity(GuardLightIntensity * Pulse * Boost);
			if (!GuardLight->IsVisible())
			{
				GuardLight->SetVisibility(true);
			}
		}
		else if (GuardLight->IsVisible())
		{
			GuardLight->SetIntensity(0.f);
			GuardLight->SetVisibility(false);
			GuardVisualTime = 0.f;
			GuardImpactFlashRemaining = 0.f;
		}
	}

	// ---- 冷蓝轮廓 ----
	if (!GuardMID || !MeshPtr)
	{
		return;
	}

	if (bGuardActive)
	{
		bGuardVisualApplied = true;
		RefreshBossOverlay();
		GuardMID->SetScalarParameterValue(TEXT("Flash"), 1.f);
		GuardMID->SetScalarParameterValue(TEXT("Intensity"), GuardOverlayIntensity);
	}
	else if (bGuardVisualApplied)
	{
		bGuardVisualApplied = false;
		RefreshBossOverlay();
	}
}

void ABossCharacterBase::OnBlockedIncomingHit_Implementation(AActor* Attacker)
{
	++BlockedStreak;

	// 火花：必须让玩家看得见"这一刀被挡住了"。
	// 否则玩家只看到伤害数字变小，会读成"BOSS 血厚 / 我的攻击没生效"，而不是"它在防御"。
	if (UWorld* World = GetWorld())
	{
		const FVector ToAttacker = Attacker
			? (Attacker->GetActorLocation() - GetActorLocation()).GetSafeNormal2D()
			: FVector::ForwardVector;
		const FVector GuardPoint = GetActorLocation() + FVector(0.f, 0.f, 95.f) + ToAttacker * 40.f;

		// 同 SpawnBossHitSpark：爆闪球的材质编译失败 → 灰球，默认关掉（2026-09-29）。
		// "被挡住了"的反馈改由『冷蓝覆盖材质 GuardMID + 护体光爆闪 + 撞击音』承担。
		if (bBossImpactBurstEnabled)
		{
			ADemoImpactBurst::SpawnBurst(World, GuardPoint, ToAttacker,
				FLinearColor(0.55f, 0.78f, 1.f, 1.f),   // 冷蓝 = 防御，和受击的白/橙区分开
				60.f, 0.20f, /*bSpawnLight=*/false);
		}

		// 撞击音：听觉是独立于视觉的第二条通道 ——
		// 玩家盯着 BOSS 脚下看的时候，"叮"的一声仍然能告诉他"被挡了"。
		if (BlockSound)
		{
			UGameplayStatics::PlaySoundAtLocation(World, BlockSound, GuardPoint);
		}
	}

	// 护体光爆一下：让"被挡"这一个瞬间在 BOSS 身上有明确的落点
	GuardImpactFlashRemaining = 0.25f;

	if (BlockedStreak >= DemoBalance::BossCounterAfterBlocks && Brain)
	{
		if (Brain->RequestRevengeAttack())
		{
			BlockedStreak = 0;

			// 反制是稀有事件（每 3 次格挡最多一次），直接打日志，不套 CVar 开关
			UE_LOG(LogDemoCombat, Warning,
				TEXT("[Boss] 连续挡下 %d 次 → 反制还手（别站桩，去找它的出招/后摇窗口）"),
				DemoBalance::BossCounterAfterBlocks);
		}
	}
}

// ------------------------------------------------------------------ 失衡绿轮廓
void ABossCharacterBase::UpdateUnstableVisual(float DeltaTime)
{
	if (!UnstableMID)
	{
		return;
	}

	const bool bUnstable = AbilitySystemComponent
		&& AbilitySystemComponent->HasMatchingGameplayTag(DemoTag::State_Unstable)
		&& !IsDead();

	USkeletalMeshComponent* MeshPtr = GetMesh();

	if (!bUnstable)
	{
		if (bUnstableOverlayApplied)
		{
			bUnstableOverlayApplied = false;
			RefreshBossOverlay();
		}
		UnstableVisualTime = 0.f;
		return;
	}

	if (!MeshPtr)
	{
		return;
	}

	if (!bUnstableOverlayApplied)
	{
		UnstableVisualTime = 0.f;

		UE_LOG(LogDemoCombat, Warning,
			TEXT("[Boss] 冲击异常条打满 → %s 进入失衡（绿轮廓 + 受伤加成 %.2fx，持续 %.1fs）"),
			*GetName(), DemoBalance::UnstableDamageBonus, DemoBalance::UnstableDuration);
	}

	// 只置标记、真正挂载交给 RefreshBossOverlay（起手红光优先级更高，不会再顶掉绿轮廓）
	bUnstableOverlayApplied = true;
	RefreshBossOverlay();

	// 缓慢呼吸：失衡是一个"状态"而不是"一次冲击"，闪太急会让人以为又要出招了
	// （急闪是起手预警的视觉语言，两者必须区分开）
	UnstableVisualTime += DeltaTime;
	const float Pulse = 0.55f + 0.45f * FMath::Sin(static_cast<float>(UnstableVisualTime * 1.6f * UE_TWO_PI));
	UnstableMID->SetScalarParameterValue(TEXT("Flash"), Pulse);
	UnstableMID->SetScalarParameterValue(TEXT("Intensity"), UnstableGlowIntensity * Pulse);
}

// ------------------------------------------------------------------ 起手预警红光
void ABossCharacterBase::UpdateTelegraphFlash(float DeltaTime)
{
	const bool bTelegraphing = bTelegraphFlashEnabled && Brain && Brain->IsTelegraphing() && !IsDead();

	if (!bTelegraphing)
	{
		bWasTelegraphing = false;
		TelegraphFlashTime = 0.f;

		if (TelegraphFlashLight && TelegraphFlashLight->IsVisible())
		{
			TelegraphFlashLight->SetIntensity(0.f);
			TelegraphFlashLight->SetVisibility(false);
		}

		if (bTelegraphOverlayApplied)
		{
			bTelegraphOverlayApplied = false;
			RefreshBossOverlay();
		}
		return;
	}

	// ---- 刚进入起手：只保留"身上的红光 / 红轮廓"作为起点提示 ----
	//
	// 2026-09-29：这里原来会在 BOSS 头部高度（角色中心 +90cm ≈ 220cm）炸一个【爆闪球】。
	//   那个球用的材质 M_BossSpriteAdd 在 PCD3D_SM6 上编译失败 → 引擎回退成默认材质 →
	//   看起来就是"BOSS 头上顶着一个黑球"（用户明确要求删掉）。
	//   起手预警的"读招"信息本来就由【红光点光源 + 全身红轮廓】承担，
	//   去掉这个球不损失信息，所以直接删掉这次 SpawnBurst。
	if (!bWasTelegraphing)
	{
		bWasTelegraphing = true;
		TelegraphFlashTime = 0.f;
	}

	// ---- 越接近出招闪得越急：不看预警条也能凭节奏读招 ----
	const float Progress = FMath::Clamp(Brain->GetTelegraphProgress(), 0.f, 1.f);
	const float Hz = FMath::Lerp(DemoBalance::BossTelegraphFlashHzStart,
		DemoBalance::BossTelegraphFlashHzEnd, Progress);
	TelegraphFlashTime += DeltaTime;
	const float Pulse = 0.5f + 0.5f * FMath::Sin(static_cast<float>(TelegraphFlashTime * Hz * UE_TWO_PI));

	if (TelegraphFlashLight)
	{
		// ⚠ 半径【不在这里设】：UpdateFlashLightPositions 每帧按躯干尺寸算"贴身半径"，
		//   在这里再写一次会把它覆盖回 TelegraphFlashLightRadius(1000) = 10 米，
		//   红光就会重新变成罩住人物的一大坨（这正是"不贴合模型"的成因）。
		//   同理强度改用贴身档：半径从 7~10 米收到 1~2 米后，强度必须按平方缩，
		//   否则会变成"贴脸爆白"。
		TelegraphFlashLight->SetIntensity(TelegraphLightFitIntensity * Pulse);
		if (!TelegraphFlashLight->IsVisible())
		{
			TelegraphFlashLight->SetVisibility(true);
		}
	}

	if (TelegraphFlashMID)
	{
		// 只置标记、真正挂载交给 RefreshBossOverlay（三个状态光抢同一个 overlay 槽）
		bTelegraphOverlayApplied = true;
		RefreshBossOverlay();

		// ⚠⚠ 强度必须压得很低（2026-10-04）。玩家截图里那团"比 BOSS 还大的红色物体"
		// 就是本层烧过头：原来 Intensity = 1 + Pulse×5（最高 6），而材质 M_BossFlashOverlay
		// 里还有 0.4 的"整身保底亮度" → 正面实打实 2.4 的加法发光 → BOSS 整个被烧成
		// 一片亮红，再被 Bloom 糊成一团红雾，看起来完全不像"贴着模型"。
		//
		// Overlay 是逐像素作用在网格上的，物理上不可能跑到模型外面 ——
		// 所以"看起来不贴合"只有一个解释：【太亮太糊，把模型本身吞掉了】。
		//
		// ⚠ 材质里的 Fresnel 权重（0.06 + 0.94·F）已经改在 Tools/SetupBossVFX.py 里，
		//   但【编辑器开着时 .uasset 被锁、脚本写不进去】(Error Code 32)，
		//   所以那一步还没生效。这里的系数是按【旧材质(0.4/0.6)】反推的：
		//   峰值 0.6 → 正面 0.24 / 边缘 0.36，BOSS 本体照常看得清，只有轮廓在烧。
		//   等材质重建成功（关掉编辑器后跑 Tools/SetupBossOverlayMaterial.py），
		//   可以把这里再往上调一档。
		TelegraphFlashMID->SetScalarParameterValue(TEXT("Flash"), 0.65f + Pulse * 0.35f);
		TelegraphFlashMID->SetScalarParameterValue(TEXT("Intensity"), 0.15f + Pulse * 0.45f);
	}
}

// ------------------------------------------------------------------ 状态光仲裁
//
// 预警红光 / 失衡绿轮廓 / 格挡冷蓝轮廓 共用 Mesh 的【同一个 OverlayMaterial 槽】。
// 之前三条通道各自 SetOverlayMaterial，谁最后跑谁赢：起手红光会把失衡绿轮廓整个顶掉，
// 于是"绿光时有时无"，读起来就是"光不贴在模型上"。
// 现在统一在这里按优先级挂载：起手红 > 失衡绿 > 格挡蓝。
void ABossCharacterBase::RefreshBossOverlay()
{
	USkeletalMeshComponent* MeshPtr = GetMesh();
	if (!MeshPtr)
	{
		return;
	}

	UMaterialInstanceDynamic* Desired = nullptr;
	if (bTelegraphOverlayApplied && TelegraphFlashMID)
	{
		Desired = TelegraphFlashMID;
	}
	else if (bUnstableOverlayApplied && UnstableMID)
	{
		Desired = UnstableMID;
	}
	else if (bGuardVisualApplied && GuardMID)
	{
		Desired = GuardMID;
	}

	if (Desired != AppliedOverlayMID)
	{
		MeshPtr->SetOverlayMaterial(Desired);
		AppliedOverlayMID = Desired;
	}
}

// ------------------------------------------------------------------ 灯位对准模型（预警红光 / 受击白光）
//
// ⚠⚠⚠ 这个函数此前只存在于注释里、从未被实现（2026-10-02 第二十一轮补上）。
//
// 症状：预警红光和 BOSS 模型【错位】——红光只照亮脚部 / 偏在一旁。
// 原因：两个点光源都挂在 Mesh 组件上，而 `SetupAttachment(GetMesh())` +
// `SetRelativeLocation(FVector::ZeroVector)` = 灯位 = **Mesh 组件原点**。
// SkeletalMesh 的组件原点在**脚底（胶囊底部）**，不在身体中间。
// 于是点光源实际就在 BOSS 脚边 —— 离得太远还容易超出衰减半径，红光糊在脚下。
//
// 正解：每帧按【当前姿势的真实骨骼范围】算出躯干中心，把灯移到那里。
// 用 Mesh->Bounds 是骨骼包围盒（已含当前姿势的动画形变），比固定高度准。
void ABossCharacterBase::UpdateFlashLightPositions(float DeltaTime)
{
	USkeletalMeshComponent* MeshPtr = GetMesh();
	if (!MeshPtr)
	{
		return;
	}

	// 躯干中心：优先用【上半身骨骼】的平均位置。
	// 直接用 Bounds.Origin 也行，但它会被武器 / 挂件拉偏（BOSS 的剑很长），
	// 所以这里用 spine / chest 自己算，稳。
	//
	// ⚠ 2026-10-04：原来这张表里【包含 pelvis（骨盆）】。骨盆是下半身最靠下的骨，
	//   混进平均后灯心被拉到腰腿高度（实测灯心 Z 只有 51cm），
	//   于是红光主要照在腿上而不是胸腔 —— 位置一偏，用户看到的还是"光没贴在身上"。
	//   现在只取胸椎 / 胸骨，再按【上半身高度】把灯心顶到胸腔。
	const float BodyHalfHeight = FMath::Max(MeshPtr->Bounds.BoxExtent.Z, 40.f);
	FVector Center = MeshPtr->Bounds.Origin;
	{
		static const FName CoreBones[] = {
			TEXT("spine_01"), TEXT("spine_02"), TEXT("spine_03"),
			TEXT("chest"),
			TEXT("spine"), TEXT("spine1"), TEXT("spine2") };
		FVector Sum = FVector::ZeroVector;
		int32 N = 0;
		for (const FName B : CoreBones)
		{
			if (MeshPtr->GetBoneIndex(B) != INDEX_NONE)
			{
				Sum += MeshPtr->GetBoneLocation(B, EBoneSpaces::WorldSpace);
				++N;
			}
		}
		if (N > 0)
		{
			Center = Sum / (float)N;
		}
		// 顶到胸腔：+0.18 × 躯干半高。胸椎平均大约在肚脐高度，当灯心正好。
		Center.Z += BodyHalfHeight * 0.18f;
	}

	// 灯挂在 Mesh 上，所以要换算回【相对 Mesh 的局部坐标】
	const FVector LocalTarget = MeshPtr->GetComponentTransform().InverseTransformPosition(Center);

	// ⚠⚠ 半径必须【贴身】（2026-10-04，用户反馈"红光不贴合模型"）
	//
	// 点光源是"照亮范围内所有表面"的：半径一大，BOSS 周围的地面、空气都被染红，
	// 画面上就成了"一大坨红光罩在人物外面"，而不是"模型自己在发光"。
	// 真正逐像素贴合模型的是 OverlayMaterial 那一层（MI_BossFlash，Fresnel 边缘），
	// 点光只负责补一点体积感，所以半径按【躯干半高】给，不多给。
	const float FitRadius = FMath::Clamp(
		BodyHalfHeight * TelegraphLightFitScale,
		TelegraphLightFitMinRadius,
		TelegraphLightFitMaxRadius);

	// ⚠ 预警红光和受击白光都照这同一个躯干中心，保证两者观感一致
	if (TelegraphFlashLight)
	{
		TelegraphFlashLight->SetAttenuationRadius(FitRadius);
		TelegraphFlashLight->SetRelativeLocation(LocalTarget);
	}
	if (HitFlashLight)
	{
		// 受击白光比预警红光更短促，允许比红光略大一圈（要"啪"的一下），但同样不外溢
		HitFlashLight->SetAttenuationRadius(FMath::Min(FitRadius * 1.15f, TelegraphLightFitMaxRadius * 1.2f));
		HitFlashLight->SetRelativeLocation(LocalTarget);
	}

	// 诊断：把"光到底贴不贴身"变成可核对的数字，而不是靠肉眼猜
	LightDiagTimer -= DeltaTime;
	if (LightDiagTimer <= 0.f)
	{
		LightDiagTimer = 1.f;
		UE_LOG(LogDemoCombat, Log,
			TEXT("[Boss][灯位体检] 躯干半高=%.0fcm → 贴身半径=%.0fcm（旧配置是 700~1000cm 的大光球）| 灯心Z=%.0f | 红光亮=%d 强度=%.0f | 轮廓层=%s"),
			BodyHalfHeight, FitRadius, LocalTarget.Z,
			TelegraphFlashLight ? (TelegraphFlashLight->IsVisible() ? 1 : 0) : 0,
			TelegraphFlashLight ? TelegraphFlashLight->Intensity : 0.f,
			AppliedOverlayMID == TelegraphFlashMID ? TEXT("起手红")
				: AppliedOverlayMID == UnstableMID ? TEXT("失衡绿")
				: AppliedOverlayMID == GuardMID ? TEXT("格挡蓝")
				: TEXT("无"));
	}
}

// ------------------------------------------------------------------ 破防跪地锁死
//
// 破防（韧性清零）之后 BOSS 会跪地一段时间，窗口见 DemoBalance::BossPoiseBreakWeakDuration。
// 之前这段时间里它是【能平移的】：
//   · 位移安全阀按"走路速度 × 余量"（约 600cm/s）放行；
//   · 大脑的走位 / 突进残留速度没被清；
//   · 单节点 loco 会重新挂上行走蒙太奇（若虚弱蒙太奇已播完）→ 变成"跪着滑行"。
// 跪着被推着走是最难看的破绽：玩家预期是"你打空了它，它就该跪在那儿不动"。
//
// ⚠ 与死亡冻结的区别：破防会恢复，所以【不能】DisableMovement（否则起身就走不了），
//   这里只锁位置 + 每帧清速度，恢复时 MovementMode 照常。
void ABossCharacterBase::UpdateBreakFreeze(float DeltaTime)
{
	const bool bBroken = IsPoiseBroken();

	if (!bBroken)
	{
		// 起身（或还没破防）→ 锚点作废，下次破防重新记
		bBreakAnchorValid = false;
		BreakDriftTotal = 0.f;
		BreakRevertTotal = 0.f;
		BreakFreezeLogTimer = 0.f;

		// ★ 把移动模式还回去（破防锁期间存下来的那个）。
		//   先钉回锚点再还模式：MOVE_None 里攒下的"悬空"会让恢复那一帧被拽一下。
		if (bBreakSavedMoveMode)
		{
			bBreakSavedMoveMode = false;
			if (UCharacterMovementComponent* MoveComp = GetCharacterMovement())
			{
				SetActorLocation(BreakAnchorLocation, /*bSweep=*/false, nullptr, ETeleportType::None);
				MoveComp->SetMovementMode(SavedBreakMoveMode);
				UE_LOG(LogDemoCombat, Warning,
					TEXT("[Boss][破防冻结] 起身：移动模式还原为 %d（当前偏移 %.1fcm）"),
					static_cast<int32>(SavedBreakMoveMode),
					FVector::Dist(GetActorLocation(), BreakAnchorLocation));
			}
		}
		return;
	}

	if (!bBreakAnchorValid)
	{
		bBreakAnchorValid = true;
		BreakAnchorLocation = GetActorLocation();
		BreakAnchorYaw = GetActorRotation().Yaw;
		// ⚠ MovementMode 只在【进入破防的这一帧】存一次。
		//   跪地期间压力测试会不停 LaunchCharacter，而 LaunchCharacter 在 MOVE_None 下
		//   会把模式改成 MOVE_Falling —— 如果每帧都存，恢复时就永远还原成 Falling，
		//   起身瞬间 BOSS 会突然往下掉。
		if (UCharacterMovementComponent* MoveComp = GetCharacterMovement())
		{
			SavedBreakMoveMode = MoveComp->MovementMode;
			bBreakSavedMoveMode = (SavedBreakMoveMode != EMovementMode::MOVE_None);
			MoveComp->StopMovementImmediately();
			MoveComp->SetMovementMode(EMovementMode::MOVE_None);
		}
		UE_LOG(LogDemoCombat, Warning,
			TEXT("[Boss][破防冻结] 跪地锁位：锚点 (%.1f, %.1f, %.1f) 进入时移动模式=%d → 强制 MOVE_None"),
			BreakAnchorLocation.X, BreakAnchorLocation.Y, BreakAnchorLocation.Z,
			static_cast<int32>(SavedBreakMoveMode));
	}

	// 每帧都保证它是"停"的：清速度 + 整个移动组件停机。
	// 只清速度不够 —— 移动组件内部仍会做地面检测/重力，视觉上原地抖。
	if (UCharacterMovementComponent* MoveComp = GetCharacterMovement())
	{
		MoveComp->StopMovementImmediately();
		if (MoveComp->Velocity != FVector::ZeroVector)
		{
			MoveComp->Velocity = FVector::ZeroVector;
		}
		if (MoveComp->MovementMode != EMovementMode::MOVE_None)
		{
			MoveComp->SetMovementMode(EMovementMode::MOVE_None);
		}
	}

	const float Drift = FVector::Dist(GetActorLocation(), BreakAnchorLocation);
	BreakDriftTotal += Drift;
	if (Drift > 0.1f)
	{
		SetActorLocation(BreakAnchorLocation, /*bSweep=*/false, nullptr, ETeleportType::None);
		BreakRevertTotal += Drift;
	}

	// 跪着不转身
	if (!FMath::IsNearlyEqual(GetActorRotation().Yaw, BreakAnchorYaw, 0.5f))
	{
		const FRotator R = GetActorRotation();
		SetActorRotation(FRotator(R.Pitch, BreakAnchorYaw, R.Roll));
	}

	BreakFreezeLogTimer -= DeltaTime;
	if (BreakFreezeLogTimer <= 0.f)
	{
		BreakFreezeLogTimer = 1.f;
		// ⚠ 四个数要分清（判据：注入 ≈ 拉回，且移动模式恒为 0 = MOVE_None）：
		//   移动模式     = 0 说明整个移动组件已停机，它自己【不可能】产生任何位移
		//   本帧被推离   = 外部注入的那一下被测到多少（下一帧就被钉回，玩家看不见）
		//   已拉回累计   ≈ 注入累计 → 一次都没漏出去
		//   帧间漂移累计 = 上面那项的累加（只是记账用，别单独拿它当"位移"）
		UE_LOG(LogDemoCombat, Warning,
			TEXT("[Boss][破防冻结] 跪地期间 移动模式=%d | 本帧被推离=%.1fcm | 注入累计=%.1fcm 已拉回=%.1fcm | 推力 %d 次"),
			GetCharacterMovement() ? static_cast<int32>(GetCharacterMovement()->MovementMode) : -1,
			Drift, BreakTestInjectedCm, BreakRevertTotal, BreakTestPushCount);
	}
}

bool ABossCharacterBase::IsPoiseBroken() const
{
	const UAbilitySystemComponent* ASC = GetAbilitySystemComponent();
	return ASC && ASC->HasMatchingGameplayTag(DemoTag::State_Broken);
}

// ------------------------------------------------------------------ 死亡冻结
//
// OnDeathReaction 已经关了大脑 / DisableMovement / 关碰撞，但那只挡住了"主动移动"。
// 真正让尸体还会动的是每帧的这两条：
//   ① UpdateDisplacementGuard —— 它只把单帧位移夹在"走路速度 × 余量"（约 600cm/s），
//      死亡后照样放行；一旦有外力（弹反击退 / 残留 RequestDirectMove），
//      它不但不拦，还会用 SetActorLocation 把尸体一路"夹"着滑出去。
//   ② UpdateSmoothFacing —— 死了还能转身。
// 所以这里不做"判断该拦什么"，直接每帧把 Actor 硬钉回倒下那一刻的位姿：位置、朝向、速度。
void ABossCharacterBase::UpdateDeathFreeze(float DeltaTime)
{
	const bool bDeadNow = IsDead();

	if (!bDeathFrozen)
	{
		if (!bDeadNow)
		{
			return;
		}
		// 死亡（或死亡反应已经跑过）→ 记下锚点，从这一帧起冻死
		bDeathFrozen = true;
		DeathAnchorLocation = GetActorLocation();
		DeathAnchorYaw = GetActorRotation().Yaw;
		DeathDriftAccum = 0.f;
		DeathDriftMax = 0.f;
		DeathRevertTotal = 0.f;
		DeathFreezeLogTimer = 0.f;
		UE_LOG(LogDemoCombat, Warning,
			TEXT("[Boss][死亡冻结] 已锁定死亡锚点 (%.1f, %.1f, %.1f) 朝向 %.1f"),
			DeathAnchorLocation.X, DeathAnchorLocation.Y, DeathAnchorLocation.Z, DeathAnchorYaw);
	}

	if (UCharacterMovementComponent* MoveComp = GetCharacterMovement())
	{
		MoveComp->StopMovementImmediately();
		if (MoveComp->Velocity != FVector::ZeroVector)
		{
			MoveComp->Velocity = FVector::ZeroVector;
		}
		if (MoveComp->MovementMode != EMovementMode::MOVE_None)
		{
			MoveComp->DisableMovement();
		}
	}

	// ⚠ 取证用的"外力压力测试"：死亡之后仍然每 0.5 秒强行推它一次
	//   （直接平移 + 给移动组件一个直接移动请求，模拟"玩家还在打 / 残留速度"）。
	//   没有这一段的话，"死亡后位移 0"可能只是因为场上恰好没人碰它，证明不了冻结扛得住外力。
	//   开着它时日志里应该看到【累计偏移 ≈ 已拉回】（说明推得动、但每帧都被钉回去）。
	if (bKillTestFired)
	{
		DeathStressTimer -= DeltaTime;
		if (DeathStressTimer <= 0.f)
		{
			DeathStressTimer = 0.5f;
			const FVector PushDir = FVector(0.8f, 0.6f, 0.f).GetSafeNormal();
			AddActorWorldOffset(PushDir * 120.f, /*bSweep=*/false, nullptr, ETeleportType::None);
			if (UCharacterMovementComponent* MoveComp3 = GetCharacterMovement())
			{
				MoveComp3->RequestDirectMove(PushDir * 400.f, /*bForceMaxSpeed=*/true);
			}
		}
	}

	// 只有真的偏离锚点才写回，避免每帧无意义的 SetActorLocation 广播
	const float Drift = FVector::Dist(GetActorLocation(), DeathAnchorLocation);
	DeathDriftAccum += Drift;
	DeathDriftMax = FMath::Max(DeathDriftMax, Drift);

	if (Drift > 0.1f)
	{
		SetActorLocation(DeathAnchorLocation, /*bSweep=*/false, nullptr, ETeleportType::None);
		DeathRevertTotal += Drift;
	}
	if (!FMath::IsNearlyEqual(GetActorRotation().Yaw, DeathAnchorYaw, 0.5f))
	{
		const FRotator R = GetActorRotation();
		SetActorRotation(FRotator(R.Pitch, DeathAnchorYaw, R.Roll));
	}

	// 取证日志：无头跑之后直接读这行
	DeathFreezeLogTimer -= DeltaTime;
	if (DeathFreezeLogTimer <= 0.f)
	{
		DeathFreezeLogTimer = 1.f;
		UE_LOG(LogDemoCombat, Warning,
			TEXT("[Boss][死亡冻结] 死亡后累计被推 %.1fcm / 单帧最大 %.2fcm / 已硬拉回 %.1fcm —— 拉回≈被推即表示【推得动但钉得住】"),
			DeathDriftAccum, DeathDriftMax, DeathRevertTotal);
	}
}

// ------------------------------------------------------------------ 忽略玩家位移（根治被推/打飞）
void ABossCharacterBase::UpdatePlayerPushImmunity()
{
	if (UCapsuleComponent* Capsule = GetCapsuleComponent())
	{
		if (APawn* Player = UGameplayStatics::GetPlayerPawn(GetWorld(), 0))
		{
			Capsule->IgnoreActorWhenMoving(Player, true);
		}
	}
}

void ABossCharacterBase::PlayMontageOnMesh(UAnimMontage* Montage, float PlayRate)
{
	if (!Montage)
	{
		return;
	}

	USkeletalMeshComponent* MeshComp = GetMesh();
	if (!MeshComp)
	{
		return;
	}

	// 单节点模式：用 PlayAnimation 设为当前资产才能渲染（Montage_Play 在单节点下不可见）。
	// 受击 / 破防 / 死亡这类被动反应不需要"播完"回调（loco 由 UpdateBossLocomotion 自动恢复）。
	if (UAnimSingleNodeInstance* SN = MeshComp->GetSingleNodeInstance())
	{
		SN->SetPlayRate(PlayRate);
	}
	MeshComp->PlayAnimation(Montage, /*bLoop=*/false);
}

// ------------------------------------------------------------------ 基础动画驱动（待机 / 移动）

// 判定一个动画资产是不是【加法动画】。
// 走 UAnimationAsset 基类指针调用（AnimationAsset.h 里 `virtual bool IsValidAdditive() const` 是 public），
// 不依赖 UAnimSequence / UAnimMontage 派生类里该函数的访问权限，最稳。
static bool IsAdditiveAnim(const UObject* Asset)
{
	const UAnimationAsset* AsAnim = Cast<UAnimationAsset>(Asset);
	return AsAnim && AsAnim->IsValidAdditive();
}

// ⚠ 关于"单节点蒙太奇为什么不循环"，记一笔（2026-10-03 查引擎源码）：
//   循环链接【不在资产上】，而在 FAnimMontageInstance 自己的 NextSections 图里。
//   · UAnimMontage::CompositeSections[].NextSectionName 只是"新建实例时的初始模板"；
//   · 真正的循环是 UAnimSingleNodeInstance::SetMontageLoop() 写进【实例】的；
//   · PlayAnimation 内部顺序是 SetAnimationAsset(bIsLooping=false) → SetMontageLoop(...,false)
//     → Play(true) → SetLooping(true) → SetMontageLoop(...,true)，理论上最后会接上。
//   所以：要补循环必须调【实例的】SetMontageLoop / SetLooping，
//   【直接改 CompositeSections 是无效的】（对已经在播的实例不起作用）。

void ABossCharacterBase::SetupBossAnimation()
{
	USkeletalMeshComponent* MeshComp = GetMesh();
	if (!MeshComp)
	{
		return;
	}

	bUsesBossAnimBP = false;

	// 可选：仅当在 BP_Boss 里显式指了 BossAnimBlueprint 才走专用 ABP（动画图需含 Slot 'DefaultSlot'）。
	// ⚠ 不要自动按约定路径去 Load，因为用户若手建的 ABP 有瑕疵（Slot 名不对 / 骨架不匹配）反而会
	//   让攻击蒙太奇继续静默失败。没显式指定时，一律走下方"单节点 + 动态待机蒙太奇"路径，
	//   该路径已在 C++ 内注册好 DefaultSlot，攻击/受击/破防/死亡蒙太奇能直接分层播出来。
	UClass* ABPClass = nullptr;
	if (BossAnimBlueprint.IsValid())
	{
		ABPClass = BossAnimBlueprint.LoadSynchronous();
	}

	if (ABPClass)
	{
		MeshComp->SetAnimInstanceClass(ABPClass);
		bUsesBossAnimBP = true;
		UE_LOG(LogDemoCombat, Log,
			TEXT("[Boss][动画] 已切换到 BOSS 专用 ABP：%s（Idle/Jog + 攻击/受击/破防/死亡蒙太奇分层）"),
			*ABPClass->GetName());
		return;
	}

	// ---- 默认路径：单节点模式 + 动态待机/行走蒙太奇（无需任何手动 ABP / Python，开箱即用）----
	// 强制单节点模式：彻底绕开 BP_Boss 上可能还指着的 Khaimera_AnimBlueprint。
	// 那是玩家角色的 ABP，内部依赖英雄状态变量 → 除以零 → 整只 BOSS 完全不动 / 永远 T-Pose。
	//
	// ⚠ 关键修复（查引擎源码确认）：单节点实例只有在 SetAnimationAsset 的是【蒙太奇】时才会注册
	//   该蒙太奇自带的 Slot 节点；之前用"序列"当待机（序列没有 Slot）→ DefaultSlot 永远不存在 →
	//   GAS 攻击/受击/破防/死亡 Montage_Play 找不到 Slot → 静默失败（BOSS 攻击不播动画）。
	//   现在把待机/行走也做成【挂在 DefaultSlot 的循环动态蒙太奇】，SetAnimationAsset 时注册好
	//   DefaultSlot，攻击蒙太奇就能正确分层播出来 —— 全程 C++ 完成，不需要建 ABP、不需要 Python。
	MeshComp->SetAnimationMode(EAnimationMode::AnimationSingleNode);

	// 待机 / 行走：蓝图没配就按约定路径兜底。
	//
	// ⚠⚠ 关键坑（2026-09-29 查资产定案）：
	//   Khaimera 包里 Idle_Relaxed_A / Idle / Idle_Relaxed_B **全是加法动画**
	//   （AdditiveAnimType = AAT_LocalSpaceBase）。而单节点实例【拒绝设置加法动画】，
	//   引擎每帧刷 "Setting an additive animation (AnimMontage_0) on an AnimSingleNodeInstance ... is not allowed"
	//   → 待机压根播不出来 → BOSS 一直停在参考姿势（T/A-Pose），既没待机、也没走路动画
	//   （就是用户看到的"BOSS 像一团发白的影子站着不动"）。
	//   → 必须用【非加法】的待机：Idle_NonAdditive（其次 HS_Idle_Relaxed_A / HS_Idle）。
	if (!BossIdleAnimation || IsAdditiveAnim(BossIdleAnimation))
	{
		static const TCHAR* IdleCandidatePaths[] =
		{
			TEXT("/Game/ParagonKhaimera/Characters/Heroes/Khaimera/Animations/Idle_NonAdditive.Idle_NonAdditive"),
			TEXT("/Game/ParagonKhaimera/Characters/Heroes/Khaimera/Animations/HS_Idle_Relaxed_A.HS_Idle_Relaxed_A"),
			TEXT("/Game/ParagonKhaimera/Characters/Heroes/Khaimera/Animations/HS_Idle.HS_Idle"),
		};

		for (const TCHAR* CandidatePath : IdleCandidatePaths)
		{
			UAnimSequence* Candidate = Cast<UAnimSequence>(
				StaticLoadObject(UAnimSequence::StaticClass(), nullptr, CandidatePath));
			if (Candidate && !IsAdditiveAnim(Candidate))
			{
				BossIdleAnimation = Candidate;
				break;
			}
		}
	}

	// 行走：Jog_Fwd 本身是非加法（已核实），但还是体检一遍，防止换成加法版本后静默失效
	if (!BossJogAnimation || IsAdditiveAnim(BossJogAnimation))
	{
		UAnimSequence* Jog = Cast<UAnimSequence>(StaticLoadObject(
			UAnimSequence::StaticClass(), nullptr,
			TEXT("/Game/ParagonKhaimera/Characters/Heroes/Khaimera/Animations/Jog_Fwd.Jog_Fwd")));
		BossJogAnimation = (Jog && !IsAdditiveAnim(Jog)) ? Jog : nullptr;
	}

	// 方向化行走：前后左右四向 Jog（绕圈 / 后撤时不再全程播前冲，否则看着像溜冰/倒滑）。
	// 任一素材缺失或加法 → 留空，运行时 PickLocoJogSequence 会回退到 BossJogAnimation（前冲）。
	auto LoadDirJog = [&](const TCHAR* Path) -> UAnimSequence*
	{
		UAnimSequence* Seq = Cast<UAnimSequence>(StaticLoadObject(UAnimSequence::StaticClass(), nullptr, Path));
		return (Seq && !IsAdditiveAnim(Seq)) ? Seq : nullptr;
	};
	BossJogBwdAnimation   = LoadDirJog(TEXT("/Game/ParagonKhaimera/Characters/Heroes/Khaimera/Animations/Jog_Bwd.Jog_Bwd"));
	BossJogLeftAnimation  = LoadDirJog(TEXT("/Game/ParagonKhaimera/Characters/Heroes/Khaimera/Animations/Jog_Left.Jog_Left"));
	BossJogRightAnimation = LoadDirJog(TEXT("/Game/ParagonKhaimera/Characters/Heroes/Khaimera/Animations/Jog_Right.Jog_Right"));

	// ⚠⚠ "脚速基准"取值（消滑步的核心）
	//
	// 问题：BOSS 胶囊体靠 DriveMoveNoNav 写速度平移（移速 = MaxWalkSpeed，默认 380cm/s），
	//   而 Jog 动画在【单节点 + RootMotionFromMontagesOnly】下是【原地播】的，脚按动画自己的
	//   "原生地面速度"在跑。两者不等 → 脚在跑、地没动（或反过来）→ 画面"滑步"。
	//   所以播放倍速必须 = 实际移速 / 动画原生地面速度，让脚向后蹬的速度恰好等于胶囊平移速度。
	//
	// 取原生地面速度的首选办法是读根位移：Paragon 的部分 Jog 烘焙了根位移，ExtractRootMotion 能拿到。
	// 但本项目这套 Jog 是【纯原地】(enable_root_motion=False)，根位移为 0，此法失效 → 落到 else 分支，
	// 沿用头文件里测好的默认值 370 cm/s（用 Tools/MeasureJogFootSpeed.py 量脚骨支撑相向后速度得到）。
	// 若以后换成带根位移的 Jog，下面这段会自动覆盖为本动画的真实数值，无需手改。
	if (BossJogAnimation)
	{
		const float JogDur = BossJogAnimation->GetPlayLength();
		if (JogDur > 0.01f)
		{
			// ExtractRootMotionFromRange 读动画里烘焙好的根位移；5.6 起用 3 参版（带 FAnimExtractContext）。
			const FAnimExtractContext ExtractCtx;
			const FTransform RootMotion = BossJogAnimation->ExtractRootMotionFromRange(0.0, static_cast<double>(JogDur), ExtractCtx);
			const FVector2D Horiz(RootMotion.GetTranslation().X, RootMotion.GetTranslation().Y);
			const float NativeGroundSpeed = Horiz.Size() / JogDur;
			if (NativeGroundSpeed > 1.f)
			{
				BossJogReferenceSpeed = NativeGroundSpeed;
				UE_LOG(LogDemoCombat, Log,
					TEXT("[Boss][动画] 自校准 Jog 脚速基准：原生地面速度=%.0f cm/s（时长 %.2fs）→ 播放倍速随实际移速缩放，消除滑步"),
					NativeGroundSpeed, JogDur);
			}
			else
			{
				UE_LOG(LogDemoCombat, Log,
					TEXT("[Boss][动画] Jog_Fwd 根位移为 0（纯原地动画）→ 沿用实测默认 %.0f cm/s（Tools/MeasureJogFootSpeed.py 测得）；如仍滑步重测该值"),
					BossJogReferenceSpeed);
			}
		}
	}

	// 把序列包成"挂在 DefaultSlot 的循环蒙太奇"。LoopCount=0 = 无限循环。
	// 这样 SetAnimationAsset(待机蒙太奇) 时引擎会注册 DefaultSlot 节点，
	// 攻击/受击/破防/死亡这些也是 DefaultSlot 的蒙太奇就能盖在上面分层播放。
	if (BossIdleAnimation)
	{
		BossIdleMontage = UAnimMontage::CreateSlotAnimationAsDynamicMontage(
			BossIdleAnimation, TEXT("DefaultSlot"), 0.25f, 0.25f, 1.f, /*LoopCount*/ 0);
	}
	if (BossJogAnimation)
	{
		BossJogMontage = UAnimMontage::CreateSlotAnimationAsDynamicMontage(
			BossJogAnimation, TEXT("DefaultSlot"), 0.25f, 0.25f, 1.f, /*LoopCount*/ 0);
	}

	// ⚠⚠⚠ BOSS 的动画必须【无条件】tick，且必须【刷新骨骼】（2026-10-02 定案，第三次改这个值）
	//
	// 枚举语义（SkinnedMeshComponent.h）—— 名字很容易骗人：
	//   AlwaysTickPoseAndRefreshBones(0)：不管渲不渲染，都 tick 姿势【并刷新骨骼变换】
	//   AlwaysTickPose(1)              ：都 tick 姿势，但【只有被渲染时才刷新骨骼】
	//   其余两个更弱（只 tick 蒙太奇 / 只在被渲染时 tick）
	//
	// 之前设的是 AlwaysTickPose(1)，以为"无条件 tick"就够 —— 大错：
	// 它只保证【求值姿势】，不保证【把骨骼变换写回组件】。一旦没被渲染（背身、远处、
	// 被遮挡，或无头 -NullRHI 环境），骨骼变换就停在参考姿势：
	//   实测脚骨 foot_l 的世界 Z 被钉在 18cm、Mesh 空间位移恒为 0.0cm，
	//   而世界位移恰好等于身体位移（190cm/0.5s = 380cm/s）——
	//   也就是"腿一动不动地跟着身体平移"，正是玩家说的"没有动画、只是模型平移"。
	// 单节点下没有 AnimBP 兜底，骨骼不刷新 = 画面完全不更新。
	//
	// 正确值就是 AlwaysTickPoseAndRefreshBones。BOSS 是战斗核心对象，这点开销可接受。
	// （UE 5.7 没有 setter，直接改属性；MEMORY 里"设 AlwaysTickPose 即可"的旧结论是错的，已订正。）
	MeshComp->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;

	// ---------------- 动画体检：找出"加法动画" ----------------
	// 单节点实例【拒绝加法动画】（引擎只打一行 LogAnimation 警告就完事），
	// 被拒绝的蒙太奇画面上会定格/像 T-Pose —— 玩家看到的就是"这招没有动作"。
	// 实测抓到的第一个受害者就是举盾蒙太奇 Boss_Guard_Idle_Montage。
	// 这里开局一次性全查一遍，把"哪些招会看不见"直接列出来，省得靠肉眼去猜。
	{
		struct FCheck { const TCHAR* Label; const UAnimMontage* Montage; };
		const FCheck Checks[] = {
			{ TEXT("举盾"),   GuardMontage },
			{ TEXT("受击"),   HitReactMontage },
			{ TEXT("虚弱"),   WeakMontage },
			{ TEXT("虚弱循环"), WeakLoopMontage },
			{ TEXT("虚弱恢复"), WeakRecoverMontage },
			{ TEXT("硬直"),   StaggerMontage },
			{ TEXT("死亡"),   DeathMontage },
		};
		for (const FCheck& Item : Checks)
		{
			if (Item.Montage && IsAdditiveAnim(Item.Montage))
			{
				UE_LOG(LogDemoCombat, Error,
					TEXT("[Boss][动画体检] 「%s」蒙太奇 %s 是【加法动画】→ 单节点拒绝播放，画面上会定格/怪异。")
					TEXT("需换成非加法素材重做该蒙太奇（Khaimera 包里 Idle_* 很多都是加法的，别用）"),
					Item.Label, *Item.Montage->GetName());
			}
		}
	}

	// ⚠⚠ 底层待机/行走【直接播序列】，不再包成循环蒙太奇（2026-09-30 定案）
	//   原因见 CurrentLocoAsset 的注释：单节点下循环蒙太奇的时间不推进，
	//   而非循环蒙太奇（所有攻击/受击）正常 —— 序列循环是单节点最原始的通道，最稳。
	if (BossIdleAnimation)
	{
		MeshComp->PlayAnimation(BossIdleAnimation, /*bLoop=*/true);
		CurrentLocoAsset = BossIdleAnimation;
		bBossLocoPlayingJog = false;
		UE_LOG(LogDemoCombat, Log,
			TEXT("[Boss][动画] 单节点兜底：待机序列 %s（Jog=%s）"),
			*BossIdleAnimation->GetName(),
			BossJogAnimation ? *BossJogAnimation->GetName() : TEXT("未找到"));
	}
	else
	{
		UE_LOG(LogDemoCombat, Warning,
			TEXT("[Boss][动画] 没找到可用待机序列（非加法素材缺失）—— BOSS 可能仍是 T-Pose（检查 Khaimera 是否导入成功）"));
	}
}

void ABossCharacterBase::DebugDumpAnimState(float DeltaTime)
{
	AnimDebugTimer += DeltaTime;
	if (AnimDebugTimer < 0.5f)
	{
		return;
	}
	AnimDebugTimer = 0.f;

	USkeletalMeshComponent* MeshComp = GetMesh();
	UAnimInstance* AnimInstance = MeshComp ? MeshComp->GetAnimInstance() : nullptr;
	if (!MeshComp || !AnimInstance)
	{
		UE_LOG(LogDemoCombat, Warning, TEXT("[Boss][anim自检] 没有 Mesh / AnimInstance"));
		return;
	}

	// ---------- 1) 单节点：当前资产 + 播放时间有没有在推进 ----------
	const UAnimSingleNodeInstance* SingleNode = MeshComp->GetSingleNodeInstance();
	const UAnimationAsset* NodeAsset = SingleNode ? SingleNode->GetAnimationAsset() : nullptr;
	const float NodeTime = SingleNode ? SingleNode->GetCurrentTime() : -1.f;
	const float NodeTimeDelta = NodeTime - LastDebugNodeTime;
	LastDebugNodeTime = NodeTime;

	// ---------- 2) GAS 蒙太奇（攻击 / 受击）有没有在播 ----------
	const UAnimMontage* ActiveMontage = AnimInstance->GetCurrentActiveMontage();
	const bool bMontagePlaying = ActiveMontage && AnimInstance->Montage_IsPlaying(ActiveMontage);
	const float MontagePos = bMontagePlaying ? AnimInstance->Montage_GetPosition(ActiveMontage) : -1.f;

	// ---------- 3) 关键骨骼：相对角色的位置有没有变（= 画面到底动没动）----------
	//
	// ⚠⚠ 必须用【脚骨】，不能用手骨（2026-10-02 订正）。
	// 行走时手几乎不动，测手骨会得出"骨骼 0.0cm → 动画没动"的错误结论，
	// 害我误以为是姿势 tick 的问题。脚骨才是行走的直接证据。
	// 优先级：foot_l/foot_r → calf_l/calf_r → 兜底 hand_r。
	if (DebugBoneIndex == INDEX_NONE)
	{
		static const FName BoneCandidates[] = {
			TEXT("foot_l"), TEXT("foot_r"), TEXT("calf_l"), TEXT("calf_r"),
			TEXT("ball_l"), TEXT("hand_r"), TEXT("pelvis"),
		};
		for (const FName Candidate : BoneCandidates)
		{
			const int32 Idx = MeshComp->GetBoneIndex(Candidate);
			if (Idx != INDEX_NONE)
			{
				DebugBoneIndex = Idx;
				break;
			}
		}
	}

	FVector BoneLocalPos = FVector::ZeroVector;
	float BoneMove = -1.f;
	if (DebugBoneIndex != INDEX_NONE)
	{
		// ⚠ 必须转到【Mesh 局部空间】再比较：直接用"世界坐标 - 角色位置"的话，
		// BOSS 一转向，骨头跟着转，差值也会变 —— 那测出来的是转向，不是动画。
		// （Mesh 有 Yaw-90° 相对旋转，用 Actor 空间同样会量错，所以统一用 Mesh 空间。）
		BoneLocalPos = MeshComp->GetComponentTransform().InverseTransformPosition(
			MeshComp->GetBoneTransform(DebugBoneIndex).GetLocation());
		if (!LastDebugBoneLocalPos.IsZero())
		{
			BoneMove = FVector::Dist(BoneLocalPos, LastDebugBoneLocalPos);
		}
		LastDebugBoneLocalPos = BoneLocalPos;
	}

	// ---------- 4) 坐标空间一致性诊断（这才是"看着像滑步"的系统性根因）----------
	//
	// ⚠⚠⚠ 方向判定用的是【Actor 空间】（GetActorTransform），而动画的"前/右"是【Mesh 空间】。
	//   只要 BP_Boss 的 Mesh 带了相对旋转（Paragon Khaimera 极常见，为了让模型正对 +X），
	//   两个空间就不一致 → 我们算出的"前"其实喂给了动画的"侧" →
	//   BOSS 明明在往前走，却播侧向/倒向动画，画面上就是"模型平移、腿不迈步"。
	//
	// 这不是调数值能解决的，必须让方向换算走【和动画同一个空间】。
	// 这里把 Mesh 的相对旋转、以及"世界前向在两种空间下分别映成什么"打出来，一眼判定。
	{
		// ⚠ UE 5.7 里 GetRelativeRotation() 已经直接返回 FRotator，不要再 .Rotator() 转一次
		const FRotator MeshRelDeg = MeshComp->GetRelativeRotation();
		// 角色当前真实朝向（世界空间）
		const FVector WorldFwd = GetActorForwardVector().GetSafeNormal2D();
		// 世界前向在【Actor 空间】下的分量（我们以前用它算方向 → 与动画差了 90°）
		const FVector FwdInActor = GetActorTransform().InverseTransformVectorNoScale(WorldFwd).GetSafeNormal2D();
		// 世界前向在【Mesh 空间】下的分量（动画真正的"前"，现在方向判定用的就是这个）
		const FVector FwdInMesh = MeshComp->GetComponentTransform().InverseTransformVectorNoScale(WorldFwd).GetSafeNormal2D();

		UE_LOG(LogDemoCombat, Warning,
			TEXT("[Boss][空间诊断] Mesh相对旋转=(Pitch%.1f,Yaw%.1f,Roll%.1f) | 世界前向: Actor空间=(%.2f,%.2f) Mesh空间=(%.2f,%.2f) | 方向判定已改用Mesh空间(与动画一致=%d)"),
			MeshRelDeg.Pitch, MeshRelDeg.Yaw, MeshRelDeg.Roll,
			FwdInActor.X, FwdInActor.Y, FwdInMesh.X, FwdInMesh.Y,
			1);

		// 脚骨世界轨迹：直接看脚有没有在【世界空间】里朝移动方向迈步
		if (DebugBoneIndex != INDEX_NONE)
		{
			const FName BoneName = MeshComp->GetBoneName(DebugBoneIndex);
			const FVector FootWorld = MeshComp->GetBoneLocation(BoneName);
			UE_LOG(LogDemoCombat, Warning,
				TEXT("[Boss][空间诊断] 脚骨[%s] 世界位置=(%.0f,%.0f,%.0f) 本帧世界位移=%.1fcm | 角色速度=%.0f"),
				*BoneName.ToString(),
				FootWorld.X, FootWorld.Y, FootWorld.Z,
				(FootWorld - LastDebugFootWorld).Size(),
				GetVelocity().Size2D());
			LastDebugFootWorld = FootWorld;
		}
	}

	// ---------- 5) 一次性开局体检：把"为什么播不出走路动画"的原因全打出来 ----------
	// 只在第一次进入时打一次。存在的意义：别再靠猜，直接看资产到底是什么。
	// 重点看 EnableRootMotion —— 原地(in-place)序列必须【关掉】根位移，
	// 若序列自带根位移而播放模式是 RootMotionFromMontagesOnly，单节点不会应用它（正常）；
	// 反过来若把 RootMotionMode 设成别的，就会出现"动画在动但模型不跟"或反之的错配。
	if (!bLocoAuditLogged)
	{
		bLocoAuditLogged = true;
		UE_LOG(LogDemoCombat, Warning,
			TEXT("[Boss][开局体检] 动画模式=%d 是否用ABP=%d 可见性选项=%d 根位移模式=%d"),
			static_cast<int32>(MeshComp->GetAnimationMode()),
			bUsesBossAnimBP ? 1 : 0,
			static_cast<int32>(MeshComp->VisibilityBasedAnimTickOption),
			static_cast<int32>(AnimInstance->RootMotionMode));

		auto DumpSeq = [](const TCHAR* Label, const UAnimSequence* Seq)
		{
			if (!Seq)
			{
				UE_LOG(LogDemoCombat, Warning, TEXT("[Boss][开局体检] %s = 【未配置】← 这就是没有该动作的原因"), Label);
				return;
			}
			UE_LOG(LogDemoCombat, Warning,
				TEXT("[Boss][开局体检] %s = %s 加法=%d 根位移=%d 时长=%.2fs 帧数=%d"),
				Label, *Seq->GetName(),
				Seq->IsValidAdditive() ? 1 : 0,
				Seq->HasRootMotion() ? 1 : 0,
				Seq->GetPlayLength(), Seq->GetNumberOfSampledKeys());
		};
		DumpSeq(TEXT("待机"), BossIdleAnimation);
		DumpSeq(TEXT("行走Fwd"), BossJogAnimation);
		DumpSeq(TEXT("行走Bwd"), BossJogBwdAnimation);
		DumpSeq(TEXT("行走Left"), BossJogLeftAnimation);
		DumpSeq(TEXT("行走Right"), BossJogRightAnimation);
	}

	UE_LOG(LogDemoCombat, Warning,
		TEXT("[Boss][anim自检] 单节点资产=%s 时间=%.2f(Δ%+.2f) 在播=%d 循环=%d 速率=%.2f | 蒙太奇=%s 播放中=%d 位置=%.2f | 脚骨=%d 角色空间位移=%.1fcm | 实测速度=%.0f 意图=%d 物理移动=%d"),
		NodeAsset ? *NodeAsset->GetName() : TEXT("无"), NodeTime, NodeTimeDelta,
		(SingleNode && SingleNode->IsPlaying()) ? 1 : 0,
		(SingleNode && SingleNode->IsLooping()) ? 1 : 0,
		SingleNode ? SingleNode->GetPlayRate() : -1.f,
		ActiveMontage ? *ActiveMontage->GetName() : TEXT("无"), bMontagePlaying ? 1 : 0, MontagePos,
		DebugBoneIndex, BoneMove,
		DebugMeasuredSpeed, DebugMoveIntent ? 1 : 0, DebugPhysMoving ? 1 : 0);
}

UAnimSequence* ABossCharacterBase::PickLocoJogSequence()
{
	// 前冲是兜底，一定要能用
	UAnimSequence* Fwd = BossJogAnimation;
	if (!Fwd)
	{
		return nullptr;
	}

	// 当前水平速度方向（世界空间）
	FVector Vel = GetVelocity();
	Vel.Z = 0.f;
	if (Vel.SizeSquared2D() < KINDA_SMALL_NUMBER)
	{
		return Fwd;
	}

	// ⚠⚠⚠⚠ 必须换算到【Mesh 空间】，不是 Actor 空间（2026-10-02 决定性修复）
	//
	// 实测（`[Boss][空间诊断]`）：BP_Boss 的 Mesh 带【Yaw = -90°】相对旋转，
	// 于是「世界前向」在 Actor 空间是 (1,0)，在 Mesh 空间却是 (0,1) —— 差 90°。
	//
	// 后果：以前用 GetActorTransform() 算方向，等于把我们以为的"前"喂给了动画的【右/侧】轴。
	//   → BOSS 明明朝玩家直冲，却播成侧向/倒退的腿 → 画面上"模型整体平移、腿不迈步"，
	//     这才是前几轮调脚速、调朝向、调换向死区全都打偏的【系统性根因】。
	//   那些修复其实都没错，但被这个 90° 错位盖住了。
	//
	// 正解：动画的"前/右"是 Mesh 本地轴，所以方向也必须在 Mesh 空间里求。
	//   用 Mesh 的世界变换逆变换速度，得到的就是"相对于动画朝向"的分量。
	const USkeletalMeshComponent* MeshComp = GetMesh();
	const FTransform MeshXf = MeshComp ? MeshComp->GetComponentTransform() : GetActorTransform();
	const FVector Local = MeshXf.InverseTransformVectorNoScale(Vel).GetSafeNormal2D();
	// ⚠⚠ UE 5.7 里 FVector2D = TVector2<double>，没有接收 (float, float) 的构造函数，显式转 double。
	const FVector2D RawDir(static_cast<double>(Local.X), static_cast<double>(Local.Y));

	// ⚠⚠⚠ 换向角度死区（2026-10-02 实测日志订正，这才是"移动动作不自然"的真凶）
	//
	// BOSS 绕圈走位时移动方向持续小幅旋转。之前只有"冷却到了就重选"，于是
	// Fwd→Left→Bwd→Fwd→Right 每 0.5s 翻一次；每次翻向都 PlayAnimation() 把播放时间【清零】
	// （实测时间在 0.00 / 0.09 / 0.20 / 0.26 之间反复归零）。Jog 步态周期约 0.667s ——
	// 腿连一个完整步子都走不完就被打断重播，看上去就是原地倒腾 / 滑步 / 动作不连贯。
	//
	// 关键修正（第一版死区没治好）：比较对象必须是【当前正在播的那个动画的方向】，
	// 而不是"上一帧的方向"。上一帧方向在持续缓慢漂移，死区照样会被跨过。
	// 所以这里把"正在播的方向"记在 PlayingLocoLocalDir，只在真正换资产时才更新它。
	const FVector2D CompareDir = PlayingLocoLocalDir.IsNearlyZero() ? RawDir : PlayingLocoLocalDir;
	if (!RawDir.IsNearlyZero() && !CompareDir.IsNearlyZero())
	{
		const float CosAngle = static_cast<float>(
			FVector2D::DotProduct(CompareDir.GetSafeNormal(), RawDir.GetSafeNormal()));
		const float AngleDeg = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(CosAngle, -1.f, 1.f)));
		if (AngleDeg < LocoDirSwitchAngleDeg)
		{
			// 还在死区内 → 沿用当前正在播的动画（调用方保留 CurrentLocoAsset），不打断步态
			return nullptr;
		}
	}

	// ⚠ 与 PickLocoWalkMontage 同样的轴向修正（2026-10-03）：
	//   实测角色正前方在 Mesh 空间是 **+Y**（Mesh 带 Yaw=-90°），不是 +X。
	//   写死 X=前后 会把"朝前走"判成"侧移"。这里同样改成运行时测量。
	// ⚠ Local 是 const，所以结果放进新变量，不去改它。
	double FwdComp = Local.X;
	double RightComp = Local.Y;
	{
		const FVector FwdInMesh2 = MeshXf.InverseTransformVectorNoScale(
			GetActorForwardVector()).GetSafeNormal2D();
		if (!FwdInMesh2.IsNearlyZero())
		{
			const FVector RightInMesh2 = FVector::CrossProduct(FwdInMesh2, FVector::UpVector).GetSafeNormal2D();
			// ⚠ RawDir 是 FVector2D、基向量是 FVector（3 分量）→ 点积要显式写成 2D 点积，
			//    否则 C2664（TVector 与 TVector2<double> 类型不匹配）。
			FwdComp = RawDir.X * FwdInMesh2.X + RawDir.Y * FwdInMesh2.Y;
			RightComp = RawDir.X * RightInMesh2.X + RawDir.Y * RightInMesh2.Y;
		}
	}

	const bool bForward = FwdComp >= 0.0;
	const bool bRight = RightComp >= 0.0;
	const float AbsF = FMath::Abs(FwdComp);
	const float AbsR = FMath::Abs(RightComp);

	UAnimSequence* Pick = nullptr;
	if (AbsF >= AbsR)
	{
		Pick = bForward ? BossJogAnimation.Get() : BossJogBwdAnimation.Get();
	}
	else
	{
		Pick = bRight ? BossJogRightAnimation.Get() : BossJogLeftAnimation.Get();
	}
	// 任一侧素材缺失（或加载时被加法体检剔掉）→ 回退前冲
	Pick = Pick ? Pick : Fwd;

	// 真正换向时才更新"正在播的方向"记忆
	PlayingLocoLocalDir = RawDir.GetSafeNormal();
	return Pick;
}

void ABossCharacterBase::NotifyBossMoveDirection(const FVector& InDir)
{
	LastMoveCommandDir = InDir.GetSafeNormal2D();
}

UAnimMontage* ABossCharacterBase::PickLocoWalkMontage()
{
	// 前冲是兜底，一定要能用
	UAnimMontage* Fwd = BossWalkFwdMontage;
	if (!Fwd)
	{
		return nullptr;
	}

	// ⚠⚠⚠ 方向必须用【实测世界位移】，不能用 GetVelocity()（2026-10-02 第二十轮决定性修复）
	//
	// BOSS 由 AIController 驱动、靠 `RequestDirectMove` 写速度。
	// ACharacter::Tick 读 GetVelocity() 的时机在 CharacterMovement 消费掉本帧速度【之后】
	// → 实测每帧几乎恒为 0（MEMORY 里早有记录）。
	// 而这里原来第一件事就是 `if (Vel.SizeSquared2D() < KINDA_SMALL_NUMBER) return Fwd;`
	// → 每帧都命中这个提前返回 → **永远只播前冲**。
	// 这就是"四个方向的行走动画素材都在、却一个都播不到"的直接原因。
	//
	// 正解：用和移动判定同一个可靠来源 —— 本帧的真实世界位移（MoveDeltaThisFrame），
	// 它由 CharacterMovement 亲手写入，不受速度读取时机影响。详见下面的换算段。

	// ⚠⚠ 必须用【Mesh 空间】换算，不能用 Actor 空间。
	// BP_Boss 的 Mesh 带 Yaw = -90° 相对旋转，两个空间差 90°。
	// 用 Actor 空间 = 把"前"喂给动画的"侧"轴 → 明明在直冲却播侧向腿 = 看着像滑步。
	//
	// ⚠⚠⚠ 而且【绝不能读 GetVelocity()】（2026-10-02 决定性修复）
	//   AI 直驱的角色（RequestDirectMove）在本函数被调用的时机里 GetVelocity() 恒为 0 ——
	//   CharacterMovement 已经消费掉了本帧速度。实测症状：方向判定恒等于"没在动"
	//   → 永远 return Fwd → 整局只播 Boss_Walk_Fwd_Montage（21/21 帧全是它），
	//   绕圈/后撤时也在"往前跑"，于是画面上就是溜冰。
	// 正确做法：用【实测世界位移方向】(LastLocoSampleLoc → CurrentLoc)，
	//   那是由 CharacterMovement 亲手写入的，不受速度读取时机影响。
	UAnimInstance* AI = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr;
	UAnimMontage* Cur = AI ? AI->GetCurrentActiveMontage() : nullptr;
	// ⚠⚠ 不要再"没有正在播的蒙太奇就 return Fwd"（2026-10-02 修正）
	//   原来这里有一句 `if (!Cur) return Fwd;` ——
	//   但"当前没有蒙太奇"恰恰是【静止 → 刚开始移动】的时刻，
	//   也就是最需要判断方向的时刻。提前返回 = 起步永远播前冲。
	//   现在改成：拿不到当前蒙太奇时用【PlayerLocoLocalDir 里记的上一次方向】，
	//   仍然没有才回退 Fwd（真正的兜底）。

	const FTransform MeshXf = GetMesh() ? GetMesh()->GetComponentTransform() : GetActorTransform();
	// MoveDelta = 本帧真正走了多远、往哪走（由 CharacterMovement 写入，不受速度读取时机影响）
	FVector MoveDelta = MoveDeltaThisFrame;
	MoveDelta.Z = 0.f;
	if (MoveDelta.SizeSquared2D() < KINDA_SMALL_NUMBER)
	{
		// 位移也算不出方向时（起步第一帧 / 转向死区里），退回"Brain 下令的方向"；
		// 再没有就用【上一次播过的方向】（绕圈时方向缓慢变化，这个记忆最接近真实意图）。
		if (!LastMoveCommandDir.IsNearlyZero())
		{
			MoveDelta = LastMoveCommandDir;
		}
		else if (!PlayingLocoLocalDir.IsNearlyZero())
		{
			// 把 Mesh 空间的记忆方向变回世界空间再参与换算
			MoveDelta = MeshXf.TransformVectorNoScale(
				FVector(PlayingLocoLocalDir.X, PlayingLocoLocalDir.Y, 0.0));
		}
		else
		{
			return Fwd;
		}
	}

	const FVector Local = MeshXf.InverseTransformVectorNoScale(MoveDelta).GetSafeNormal2D();
	// UE 5.7 里 FVector2D = TVector2<double>，要 double 字面量
	const FVector2D RawDir(static_cast<double>(Local.X), static_cast<double>(Local.Y));

	// 换向死区：与"正在播的方向"夹角小于阈值就不换，避免打断步态
	const FVector2D CompareDir = PlayingLocoLocalDir.IsNearlyZero() ? RawDir : PlayingLocoLocalDir;
	if (!RawDir.IsNearlyZero() && !CompareDir.IsNearlyZero())
	{
		const float CosAngle = static_cast<float>(
			FVector2D::DotProduct(CompareDir.GetSafeNormal(), RawDir.GetSafeNormal()));
		const float AngleDeg = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(CosAngle, -1.f, 1.f)));
		if (AngleDeg < LocoDirSwitchAngleDeg)
		{
			// 死区内：返回当前正在播的那个（nullptr 表示"别换"）
			if (Cur == BossWalkBwdMontage)    { return BossWalkBwdMontage; }
			if (Cur == BossWalkLeftMontage)   { return BossWalkLeftMontage; }
			if (Cur == BossWalkRightMontage)  { return BossWalkRightMontage; }
			return Fwd;
		}
	}

	UAnimMontage* Pick = nullptr;
	int32 DirIndex = 0;   // 0=前 1=后 2=左 3=右

	// ⚠⚠⚠⚠ 2026-10-03 决定性修复：这里原来【写死】"X=前后、Y=左右"，是错的！
	//
	// 实测（[Boss][方向诊断] 日志）：
	//     角色正前方(Mesh空间)=(-0.00, 1.00)   ← 角色明明朝前，正前方却是 +Y！
	//     本帧移动方向(Mesh空间)=(0.91, -0.42) ← BOSS 在朝玩家走，方向却是 +X
	// 原因：BP_Boss 的 Mesh 带 Yaw=-90° 相对旋转，所以【Mesh 空间的 Y 轴才是角色正前方】。
	// 而旧代码 `bForward = Local.X >= 0` 把 +X 当"前" → 角色朝前走被判成"向右横移"
	//   → 播 Boss_Walk_Right_Montage → 玩家看到的正是【面向玩家追踪却是侧着走】。
	//
	// 正解：不要写死轴，改成【运行时测量角色正前方在 Mesh 空间的方向】，
	// 再把移动方向投影到"前/右"两个基上。骨架换了、Mesh 旋转改了都自动正确。
	const FVector FwdInMesh = MeshXf.InverseTransformVectorNoScale(
		GetActorForwardVector()).GetSafeNormal2D();
	if (FwdInMesh.IsNearlyZero())
	{
		return Fwd;
	}
	// 右手系：右 = 前 × Up（Up = (0,0,1)）
	const FVector RightInMesh = FVector::CrossProduct(FwdInMesh, FVector::UpVector).GetSafeNormal2D();

	// 把移动方向投影到"前"和"右"两个基上（显式 2D 点积：RawDir 是 FVector2D、基是 FVector）
	const float FwdComp = RawDir.X * FwdInMesh.X + RawDir.Y * FwdInMesh.Y;
	const float RightComp = RawDir.X * RightInMesh.X + RawDir.Y * RightInMesh.Y;
	const bool bForward = FwdComp >= 0.f;
	const bool bRight = RightComp >= 0.f;
	const float AbsF = FMath::Abs(FwdComp);
	const float AbsR = FMath::Abs(RightComp);

	if (AbsF >= AbsR)
	{
		Pick = bForward ? BossWalkFwdMontage.Get() : BossWalkBwdMontage.Get();
		DirIndex = bForward ? 0 : 1;
	}
	else
	{
		Pick = bRight ? BossWalkRightMontage.Get() : BossWalkLeftMontage.Get();
		DirIndex = bRight ? 3 : 2;
	}
	Pick = Pick ? Pick : Fwd;
	PlayingLocoLocalDir = RawDir.GetSafeNormal();
	// ⚠⚠⚠ 记下"这次选了哪个方向"：四条动画的原生地面速度不同（325~370），
	//   播放倍速必须跟着方向走，共用一个 415 就是空步的根因（2026-10-03）。
	CurrentLocoDirIndex = DirIndex;
	return Pick;
}

float ABossCharacterBase::ComputeLocoPlayRateForDir(float Speed2D, int32 LocalDirIndex) const
{
	// ⚠⚠⚠ 基准速度必须【按方向】取（2026-10-03）——这是空步最隐蔽的根因
	//
	// 四条行走动画的原生地面速度本来就不一样（Tools/MeasureWalkSpeedAxisAware.py
	// 按主轴自适应实测，500 点采样）：
	//     Jog_Fwd      354    Jog_Bwd      325
	//     Strafe_Left  364    Strafe_Right 370
	// 四方向共用一个 415 → 偏大 12~22% → 倍率偏小 → 脚蹬得慢于身体 → 空步。
	//
	// 而且这类 bug 极难靠读代码发现：逻辑完全正确，只有数值是错的。
	//   基准偏大 → 倍率偏小 → 脚蹬得慢 → 脚在地上【往前滑】= 空步
	//   基准偏小 → 倍率偏大 → 脚蹬得快 → 脚在身后【追着滑】= 拖步
	// 两种看起来都像"滑步"，所以必须按方向精确取值。
	float Ref = BossJogReferenceSpeed;
	switch (LocalDirIndex)
	{
	case 1:  Ref = BossJogReferenceSpeedBwd;   break;   // 后撤
	case 2:  Ref = BossJogReferenceSpeedLeft;  break;   // 左横移
	case 3:  Ref = BossJogReferenceSpeedRight; break;   // 右横移
	default: Ref = BossJogReferenceSpeed;      break;   // 0 = 前冲
	}
	if (Ref <= KINDA_SMALL_NUMBER)
	{
		// 蓝图里没配（或填了 0）→ 回退到前冲的值，最后兜底 1
		Ref = BossJogReferenceSpeed > KINDA_SMALL_NUMBER ? BossJogReferenceSpeed : 1.f;
	}

	// 倍率下限 0.6：这些是 2.0~2.2s 的长资产，倍速太低会把步态周期拉得极长
	//（看着像"慢动作踱步"），0.6 是"还能看出在迈步"的下限。
	// 上限 2.5：突进 / 快速位移时别让腿糊成一片。
	return FMath::Clamp(Speed2D / Ref, 0.6f, 2.5f);
}

float ABossCharacterBase::ComputeLocoPlayRate(float Speed2D) const
{
	// 旧接口 = 前冲方向（保持向后兼容）
	return ComputeLocoPlayRateForDir(Speed2D, 0);
}

// "移动意图"还有效的窗口（秒）：Brain 每帧下令移动会刷新 LastMoveCommandTime，
// 超过这么久没下令就视为"该停了"。
// ⚠⚠ 必须是 0.5s，不能更短（2026-10-02 实测订正）：Brain 并不会每帧都下令移动
//   （转身对齐、出招、收招的间隙就不下令），窗口太短会在这些间隙里"意图过期"，
//   于是满速移动时判定反复退回待机 → 每 0.5s 翻一次 Jog↔Idle → 播放时间反复清零。
//   现在这个窗口只作为"物理位移判定的补充"，主要依据是实测世界位移。
static constexpr float BossMoveIntentWindow = 0.5f;

void ABossCharacterBase::NotifyBossMoveCommand(float CommandedSpeed)
{
	// Brain 每帧下令移动就刷新这个时间戳。UpdateBossLocomotion 据此判断"现在还想不想走"，
	// 想走就强制播 Jog，不被 AI 直驱的速度抖动（逐帧 0↔最大）把腿冻成 Idle。
	LastMoveCommandTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;

	// ⚠⚠⚠ 2026-10-02 关键修复：同时记下 Brain【下令的速度】。
	//
	// 实测日志（`[Boss][动画] 切换位移动画` 行）每一行都是「原始速度=0」，
	// 而同一时刻 Brain 自己打的 `[Boss][移动]` 行是「速度=380」。
	// 原因：ACharacter::Tick 读 GetVelocity() 的时机在 CharacterMovement 消费掉
	// RequestDirectMove 的【本帧速度】之后 —— 拿到的是被清零/尚未回填的值。
	// 于是 UpdateBossLocomotion 里：
	//   SmoothedLocoSpeed 长期趋近 0 → 播放倍速被死死钉在下限 0.75
	//   → 腿只按 0.75 倍速慢动作爬（脚速 ≈ 277cm/s），而胶囊仍以 380cm/s 平移
	//   → 画面上就是"没有动画、只是模型平移"。
	// Brain 是速度的【下达者】，它知道准确值；由它传进来，别让 C++ 去猜。
	LastMoveCommandSpeed = FMath::Max(CommandedSpeed, 0.f);
}

void ABossCharacterBase::UpdateBossLocomotion(float DeltaTime)
{
	// 已切到 BOSS 专用 ABP：Idle/Jog 由 ABP 内部的"按移动速度混合"节点自己管，
	// C++ 不再用 PlayAnimation 强行插手（否则会把 ABP 打回单节点模式，反而丢动画）。
	if (bUsesBossAnimBP)
	{
		return;
	}

	USkeletalMeshComponent* MeshComp = GetMesh();
	UAnimInstance* AnimInstance = MeshComp ? MeshComp->GetAnimInstance() : nullptr;
	if (!MeshComp || !AnimInstance)
	{
		return;
	}

	// 保险（防回归）：起点/行走素材若是【加法动画】，单节点会拒绝设置 →
	// 变成"每帧重试 + 每帧刷引擎告警"却什么都不播。直接不插手。
	if (IsAdditiveAnim(BossIdleAnimation) || IsAdditiveAnim(BossJogAnimation)
		|| IsAdditiveAnim(BossJogBwdAnimation) || IsAdditiveAnim(BossJogLeftAnimation)
		|| IsAdditiveAnim(BossJogRightAnimation))
	{
		return;
	}

	// 死亡后保持当前（死亡）姿势，不要再切回待机/行走
	if (IsDead())
	{
		return;
	}

	// ⚠ 破防跪地期间同样不插手（2026-10-04）
	//   虚弱蒙太奇播完 / 循环段没配时，下面的逻辑会重新挂上待机或行走蒙太奇 →
	//   画面就是"跪着滑行"。破防期间一律不切，姿态交给 UpdateBreakFreeze + 虚弱蒙太奇。
	if (IsPoiseBroken())
	{
		return;
	}

	UAnimSingleNodeInstance* SingleNode = MeshComp->GetSingleNodeInstance();
	UAnimationAsset* CurrentAsset = SingleNode ? SingleNode->GetAnimationAsset() : nullptr;

	// 表现动画（攻击 / 受击 / 破防 / 举盾…）正在播 → 不打扰，让它播完再说。
	//
	// ⚠⚠ 判据必须是"有没有蒙太奇正在播"，【绝对不能】写成"当前资产 != loco"（2026-09-30 定案）：
	//   单节点下 PlayAnimation(蒙太奇) 播完之后，CurrentAsset 仍然停在那个蒙太奇上（停在最后一帧），
	//   并不会自动清空。于是"当前资产不是 loco"这个条件在出招之后就【永远成立】→ 每帧早退
	//   → 待机/行走（序列）永远播不出来 → 表现就是"BOSS 一直卡在出招的最后一帧 / 没有移动动画"。
	//   日志实证：BOSS 出完招 CurrentAsset 一直是 Boss_Heavy_01_Montage（时间停在 0.81、在播=0），
	//   整份日志里 Idle_NonAdditive / Jog_Fwd 一次都没出现过。
	//   而 IsAnyMontagePlaying() 是可靠的：蒙太奇播完就变 false（同一份日志里 在播 从 1 变 0）。
	//
	// ⚠⚠⚠ 2026-10-12：这里不能再无条件 return（"移动时模型不动、贴着地面平移"的最终根因）
	//
	// 实测把「角色速度」和「当时播的资产」配对统计（107 个样本）：
	//   移动中(>100cm/s) 却没播 Jog 的样本 = **47 / 52（90%）**
	//     其中 32 个在播攻击蒙太奇、15 个在播 Idle
	// 也就是说：BOSS 绝大多数"移动"时间腿上根本没有动画，只有上半身/整体在动 → 就是用户看到的平移。
	//
	// 原因：攻击蒙太奇播放期间（本函数早退）以及出招/后摇期间 BOSS 仍在【物理位移】：
	//   ① 上一次 RequestDirectMove 残留的速度；
	//   ② 蒙太奇自身根位移。
	// 而"不播行走动画"和"确实在位移"这两件事必须解耦：
	//   蒙太奇只该盖住上半身，走路腿必须继续播 —— 单节点做不到分层，
	//   那就至少保证【只要胶囊在动，腿上就有动画】：
	//   真正在移动（实测位移速度超阈值）时，强制盖上行走动画。
	// ⚠⚠⚠ 判据必须是"【除行走蒙太奇外】还有没有别的蒙太奇在播"（2026-10-02 第十八轮修正）
	//
	// 原来写的是 `AnimInstance->IsAnyMontagePlaying()` —— 但【行走本身现在也是蒙太奇】！
	// 于是这个条件在走路时同样为真 → 每帧都走进"让招式播完"的早退分支
	// → 永远走不到下面的行走播放逻辑 → 表现是"只有追击中动一下，出招期间完全没动画"。
	// 必须把"当前正在播的行走蒙太奇"排除掉。
	UAnimMontage* PlayingNow = AnimInstance->GetCurrentActiveMontage();
	const bool bIsOwnWalkMontage =
		(PlayingNow == BossIdleMontageAsset)
		|| (PlayingNow == BossWalkFwdMontage)
		|| (PlayingNow == BossWalkBwdMontage)
		|| (PlayingNow == BossWalkLeftMontage)
		|| (PlayingNow == BossWalkRightMontage);
	const bool bOtherMontagePlaying = AnimInstance->IsAnyMontagePlaying() && !bIsOwnWalkMontage;
	// 记到成员上，供 ApplyLocomotionForFrame 的诊断日志使用（那个函数拿不到这里的局部变量）
	bOtherMontagePlayingNow = bOtherMontagePlaying;

	if (bOtherMontagePlaying)
	{
		// 只有"BOSS 此刻确实在空间里移动"才打断蒙太奇去播走路；
		// 站着出招（本来就该站桩）时绝不打扰。
		//
		// ⚠⚠ 这里必须【自己算速度并同步所有状态】，不能只算个局部变量就 return。
		//   本函数后面的 SmoothedLocoSpeed / bMoveIntent / bPhysicallyMoving /
		//   Debug* 都不会执行 —— 早退时若不同步，它们会一直保留【上一帧的旧值】，
		//   导致日志与实际判定严重错位（曾出现"实测速度恒为 0"）。
		const FVector Cur = GetActorLocation();
		const float Dt = DeltaTime > KINDA_SMALL_NUMBER ? DeltaTime : (1.f / 60.f);
		const float RawFrame = (Cur - LastLocoSampleLoc).Size2D() / Dt;
		LastLocoSampleLoc = Cur;
		bLocoSampleAdvanced = true;

		MeasuredLocoSpeed = FMath::FInterpTo(MeasuredLocoSpeed, RawFrame, DeltaTime, 20.f);
		// ⚠ 与下面"正常路径"同一处理：上升沿至少跟到实测的 70%（见下面注释的完整说明）。
		//   起步时位移是"阶跃"的，平滑值爬太慢 → 倍速跟不上身体 → 脚往前滑 = 空步。
		if (RawFrame > MeasuredLocoSpeed)
		{
			MeasuredLocoSpeed = FMath::Max(MeasuredLocoSpeed, RawFrame * 0.7f);
		}
		const bool bIntentNow = (GetWorld() ? (GetWorld()->GetTimeSeconds() - LastMoveCommandTime) < BossMoveIntentWindow : false);
		const bool bPhysNow = MeasuredLocoSpeed > BossJogSpeedThreshold;
		DebugMeasuredSpeed = MeasuredLocoSpeed;
		DebugMoveIntent = bIntentNow;
		DebugPhysMoving = bPhysNow;
		SmoothedLocoSpeed = (bPhysNow || bIntentNow)
			? FMath::Max(MeasuredLocoSpeed, bIntentNow ? LastMoveCommandSpeed : 0.f)
			: 0.f;
		LocoDwellTimer += DeltaTime;
		LocoDirSwitchTimer += DeltaTime;

		if (!bPhysNow)
		{
			return;   // 真的没在动 → 安心让蒙太奇播完
		}
		// 在动 → 继续往下走，让行走动画接管（腿要有动作）
	}
	else
	{
		// 没有蒙太奇时也要推进采样点（上面早退的分支已经推进过了）
	}

	const bool bBroken = GetAbilitySystemComponent()
		&& GetAbilitySystemComponent()->HasMatchingGameplayTag(DemoTag::State_Broken);

	// ⚠⚠ 速度低通滤波（2026-10-01 修"模型不动整体滑动"的关键）
	// AI 直驱下 GetVelocity() 会逐帧 0↔最大速度抖动（实测同秒里 380→0→380 反复）。
	// 若直接用原始速度做判定：读到 0 的那几帧就会退到 Idle（腿冻住），可胶囊下一帧又被推着走
	// → 整体滑动；速度回升又播 Jog → "有时又动"。平滑后持续移动时 SmoothedLocoSpeed 不会掉到阈值下，
	// 不再掉回 Idle；只有真正停住（攻击收招、到达距离）平滑值才会衰减到 0 → 正常进 Idle。
	// ⚠⚠ 速度来源（2026-10-02 关键修复，见 NotifyBossMoveCommand 的注释）
	//
	// 不能再用 GetVelocity() 当唯一来源：ACharacter::Tick 读它时，CharacterMovement 已经
	 // 消费完本帧的 RequestDirectMove 速度 → 实测每帧都是 0。
	// 那会让 SmoothedLocoSpeed 长期趋近 0，播放倍速被钉在下限 → 腿慢动作爬、胶囊照常平移
	// → 玩家看到的正是"没有动画，只是模型平移"。
	//
	// 正确做法：移动中优先用 Brain 下令的速度（速度的真正来源）；已经停下（意图过期）
	// 才退回读 GetVelocity()，让"停住"这件事仍然能被感知到。
	// ⚠⚠⚠ 移动判定的【唯一可靠依据 = 实际世界位移】（2026-10-02 决定性修复）
	//
	// 之前用两个间接信号判断"BOSS 在不在走"，全都不可靠：
	//   ① GetVelocity()：对 RequestDirectMove 驱动的 AI 恒为 0（已被 Movement 消费）。
	//   ② 移动意图时间戳（NotifyBossMoveCommand）：窗口只有 0.15s，而 Brain 的
	//      下令并不连续（转身/出招/收招的间隙就不下令）→ 窗口频繁过期。
	// 于是实测日志出现这种翻面（平滑速度 380 → 25 → 380 → 25，每 0.5s 一次）：
	//   平滑速度=380 → 行走 Jog
	//   平滑速度=25  → 待机 Idle     ← 明明在以 380 移动，却退成待机
	//   平滑速度=380 → 行走 Jog
	// 而每次 Idle↔Jog 切换都执行 PlayAnimation() = 播放时间【清零】，
	// 腿反复从第 0 帧重来 → 视觉上就是"没有走路动画、只是模型平移"。
	//
	// 正解：胶囊【真的有没有在空间里移动】才是唯一事实来源。
	// 直接量上一帧到这一帧的世界坐标差 —— 它由 CharacterMovement 亲自写入，
	// 不受速度读取时机、意图窗口、AI 寻路失败等任何间接环节影响。
	//
	// ⚠ 若上面的"蒙太奇分支"已经算过并推进了采样点（bLocoSampleAdvanced=true），
	//   这里【必须整段跳过】—— 再算一次会得到 0 速度、并把状态覆盖成"静止"，
	//   正是"实测速度恒为 0、行走的腿一直不播"的直接原因（本项目已踩过一次）。
	if (bLocoSampleAdvanced)
	{
		bLocoSampleAdvanced = false;
		// ⚠⚠ 这里【必须也更新 MoveDeltaThisFrame】（2026-10-02 第二十轮修正）
		//   它是方向判定的唯一可靠来源（PickLocoWalkMontage 靠它选前后左右）。
		//   蒙太奇分支里已经推进了 LastLocoSampleLoc，但【没有】写 MoveDeltaThisFrame
		//   → 它一直保留老值（甚至恒为 0）→ 方向判定失效 → 整局只播前冲。
		const FVector HereNow = GetActorLocation();
		MoveDeltaThisFrame = HereNow - LastLocoSampleLoc;
		// 直接用蒙太奇分支里已经算好的结果继续往下走，不再重复计算
		const bool bMovingNow = DebugPhysMoving || DebugMoveIntent;
		ApplyLocomotionForFrame(bMovingNow, DeltaTime);
		return;
	}

	const FVector CurrentLoc = GetActorLocation();
	// ⚠ 先算位移【再】推进采样点：顺序反了算出来永远是 0。
	//   MoveDeltaThisFrame 是方向判定的唯一可靠来源（详见头文件注释）：
	//   绝不能用 GetVelocity() —— 对 RequestDirectMove 驱动的 AI 角色它恒为 0。
	MoveDeltaThisFrame = CurrentLoc - LastLocoSampleLoc;
	LastLocoSampleLoc = CurrentLoc;
	const float FrameDelta = (DeltaTime > KINDA_SMALL_NUMBER)
		? MoveDeltaThisFrame.Size2D() / DeltaTime : 0.f;
	// 用一点点平滑压掉单帧噪声，但对真实移动几乎无延迟（等效速度远高于指令值时立刻跟上）
	const float MeasuredSpeed = FMath::FInterpTo(MeasuredLocoSpeed, FrameDelta, DeltaTime, 20.f);
	// ⚠⚠⚠ 2026-10-03：起步时【不能让倍速慢慢爬】（"太空步"的第二大来源）
	//
	// 消空步的充要条件是：脚速(= 原生速度 × 倍速) == 身体实际平移速度。
	// 倍速 = 实测位移 / 基准，所以它必须【紧跟】实测位移。
	// FInterpTo 的 20 需要约 0.15s 才收敛，而 BOSS 起步/停下时位移是"阶跃"的：
	//   起步第一帧实测就已经接近满速，但平滑值还在低位
	//   → 腿按 0.6 倍速慢动作爬，身体按 380cm/s 平移 → 脚在地上往前滑 = 空步。
	// 修法：平滑值【至少不低于实测值的 70%】——
	//   上升（起步）立刻跟上，宁可抖动也不要滑；下降（停下）仍走平滑，避免抖腿。
	// ⚠ MeasuredSpeed 是 const，结果写进 MeasuredLocoSpeed（成员）供后面统一使用。
	const float SmoothedSpeed = (FrameDelta > MeasuredSpeed)
		? FMath::Max(MeasuredSpeed, FrameDelta * 0.7f)
		: MeasuredSpeed;
	MeasuredLocoSpeed = SmoothedSpeed;

	// Brain 的指令速度作为"上限参考"：没有指令就不该有位移（防止被碰撞/推挤误判成移动）
	const bool bMoveIntent = (GetWorld() ? (GetWorld()->GetTimeSeconds() - LastMoveCommandTime) < BossMoveIntentWindow : false);
	const bool bPhysicallyMoving = SmoothedSpeed > BossJogSpeedThreshold;
	const bool bMovingNow = bPhysicallyMoving || bMoveIntent;

	// 把判定结果记下来，供 anim自检 日志和资产名打在同一行（避免靠时间戳配对而错位）
	DebugMeasuredSpeed = SmoothedSpeed;
	DebugMoveIntent = bMoveIntent;
	DebugPhysMoving = bPhysicallyMoving;

	ApplyLocomotionForFrame(bMovingNow, DeltaTime);
}

// 选定并应用本帧的位移动画（Idle / Jog）。把"判定"和"应用"拆开，
// 好让"蒙太奇分支已经算过速度"和"正常路径"两条路都复用同一段应用逻辑，不会走歪。
void ABossCharacterBase::ApplyLocomotionForFrame(bool bMovingNow, float DeltaTime)
{
	// 倍速用的速度。⚠⚠⚠ 只能用【实测位移速度】，不能取"实测与指令的较大者"（2026-10-02 第二十轮）
	//
	// 原来写的是 `Max(实测, 指令)`，本意是"起步第一帧就有正确倍速"，但造成了新问题：
	//   Brain 每帧都下令（MoveIntent 一直为真）→ 即使实测速度只有 57~127cm/s，
	//   倍速也按指令值 380 算 → 腿在按 0.92 倍速全速倒腾，而身体只挪一点点
	//   → **脚底在地面上打滑 = 空步**（实测日志：实测速度=57 时倍速仍是 0.92）。
	// 倍速必须严格跟随【身体真实位移速度】，这正是消空步的充要条件。
	SmoothedLocoSpeed = bMovingNow ? MeasuredLocoSpeed : 0.f;

	LocoDwellTimer += DeltaTime;
	LocoDirSwitchTimer += DeltaTime;

	// …以下为原本的 Idle/Jog 选择与播放逻辑…
	USkeletalMeshComponent* MeshComp = GetMesh();
	UAnimationAsset* CurrentAsset = nullptr;
	if (USkeletalMeshComponent* MC = MeshComp)
	{
		if (UAnimSingleNodeInstance* SN0 = MC->GetSingleNodeInstance())
		{
			CurrentAsset = SN0->GetAnimationAsset();
		}
	}
	// ⚠⚠ 移动意图已在上面的"速度来源"段里算好（移动中优先用 Brain 下令的速度）。
	// 只要意图在，就强制播 Jog；意图消失后才交给平滑速度 + 迟滞带判退场，避免急停时腿还乱蹬。

	// ⚠⚠ 迟滞 + 驻留（2026-09-30 定案，这一步是"移动动画不显示"的真凶修复）
	//
	// 原写法：bMoving = Speed > 阈值。BOSS 实际速度在阈值（50）附近反复抖动（实测 34~180 来回跳），
	// 于是【每一帧】都在 Idle/Jog 之间翻转，而每次翻转都执行 PlayAnimation() = 从头开始播。
	// 结果：动画永远停在第 0 帧，画面上就是"BOSS 一动不动 / 一直在重复同一个动作 / 走路动画没播出来"。
	// 实证：一次 13 秒的 PIE 里这条日志刷了 1066 次。
	//
	// 修法：
	//   ① 迟滞带：进 Jog 要超过阈值；已经在 Jog 里要掉到 阈值×LocoJogExitScale 以下才退回 Idle。
	//      中间的死区让抖动不再引发翻面。
	//   ② 驻留时间：切换后 LocoMinDwellTime 秒内一律维持现状，双保险。
	//   ③ 2026-10-01：判定改用【平滑速度】而非原始速度，吸收 AI 直驱的逐帧抖动。
	//   ④ 2026-10-02：改用【真实世界位移】+【移动意图】两个可靠信号（见上面 bMovingNow）。
	//
	// ⚠⚠ 退 Jog 的判据要【比进 Jog 严格得多】，否则一次噪声就翻面、而翻面=播放时间清零。
	//   实测旧日志在满速移动时每 0.5s 就 Jog→Idle 翻一次（380→25→380→25）。
	//   现在进 Jog 门槛 = 阈值(50)，退 Jog 门槛 = 阈值×LocoJogExitScale(0.35) ≈ 17.5，
	//   且额外要求"意图也过期"，任何一侧没满足都保持 Jog。
	bool bWantJog = false;
	if (DebugMoveIntent || DebugPhysMoving)
	{
		// 正在真实移动（或 Brain 正在下令）：腿必须动
		bWantJog = true;
	}
	else if (LocoDwellTimer < LocoMinDwellTime)
	{
		bWantJog = bBossLocoPlayingJog;   // 驻留期内不许翻面
	}
	else
	{
		// 只有"位移确实停了 且 意图也过期 且 低于退出门槛"才退回来
		bWantJog = bBossLocoPlayingJog
			? (DebugMeasuredSpeed > BossJogSpeedThreshold * LocoJogExitScale)
			: false;
	}

	// 移动中（且没破防虚弱）才走 Jog，否则待机
	// ⚠ 破防判定放在这里取：拆分出 ApplyLocomotionForFrame 后，外层的 bBroken 局部变量已不可见。
	const bool bBrokenNow = GetAbilitySystemComponent()
		&& GetAbilitySystemComponent()->HasMatchingGameplayTag(DemoTag::State_Broken);
	const bool bMoving = !bBrokenNow && bWantJog;

	// ---- 选哪个行走【蒙太奇】：按"移动方向相对自身朝向"分前后左右 ----
	// ⚠⚠⚠ 2026-10-02 第十六轮：行走改用【蒙太奇】而不是单节点序列。
	 //
	 // 为什么这是"移动时没有动画、整体在地面平移"的结构性根因：
	 //   单节点 PlayAnimation(序列) 与 招式蒙太奇【不在同一条 Slot 通道】：
	 //     · 序列没有 Slot → SetAnimationAsset 不会注册 DefaultSlot → 招式 Montage_Play 静默失败；
	 //     · 招式蒙太奇播放时会【整个盖住】底层序列 → 腿停摆；
	 //     · 招式结束回到底层又是从第 0 帧重播 → 步态永远走不完一个完整周期。
	 //   行走也用蒙太奇后，行走与招式同挂 DefaultSlot，走引擎标准的 Slot 混合，步态能真正循环。
	 // 骨架必须一致（Khaimera），否则 Montage_Play 静默失败 —— BeginPlay 已做校验并打日志。

	// 选方向（Montage 版）：逻辑与序列版一致，但返回蒙太奇资产
	UAnimMontage* DesiredMontage = nullptr;
	if (bMoving)
	{
		DesiredMontage = PickLocoWalkMontage();
	}
	else
	{
		DesiredMontage = BossIdleMontageAsset;
	}

	// ⚠⚠⚠ 方向判定诊断（2026-10-03）—— 一次性回答三个问题：
	//   ① Mesh 空间的"角色正前方"到底是 +X 还是 +Y？
	//      （BP_Boss 的 Mesh 带 Yaw=-90° 相对旋转，两者可能差 90°，
	//        如果分类时把 Local.Y 当"横向"，正前方移动就会被判成"侧向" → 播 Strafe
	//        → 玩家看到的正是"明明在朝玩家走，动画却是侧着走"。）
	//   ② 此刻是否被别的蒙太奇挡住（bOtherMontagePlaying）。
	//   ③ 移动方向源是什么（实测位移 / Brain 指令 / 上次记忆）。
	{
		const USkeletalMeshComponent* MC = GetMesh();
		const FTransform MXF = MC ? MC->GetComponentTransform() : GetActorTransform();
		// 角色（Actor）正前向在 Mesh 空间里的表示 —— 这才是"动画的正前方"
		const FVector MeshSpaceFwd = MXF.InverseTransformVectorNoScale(GetActorForwardVector()).GetSafeNormal2D();
		// 本帧实际移动方向在 Mesh 空间的表示
		FVector MD = MoveDeltaThisFrame;
		MD.Z = 0.f;
		const FVector MeshSpaceMove = MXF.InverseTransformVectorNoScale(
			MD.GetSafeNormal2D()).GetSafeNormal2D();
		// 与上一行日志的换算结果对比
		const FVector MeshFwdAlt = MXF.InverseTransformVectorNoScale(
			FVector(1, 0, 0)).GetSafeNormal2D();

		UE_LOG(LogDemoCombat, Log,
			TEXT("[Boss][方向诊断] 角色正前方(Mesh空间)=(%.2f,%.2f) 世界+X在Mesh空间=(%.2f,%.2f) ")
			TEXT("本帧移动方向(Mesh空间)=(%.2f,%.2f) 判定方向索引=%d 选中=%s 位移=%.1fcm ")
			TEXT("移动源=%s 他牌蒙太奇=%d 状态=%s"),
			MeshSpaceFwd.X, MeshSpaceFwd.Y,
			MeshFwdAlt.X, MeshFwdAlt.Y,
			MeshSpaceMove.X, MeshSpaceMove.Y,
			CurrentLocoDirIndex,
			DesiredMontage ? *DesiredMontage->GetName() : TEXT("无"),
			MD.Size2D(),
			MD.SizeSquared2D() > KINDA_SMALL_NUMBER ? TEXT("实测位移")
				: (			!LastMoveCommandDir.IsNearlyZero() ? TEXT("Brain指令") : TEXT("上次记忆")),
			bOtherMontagePlayingNow ? 1 : 0,
			bMoving ? TEXT("行走") : TEXT("待机"));
	}

	// ⚠ 播放倍速要先算出来：蒙太奇通道下面就要用（Montage_Play / Montage_SetPlayRate 都吃它）。
	// 倍速只对"行走蒙太奇"有意义；待机蒙太奇恒 1.0 倍（待机的时长不该被移速缩放）。
	//
	// ⚠⚠⚠ 按【方向】取基准速度（2026-10-03）：四条动画原生速度不同（354/325/364/370），
	//   共用一个值就是空步的根因。当前方向由 PickLocoWalkMontage 记在 CurrentLocoDirIndex。
	const bool bDesiredIsJogMontage = bMoving;
	const float LocoRate = bDesiredIsJogMontage
		? ComputeLocoPlayRateForDir(SmoothedLocoSpeed, CurrentLocoDirIndex)
		: 1.f;

	// 蒙太奇方案可用 → 走蒙太奇通道
	if (bUseMontageLocomotion)
	{
		if (!DesiredMontage)
		{
			DesiredMontage = bMoving ? BossWalkFwdMontage : BossIdleMontageAsset;
		}
		if (!DesiredMontage)
		{
			return;
		}

		UAnimSingleNodeInstance* LocoSN = MeshComp->GetSingleNodeInstance();
		UAnimationAsset* CurAsset = LocoSN ? LocoSN->GetAnimationAsset() : nullptr;

		// ⚠⚠⚠ 2026-10-03：判断"要不要重挂底层资产"的两个条件，缺一不可。
		//
		// 条件 A：目标蒙太奇和上次挂的不是同一个（换方向 / 待机↔行走）
		// 条件 B：当前资产【压根不是蒙太奇】
		//
		// ---- 为什么必须有条件 B（"开始时地面平移、第一次攻击后才有移动动画"的最终根因）----
		//
		// 单节点动画的渲染规则（读引擎源码 + 实测日志双重确认）：
		//   FAnimNode_SingleNode::Evaluate_AnyThread 只对【Proxy->CurrentAsset】求值：
		//     · CurrentAsset 是 UAnimSequence → 走序列分支，直接输出这一条序列的姿势；
		//     · CurrentAsset 是 UAnimMontage   → 走 Montage 分支，调用
		//       Proxy->SlotEvaluatePose(ActiveMontageSlot, ...)，
		//       把该 Slot 上【所有】蒙太奇实例（行走 / 攻击 / 受击）合成出来。
		//   而 ActiveMontageSlot 这个槽位是 UAnimSingleNodeInstance::SetAnimationAsset() 里
		//   对【蒙太奇资产】才注册的（RegisterSlotNodeWithAnimInstance + SetMontagePreviewSlot）。
		//
		//   ⇒ 只要 CurrentAsset 还是一条【序列】，Montage_Play 播的那些行走蒙太奇
		//     就【一个像素都不会渲染】：蒙太奇实例在跑、时间在走，但没人把它求值出来。
		//     画面上 = 腿保持序列的待机姿势，身体被胶囊推着平移 = 【地面平移】。
		//
		// 实测日志（15:02 那次录像）逐行印证：
		//   [anim自检] 单节点资产=Idle_NonAdditive 时间=1.73 | 蒙太奇=Boss_Walk_Fwd_Montage 播放中=1
		//              | 脚骨=127 角色空间位移=0.1cm      ← 走路蒙太奇在播，但脚骨一动不动
		//   第一次攻击之后（PlayCombatMontage 走的是 PlayAnimation=SetAnimationAsset）：
		//   [anim自检] 单节点资产=Boss_Slash_01_Montage ... | 脚骨=127 角色空间位移=68.0cm  ← 腿动了
		//   同一份日志里，"脚骨位移"从 0.0/0.1cm 跳到 39~68cm 的分界点，正好就是第一次攻击。
		//
		// ⇒ 修法：底层行走/待机蒙太奇必须【自己成为 CurrentAsset】。
		//   单节点下唯一能做到这件事的 API 就是 PlayAnimation（内部 = SetAnimationAsset），
		//   它顺带把 DefaultSlot 注册好 —— 之后攻击/受击蒙太奇（同挂 DefaultSlot）
		//   就能正常分层盖在腿上播出来。
		//   ⚠ 不能用 AnimInstance->Montage_Play：它只往 MontageInstances 里加实例，
		//     完全不碰 CurrentAsset → 底层还是序列 → 依然是"没有走路动画的地面平移"。
		const bool bCurAssetIsMontage = CurAsset && CurAsset->IsA<UAnimMontage>();
		// ⚠ 重挂底层资产 = RestartMontage，会把新蒙太奇插到 MontageInstances 末尾（= 叠在最上层）。
		//   出招期间重挂会把攻击动画直接盖掉，所以【招式蒙太奇在播时不允许换向】，
		//   等它播完再换 —— 顶多这一两帧腿沿用旧方向，不会出现"攻击看不见了"。
		//
		// ★ 条件 C（2026-10-03）：招式 / 受击 / 举盾蒙太奇播完之后，必须【立刻】把底层交还行走。
		//   引擎源码（AnimSingleNodeInstance.cpp:189）：RestartMontage() → Montage_Play(Montage, Rate)
		//   用的是默认 bStopAllMontages=**true** → StopAllMontagesByGroupName()
		//   → 同组（DefaultGroup）里【正在播的行走蒙太奇会被一起 Stop 掉】。
		//   于是招式播完的那一帧：CurrentAsset 还挂在招式蒙太奇上，而它自己的实例已经结束，
		//   行走蒙太奇又被 Stop 了 → 没有任何蒙太奇在播 → 姿势冻在招式最后一帧
		//   → 玩家看到的就是"一段平移"。
		//   没有条件 C 时只能靠下面 else 分支的自愈（要等 Montage_IsPlaying 判定为假），
		//   现在改成：只要"没有招式在播 + 当前资产不是我要的那条行走蒙太奇"→ 立即重挂。
		const bool bStaleMontageAsset = bCurAssetIsMontage
			&& !bOtherMontagePlayingNow
			&& (CurAsset != DesiredMontage);
		const bool bNeedApplyLoco = !bCurAssetIsMontage
			|| ((CurrentLocoMontage != DesiredMontage) && !bOtherMontagePlayingNow)
			|| bStaleMontageAsset;

		if (bNeedApplyLoco)
		{
			// 换向时先停掉上一条行走蒙太奇：否则两条会同时挂在 DefaultSlot 上互相叠加
			// （步态互相打架 = 看着像抽搐）。0.15s 淡出，跨向过渡不至于突兀。
			if (CurrentLocoMontage && CurrentLocoMontage != DesiredMontage)
			{
				if (UAnimInstance* AIStop = MeshComp->GetAnimInstance())
				{
					AIStop->Montage_Stop(0.15f, CurrentLocoMontage.Get());
				}
			}

			MeshComp->PlayAnimation(DesiredMontage, /*bLoop=*/true);
			// ⚠ 顺序很重要：PlayAnimation 内部会先用 bIsLooping=false 走一遍 SetMontageLoop
			//   （把实例上的"末段→首段"链接清掉），所以必须在它【之后】用引擎自己的
			//   SetMontageLoop(..., true) 把链接补回来。实测：不补的话
			//   Boss_Idle_Montage 会播一遍就停（走路那条恰好接上了，不能指望运气）。
			if (UAnimSingleNodeInstance* SN2 = MeshComp->GetSingleNodeInstance())
			{
				SN2->SetMontageLoop(DesiredMontage, /*bIsLooping=*/true);
				// PlayAnimation 内部会把倍速重置成 1.0，按实际移速重新设一遍（脚速要跟移速）
				SN2->SetPlayRate(LocoRate);
			}

			CurrentLocoMontage = DesiredMontage;
			bBossLocoPlayingJog = bMoving;
			LocoDwellTimer = 0.f;
			LocoDirSwitchTimer = 0.f;

			UE_LOG(LogDemoCombat, Log,
				TEXT("[Boss][动画] 挂底层行走蒙太奇：%s 倍速=%.2f（实测速度 %.0f）当前资产=%s"),
				*DesiredMontage->GetName(), LocoRate, DebugMeasuredSpeed,
				CurAsset ? *CurAsset->GetName() : TEXT("空"));
		}
		else
		{
			// 已经是当前资产：只刷新倍速（脚速要跟移速），并把进度往前推。
			if (LocoSN)
			{
				LocoSN->SetPlayRate(LocoRate);

				// ⚠ 自愈（2026-10-03）：蒙太奇必须有 CompositeSection，SetMontageLoop 才能把
				//   末段接回首段；没有 section 的蒙太奇播一遍就停 → 腿停在最后一帧 = 又变成平移。
				//   这里检测到停了就直接重挂，不依赖资产是否规范。
				UAnimMontage* AsMontage = Cast<UAnimMontage>(DesiredMontage);
				if (AsMontage && !LocoSN->Montage_IsPlaying(AsMontage) && !bOtherMontagePlayingNow)
				{
					MeshComp->PlayAnimation(DesiredMontage, /*bLoop=*/true);
					if (UAnimSingleNodeInstance* SN3 = MeshComp->GetSingleNodeInstance())
					{
						SN3->SetMontageLoop(AsMontage, /*bIsLooping=*/true);
						SN3->SetPlayRate(LocoRate);
					}
					UE_LOG(LogDemoCombat, Log,
						TEXT("[Boss][动画] 行走蒙太奇已停（可能缺 CompositeSection）→ 重挂：%s"),
						*DesiredMontage->GetName());
				}
			}
		}
		return;
	}

	// ---- 序列方案（蒙太奇缺失时的回退）----
	UAnimSequence* Desired = BossIdleAnimation;
	if (bMoving)
	{
		// 方向化 Jog 限频切换：已经在播某一向 Jog、且冷却还没到，就沿用当前资产，
		// 不再按每帧旋转后的新方向重选 → 避免 PlayAnimation 反复清零进度（卡第 0 帧像滑步）。
		if (bBossLocoPlayingJog && CurrentLocoAsset && LocoDirSwitchTimer < LocoDirSwitchCooldown)
		{
			Desired = Cast<UAnimSequence>(CurrentLocoAsset);
		}
		else
		{
			Desired = PickLocoJogSequence();
			// ⚠ PickLocoJogSequence 返回 nullptr = "新方向还在角度死区内，不许打断当前步态"。
			//   这时必须沿用正在播的那个 Jog（而不是掉回 Idle，否则会出现"走着走着卡成待机"）。
			if (!Desired && bBossLocoPlayingJog)
			{
				Desired = Cast<UAnimSequence>(CurrentLocoAsset);
			}
		}
		if (!Desired)
		{
			Desired = BossIdleAnimation;
		}
	}
	if (!Desired)
	{
		return;
	}

	// 播放倍速 LocoRate 已在上面算好（蒙太奇通道要用，序列通道共用同一个值）：
	//   倍速【只给 Jog】。待机必须恒 1.0 倍 —— 之前待机也套速度匹配，
	//   停住时速度≈0 → 倍速被钉在下限 → 待机以慢动作播放，看起来同样像"没在动"。
	const bool bDesiredIsJog = (Desired != BossIdleAnimation);
	const float SeqRate = bDesiredIsJog ? LocoRate : 1.f;

	// 已经在播想要的那个：不重播（每次重播都会把进度清零，动画就卡在第 0 帧）
	if (CurrentAsset == Desired)
	{
		// 仍按当前速度刷新播放倍速，让脚速跟移速对齐、消打滑
		if (UAnimSingleNodeInstance* SN = MeshComp->GetSingleNodeInstance())
		{
			if (FMath::Abs(SN->GetPlayRate() - SeqRate) > 0.05f)
			{
				SN->SetPlayRate(SeqRate);
			}
		}
		return;
	}

	MeshComp->PlayAnimation(Desired, /*bLoop=*/true);
	if (UAnimSingleNodeInstance* SN = MeshComp->GetSingleNodeInstance())
	{
		SN->SetPlayRate(SeqRate);
	}
	CurrentLocoAsset = Desired;
	bBossLocoPlayingJog = (Desired != BossIdleAnimation);
	LocoDwellTimer = 0.f;
	LocoDirSwitchTimer = 0.f;
	// 刚切进 Jog 时清空"正在播方向"记忆，让第一次方向判定不做死区比较（此时没有正在跑的步态）。
	if (!bBossLocoPlayingJog)
	{
		PlayingLocoLocalDir = FVector2D(0.0, 0.0);
	}

	UE_LOG(LogDemoCombat, Log,
		TEXT("[Boss][动画] 切换位移动画：实测位移速度=%.0f 倍速用速度=%.0f 意图=%d 物理移动=%d cm/s（进Jog %.0f / 退Jog %.0f）→ %s（序列 %s）"),
		DebugMeasuredSpeed, SmoothedLocoSpeed, DebugMoveIntent ? 1 : 0, DebugPhysMoving ? 1 : 0,
		BossJogSpeedThreshold, BossJogSpeedThreshold * LocoJogExitScale,
		bBossLocoPlayingJog ? TEXT("行走 Jog") : TEXT("待机 Idle"), *Desired->GetName());
}
