#include "Player/FightPlayerCharacter.h"
#include "Player/LockOnComponent.h"
#include "Abilities/DemoGameplayAbility.h"
#include "Abilities/PlayerAbilities.h"
#include "Core/DemoCommon.h"
#include "Core/DemoCombatTuning.h"
#include "Core/DemoGameplayTags.h"
#include "UI/CombatHUDWidget.h"
#include "Combat/DemoHitCameraShake.h"
#include "Camera/PlayerCameraManager.h"
#include "UObject/UObjectGlobals.h"

#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/PlayerController.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundBase.h"
#include "Engine/OverlapResult.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Components/SkeletalMeshComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	// 输入资产兜底加载：蓝图里忘了填槽位时，按 /Game/FightPlayer/Input/ 的
	// 约定路径直接把资产取回来，避免"按键完全没反应"这种静默失败。
	// 蓝图里显式填的值优先级更高（会覆盖这里的默认值）。
	template <typename TAsset>
	TObjectPtr<TAsset> LoadInputAssetFallback(const TCHAR* AssetName)
	{
		const FString Path = FString::Printf(TEXT("/Game/FightPlayer/Input/%s.%s"), AssetName, AssetName);
		ConstructorHelpers::FObjectFinderOptional<TAsset> Finder(*Path);
		return Finder.Get();
	}

	// 锁定相机诊断开关（默认关闭）：控制台 `Demo.LockOn.CameraDebug 1` 打开。
	// 排查"锁定抖 / 不居中"用 —— 打印距离 / 臂长 / 肩位 / 焦点 / 解算前后朝向 / 角色 yaw / 滞后开关。
	// 有这些量就能一眼判断到底是【焦点在抖】【角色 yaw 在抖】还是【解算/插值在抖】。
	TAutoConsoleVariable<int32> CVarLockOnCameraDebug(
		TEXT("Demo.LockOn.CameraDebug"), 0,
		TEXT("1 = 每 0.1s 打印锁定相机关键量（排查抖动/不居中）"),
		ECVF_Default);
}

AFightPlayerCharacter::AFightPlayerCharacter()
{
	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = 380.f;
	// 锁定 / 自由两种模式都由 UpdateLockOnCamera 手动把弹簧臂世界朝向钉死，
	// 不再依赖 bUsePawnControlRotation：否则弹簧臂每帧根据【控制朝向】重设旋转，
	// 会和锁定组件每帧改【角色朝向】、以及本函数显式 SetWorldRotation 三方互拽，
	// 表现为"锁定了相机不跟 Boss / 俯角越漂越大 / 攻击时画面乱抖"。
	CameraBoom->bUsePawnControlRotation = false;
	CameraBoom->SetRelativeLocation(FVector(0.f, 60.f, 70.f));

	// 弹簧臂的旋转独立于父组件（角色胶囊）。
	// 锁定组件每帧都会把角色转向 BOSS，而弹簧臂是胶囊的子组件 ——
	// 若继续继承父级旋转，角色一转身就会把镜头一起拽过去（每帧被叠加一次），
	// 表现为"镜头被甩 / 追不上 / 停下后还在飘"。改成绝对旋转后，位置仍跟着角色走，
	// 但朝向只由 UpdateLockOnCamera 决定（自由模式跟鼠标，锁定模式对准 BOSS）。
	CameraBoom->SetUsingAbsoluteRotation(true);

	// 相机“软跟随”：角色快速位移（闪避/前冲）时镜头平滑地追上去，而不是硬邦邦地瞬移贴住。
	// 跟得太软（值小）会让人觉得"按键后镜头/角色慢半拍"，所以这里调高到 30 让操作更跟手。
	CameraBoom->bEnableCameraLag = true;
	CameraBoom->CameraLagSpeed = 30.f;
	CameraBoom->bEnableCameraRotationLag = false;

	// 锁定 / 突进贴脸时镜头不能穿进 BOSS 网格：开碰撞测试，探针通道用 Camera。
	// 默认 bDoCollisionTest 虽为真，但 ProbeChannel 在不同工程里可能被改过，
	// 这里显式锁成 Camera，避免弹簧臂缩进 BOSS 身体里。
	CameraBoom->bDoCollisionTest = true;
	CameraBoom->ProbeChannel = ECC_Camera;

	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false;

	LockOn = CreateDefaultSubobject<ULockOnComponent>(TEXT("LockOn"));

	// 记下"没有锁定、也没有镜头冲击"时的镜头基准。
	// 之后锁定镜头与镜头冲击都从这两个值出发，而不是各自写死 380 / 零向量 ——
	// 否则一旦有人在 BP 里调了臂长，这两个系统就会把它掰回去。
	// （BeginPlay 还会再采一次：蓝图子类的覆盖发生在构造之后，那时才是最终值。）
	CacheDefaultCameraRig();

	bUseControllerRotationYaw = false;
	// 横移(strafe)模式：身体平滑转向"控制器朝向"(镜头/锁定方向)，而不是转向"移动方向"。
	// 只有这样，按 A/D/S 才会真正朝左/右/后移动并播对应动画。
	// 注意：bUseControllerDesiredRotation 是 UCharacterMovementComponent 上的属性（不是 Character/APawn 的），
	//       且会被 bOrientRotationToMovement 覆盖，所以必须把它设为 false。
	if (UCharacterMovementComponent* MoveComp = GetCharacterMovement())
	{
		MoveComp->bUseControllerDesiredRotation = true;
		MoveComp->bOrientRotationToMovement = false;
		// 转向更快（720 → 1000）：按 A/D 横移 / 换方向时身体几乎立刻跟上，不会"按了慢半拍"。
		MoveComp->RotationRate = FRotator(0.f, 1000.f, 0.f);
	}

	TeamId = 0;

	// ---------- 输入资产兜底（蓝图未填时生效） ----------
	DefaultMappingContext = LoadInputAssetFallback<UInputMappingContext>(TEXT("IMC_FightPlayer"));
	MoveAction            = LoadInputAssetFallback<UInputAction>(TEXT("IA_Move"));
	LookAction            = LoadInputAssetFallback<UInputAction>(TEXT("IA_Look"));
	JumpAction            = LoadInputAssetFallback<UInputAction>(TEXT("IA_Jump"));
	RunAction             = LoadInputAssetFallback<UInputAction>(TEXT("IA_Run"));
	NormalAttackAction    = LoadInputAssetFallback<UInputAction>(TEXT("IA_NormalAttack"));
	AttackSkillAction     = LoadInputAssetFallback<UInputAction>(TEXT("IA_AttackSkill"));
	DefenseAction         = LoadInputAssetFallback<UInputAction>(TEXT("IA_Defense"));
	DodgeAction           = LoadInputAssetFallback<UInputAction>(TEXT("IA_Dodge"));
}

// 角色标签 → 对应 GA 蓝图子类资产路径。强制按路径加载，彻底摆脱 BP 配置依赖。
static const TMap<FGameplayTag, FString>& GetCombatAbilityPaths()
{
	static const TMap<FGameplayTag, FString> Map = {
		{ DemoTag::Ability_Attack_Normal,  TEXT("/Game/FightPlayer/Abilities/GA_NormalAttack.GA_NormalAttack_C") },
		{ DemoTag::Ability_Attack_Skill,   TEXT("/Game/FightPlayer/Abilities/GA_AttackSkill.GA_AttackSkill_C") },
		{ DemoTag::Ability_Defense_Parry,  TEXT("/Game/FightPlayer/Abilities/GA_Parry.GA_Parry_C") },
		{ DemoTag::Ability_Movement_Dodge, TEXT("/Game/FightPlayer/Abilities/GA_Dodge.GA_Dodge_C") },
		{ DemoTag::Ability_UseItem_Heal,   TEXT("/Game/FightPlayer/Abilities/GA_UseItemHeal.GA_UseItemHeal_C") },
	};
	return Map;
}

static TSubclassOf<UGameplayAbility> LoadAbilityBP(const TCHAR* PackagePath)
{
	if (UClass* Class = StaticLoadClass(UGameplayAbility::StaticClass(), nullptr, PackagePath))
	{
		return Class;
	}
	return nullptr;
}

void AFightPlayerCharacter::BeginPlay()
{
	Super::BeginPlay();

	// 蓝图子类对 CameraBoom 的覆盖到这里才生效，重新采一次默认镜头参数
	CacheDefaultCameraRig();

	// 命中音效：蓝图留空就按约定路径兜底；再拿不到就静音（缺资源不该淹没日志）。
	// 打击感里声音占一半 —— 但"没有音频资产"不该变成报错刷屏，静默降级即可。
	if (!HitSound)
	{
		HitSound = Cast<USoundBase>(StaticLoadObject(USoundBase::StaticClass(), nullptr,
			TEXT("/Game/FightPlayer/SFX/SC_PlayerHit.SC_PlayerHit")));
	}
	if (!HeavyHitSound)
	{
		HeavyHitSound = Cast<USoundBase>(StaticLoadObject(USoundBase::StaticClass(), nullptr,
			TEXT("/Game/FightPlayer/SFX/SC_PlayerHeavyHit.SC_PlayerHeavyHit")));
	}

	if (UCharacterMovementComponent* MoveComp = GetCharacterMovement())
	{
		MoveComp->MaxWalkSpeed = WalkSpeed;
	}

	// 强制补齐核心战斗技能：普攻 / 技能 / 弹刀(Parry) / 闪避。
	// 按资产路径加载蓝图子类授予，不依赖 BP 的 DefaultAbilities / 显式槽配置；
	// 即使 BP 全部留空或填错，攻击 / 闪避 / 弹刀也一定能放出（原“格挡/防御”已移除）。
	if (AbilitySystemComponent)
	{
		GrantCombatAbility(DemoTag::Ability_Attack_Normal,    UGA_PlayerNormalAttack::StaticClass());
		GrantCombatAbility(DemoTag::Ability_Attack_Skill,     UGA_PlayerAttackSkill::StaticClass());
		GrantCombatAbility(DemoTag::Ability_Defense_Parry,    UGA_PlayerParry::StaticClass());
		GrantCombatAbility(DemoTag::Ability_Movement_Dodge,   UGA_PlayerDodge::StaticClass());
		// 回血"酒"：喝一口回 30% 最大生命（UGE_HealEffect + SetByCaller）。
		GrantCombatAbility(DemoTag::Ability_UseItem_Heal,     UGA_PlayerUseHeal::StaticClass());
		UE_LOG(LogTemp, Warning, TEXT("[Fight] 已强制授予核心技能：普攻 / 技能 / 弹刀 / 闪避 / 喝酒回血（已移除格挡）"));
	}

	// 回血"酒"开局数量：以平衡表为准，避免和蓝图 CDO 默认值打架。
	HealItemMaxCount = DemoBalance::HealItemMaxCount;
	HealItemCount = FMath::Min(DemoBalance::HealItemInitialCount, HealItemMaxCount);

	AddDefaultMappingContext();

	// 根位移模式自愈：本项目必须处于 RootMotionFromMontagesOnly。
	// 若动画实例（AnimBP 的 Class Defaults）被设成 NoRootMotionExtraction，
	// 蒙太奇的根骨骼不会被重置 → 攻击/闪避的位移只会推着 Mesh 滑、播完弹回（“拉回”），
	// 而胶囊体其实没动。出招时还会再调一次（幂等），双保险。
	EnsureRootMotionExtraction(TEXT("BeginPlay"));
}

void AFightPlayerCharacter::GrantCombatAbility(const FGameplayTag& RoleTag, TSubclassOf<UGameplayAbility> NativeFallback)
{
	if (!AbilitySystemComponent)
	{
		return;
	}

	// 先移除已存在的同角色技能（避免 BP 里填了原生类导致重复授予）
	TArray<FGameplayAbilitySpecHandle> ToRemove;
	for (const FGameplayAbilitySpec& Spec : AbilitySystemComponent->GetActivatableAbilities())
	{
		// 5.7：Spec.Ability 是 TObjectPtr<UGameplayAbility>（实例），GetClass() 取类
		if (const UGameplayAbility* Inst = Spec.Ability.Get())
		{
			if (const UDemoGameplayAbility* Def = Cast<UDemoGameplayAbility>(Inst->GetClass()->GetDefaultObject()))
			{
				if (Def->GetAssetTags().HasTag(RoleTag))
				{
					ToRemove.Add(Spec.Handle);
				}
			}
		}
	}
	for (const FGameplayAbilitySpecHandle& H : ToRemove)
	{
		AbilitySystemComponent->ClearAbility(H);
	}

	// 优先用蓝图子类（BP 里可配蒙太奇 / 参数），加载不到则退回原生 C++ 类
	TSubclassOf<UGameplayAbility> ClassToGrant = nullptr;
	if (const FString* BPPath = GetCombatAbilityPaths().Find(RoleTag))
	{
		ClassToGrant = LoadAbilityBP(**BPPath);
	}
	if (!ClassToGrant)
	{
		ClassToGrant = NativeFallback;
	}
	if (ClassToGrant)
	{
		AbilitySystemComponent->GiveAbility(FGameplayAbilitySpec(ClassToGrant, 1, INDEX_NONE, this));
	}
}

void AFightPlayerCharacter::AddDefaultMappingContext()
{
	if (bMappingContextAdded || !DefaultMappingContext)
	{
		return;
	}

	APlayerController* PC = Cast<APlayerController>(GetController());
	if (!PC)
	{
		return;
	}

	ULocalPlayer* LocalPlayer = PC->GetLocalPlayer();
	if (!LocalPlayer)
	{
		return;
	}

	if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
		ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(LocalPlayer))
	{
		Subsystem->AddMappingContext(DefaultMappingContext, 0);
		bMappingContextAdded = true;
		UE_LOG(LogTemp, Log, TEXT("[Fight] 已注册输入映射 %s"), *GetNameSafe(DefaultMappingContext));
	}
}

void AFightPlayerCharacter::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);

	if (AbilitySystemComponent)
	{
		AbilitySystemComponent->InitAbilityActorInfo(this, this);
	}

	// 关卡启动时 Possess 早于 BeginPlay，这里补一次注册
	AddDefaultMappingContext();
}

// ================================================================ 镜头冲击
void AFightPlayerCharacter::ApplyCameraPunch(float Length, float Duration)
{
	// 锁定时画面要绝对稳定（锁定本身就是为了"钉住 BOSS"），命中/弹反的镜头冲击一律不触发，
	// 否则连段时镜头会一直抖，看着像"锁了但没锁住"。
	if (IsLockedOn())
	{
		return;
	}

	if (!CameraBoom || Duration <= 0.f || Length <= 0.f)
	{
		return;
	}

	bCameraPunchActive = true;

	// 取较大的强度：连续弹反时重置计时（冲得更久），但不会把镜头越拉越近
	CameraPunchLength = FMath::Max(CameraPunchLength, Length);
	CameraPunchDuration = FMath::Max(Duration, KINDA_SMALL_NUMBER);
	CameraPunchRemaining = CameraPunchDuration;
}

void AFightPlayerCharacter::ClearCameraPunch()
{
	if (CameraBoom)
	{
		// 基准是 RestingArmLength（锁定时它已经被改成锁定臂长了），不是写死的 380
		CameraBoom->TargetArmLength = RestingArmLength;
	}

	bCameraPunchActive = false;
	CameraPunchRemaining = 0.f;
	CameraPunchLength = 0.f;
}

void AFightPlayerCharacter::PlayHitImpactShake(float Damage, float Duration)
{
	// ⚠ 锁定时【也】做命中微震：这道是叠加在 FollowCamera 相对旋转上的"旋转微震"，
	// sin 自衰减、结束精确归零，不碰弹簧臂也不碰控制朝向 —— 不会让锁定视角漂走。
	// 之前为避免"锁定时画面乱动"把它一并屏蔽了，结果锁定打 BOSS 时相机零反馈、
	// 变成"伤害在掉但没砍到感觉"。现在相机已改成单一手动控制（不再三方抢方向盘），
	// 这道旋转微震是安全的，锁定时照样给"砸实了"的镜头回馈。
	// （注意：ApplyCameraPunch 的"拉近式臂长冲击"仍只在非锁定时触发，
	//  因为它会改 TargetArmLength，和锁定臂长打架；旋转微震才是锁定安全的那一种。）
	if (Duration <= 0.f)
	{
		return;
	}

	// 伤害越大、震得越狠，但夹住上限：轻击(28)≈1.4°、重击(132)≈3.0°(封顶)，
	// 连段叠起来是节奏感，不会让镜头甩得看不清战场。
	const float Scale = FMath::Clamp(Damage * DemoBalance::HitImpactShakeDamageScale,
		DemoBalance::HitImpactShakeMinDeg, DemoBalance::HitImpactShakeMaxDeg);

	// 偏航给满、俯仰给少一点：制造"被击中一瞬的轻微错位"而不是上下点头。
	AddCameraShake(Scale, Scale * 0.6f, Duration);
}

void AFightPlayerCharacter::PlayHitScreenShake(float Intensity)
{
	const APlayerController* PC = Cast<APlayerController>(Controller);
	if (!PC || !PC->PlayerCameraManager || Intensity <= 0.f)
	{
		return;
	}

	// StartCameraShake 的 Scale 会等比缩放震荡振幅（时长不变）：
	// 轻刀小震、重刀大震，由调用方（HUD 命中回调）按命中类型传值。
	PC->PlayerCameraManager->StartCameraShake(UDemoHitCameraShake::StaticClass(), Intensity);
}

void AFightPlayerCharacter::PlayHitSound(bool bHeavy, const FVector& Location)
{
	USoundBase* Sound = bHeavy ? HeavyHitSound.Get() : HitSound.Get();
	if (!Sound)
	{
		// 没有音频资产就静音：视觉通道（顿帧 / 镜头 / 飘字 / BOSS 闪白）已经足够读，
		// 这里绝不能 warn —— 否则每次命中刷一条日志，真正的问题会被淹没。
		return;
	}

	// 位置落在【命中点】而不是角色身上：连段里玩家一直在动，用角色位置会让声音
	// 从"人这边"飘出来，而画面上火花在 BOSS 身上 —— 声画分离就没有打击感了。
	const FVector At = Location.IsNearlyZero() ? GetActorLocation() : Location;
	UGameplayStatics::PlaySoundAtLocation(this, Sound, At);
}

void AFightPlayerCharacter::PlayParryFeedback(const FVector& ContactLocation, bool bPerfect)
{
	// 只狼式弹刀的镜头反馈：一下【短促而轻】的拉近。
	//
	// 为什么这么短：连弹一段三连击时，如果每次弹刀都来一记长镜头冲击，
	// 镜头会一直在抖 —— 玩家反而读不出"下一招什么时候到"，
	// 而读招正是弹刀的前提。所以宁可轻一点，也不要抢戏。
	//
	// ApplyCameraPunch 内部对 Length 取 Max 且完成后归零，
	// 所以连续弹刀不会把镜头越拉越近。
	ApplyCameraPunch(
		bPerfect ? DemoBalance::ParryPerfectCameraPunchLength : DemoBalance::ParryCameraPunchLength,
		DemoBalance::ParryCameraPunchDuration);
}

void AFightPlayerCharacter::UpdateCameraPunch(float DeltaTime)
{
	if (!CameraBoom)
	{
		return;
	}

	if (!bCameraPunchActive)
	{
		// 没有冲击时，相机臂长由「静息值」说了算 —— 锁定镜头刚刚每帧更新过它。
		CameraBoom->TargetArmLength = RestingArmLength;
		return;
	}

	CameraPunchRemaining -= DeltaTime;

	if (CameraPunchRemaining <= 0.f)
	{
		// 收尾必须精确还原：靠曲线自然趋近会在最后留下零点几厘米的误差，
		// 玩久了镜头会一点点漂走。
		CameraBoom->TargetArmLength = RestingArmLength;
		bCameraPunchActive = false;
		CameraPunchLength = 0.f;
		return;
	}

	const float PunchProgress = 1.f - (CameraPunchRemaining / CameraPunchDuration);

	// sin 曲线：起步迅速冲到峰值（撞击感），再平滑回落（回弹）。
	// 换成线性的话，前半段和后半段一样"软"，看起来就像镜头在慢慢飘。
	const float Curve = FMath::Sin(PunchProgress * PI);

	// 永远是「静息臂长 - 冲击量」，所以锁定的远镜头和冲击叠加时不会互相抵消
	CameraBoom->TargetArmLength = RestingArmLength - CameraPunchLength * Curve;

	// ---- 命中相机微震：叠在 FollowCamera 的"相对旋转"上（不碰弹簧臂/控制朝向，零累积）----
	// 玩家命中 BOSS / 玩家被重击各触发一次；sin 自衰减，结束归零。
	CameraShakeRemaining -= DeltaTime;
	if (CameraShakeRemaining > 0.f)
	{
		const float Progress = 1.f - (CameraShakeRemaining / FMath::Max(CameraShakeDuration, KINDA_SMALL_NUMBER));
		const float Env = FMath::Sin(Progress * PI);
		if (FollowCamera)
		{
			FollowCamera->SetRelativeRotation(FRotator(CameraShakePitch * Env, CameraShakeYaw * Env, 0.f));
		}
	}
	else if (FollowCamera && (CameraShakeYaw != 0.f || CameraShakePitch != 0.f))
	{
		FollowCamera->SetRelativeRotation(FRotator::ZeroRotator);
		CameraShakeYaw = 0.f;
		CameraShakePitch = 0.f;
	}

	// ---- 受击 FOV 脉冲：Boss 突进命中瞬间短暂扩 FOV 再回弹（"砸脸"的镜头冲击）----
	CameraFOVPulseRemaining -= DeltaTime;
	if (CameraFOVPulseRemaining > 0.f)
	{
		if (APlayerController* PC = Cast<APlayerController>(GetController()))
		{
			if (!bFOVPulseCached && PC->PlayerCameraManager)
			{
				CachedDefaultFOV = PC->PlayerCameraManager->GetFOVAngle();
				bFOVPulseCached = true;
			}
			const float Progress = 1.f - (CameraFOVPulseRemaining / FMath::Max(CameraFOVPulseDuration, KINDA_SMALL_NUMBER));
			const float Env = FMath::Sin(Progress * PI);
			if (PC->PlayerCameraManager)
			{
				PC->PlayerCameraManager->SetFOV((bFOVPulseCached ? CachedDefaultFOV : 90.f) + CameraFOVPulseAmount * Env);
			}
		}
	}
	else if (bFOVPulseCached)
	{
		if (APlayerController* PC = Cast<APlayerController>(GetController()))
		{
			if (PC->PlayerCameraManager)
			{
				PC->PlayerCameraManager->SetFOV(CachedDefaultFOV);
			}
		}
		bFOVPulseCached = false;
	}
}

void AFightPlayerCharacter::AddCameraShake(float YawDeg, float PitchDeg, float Duration)
{
	if (Duration <= 0.f)
	{
		return;
	}

	// 取较大值并刷新计时：连续命中不会把镜头越抖越离谱，但能让抖动"续命"
	CameraShakeYaw = FMath::Max(CameraShakeYaw, YawDeg);
	CameraShakePitch = FMath::Max(CameraShakePitch, PitchDeg);
	CameraShakeDuration = FMath::Max(CameraShakeDuration, Duration);
	CameraShakeRemaining = CameraShakeDuration;
}

void AFightPlayerCharacter::PulseCameraFOV(float DeltaDeg, float Duration, bool bAllowWhileLocked)
{
	// 默认锁定时不脉冲：受击时视角稳定优先（用户明确要求过"锁定别乱动"）。
	// 但【玩家命中敌人】要传 bAllowWhileLocked=true —— FOV 不碰臂长也不碰朝向，
	// 是锁定安全的冲击通道，不给的话锁定打 BOSS 就完全没有镜头反馈 = "砍在棉花上"。
	if (!bAllowWhileLocked && IsLockedOn())
	{
		return;
	}

	if (Duration <= 0.f)
	{
		return;
	}

	CameraFOVPulseAmount = FMath::Max(CameraFOVPulseAmount, DeltaDeg);
	CameraFOVPulseDuration = FMath::Max(CameraFOVPulseDuration, Duration);
	CameraFOVPulseRemaining = CameraFOVPulseDuration;
}

// ================================================================ 锁定
bool AFightPlayerCharacter::IsLockedOn() const
{
	return LockOn && LockOn->IsLockedOn();
}

void AFightPlayerCharacter::ToggleLockOn()
{
	if (!LockOn)
	{
		return;
	}

	LockOn->ToggleLockOn();

	// 换目标 / 解锁时把手动偏移清零：不然新目标会带着上一个目标拖出来的偏角，
	// 看起来就像"锁上了但镜头偏在一边"。
	LockOnViewYawOffset = 0.f;
	LockOnViewPitchOffset = 0.f;

	UE_LOG(LogDemoCombat, Log, TEXT("[LockOn] %s"),
		IsLockedOn() ? TEXT("已锁定") : TEXT("已解除锁定"));
}

void AFightPlayerCharacter::SetLockOnEnabled(bool bEnabled)
{
	if (!LockOn)
	{
		return;
	}

	if (bEnabled)
	{
		LockOn->EnableLockOn();
	}
	else
	{
		LockOn->DisableLockOn();
	}
}

void AFightPlayerCharacter::AddLockOnViewOffset(float DeltaYaw, float DeltaPitch)
{
	if (!bAllowManualViewWhileLocked)
	{
		return;
	}

	LockOnViewYawOffset = FMath::Clamp(LockOnViewYawOffset + DeltaYaw, -LockOnManualYawLimit, LockOnManualYawLimit);
	LockOnViewPitchOffset = FMath::Clamp(LockOnViewPitchOffset + DeltaPitch, -LockOnManualPitchLimit, LockOnManualPitchLimit);
}

void AFightPlayerCharacter::UpdateLockOnInput()
{
	const APlayerController* PC = Cast<APlayerController>(GetController());
	if (!PC)
	{
		return;
	}

	// 这里用「自己记上一帧状态」而不是 WasInputKeyJustPressed：
	// 左Shift 那条兜底通道已经和 Enhanced Input 抢过一次按键状态，
	// 把 WasInputKeyJustPressed 用在中键上时行为不稳定（同一帧被两边消费）。
	// 自己维护一个边沿检测最稳，也不依赖任何 IMC 配置。
	const bool bLockOnNow = PC->IsInputKeyDown(LockOnKey);
	if (bLockOnNow && !bLockOnKeyHeld)
	{
		ToggleLockOn();
	}
	bLockOnKeyHeld = bLockOnNow;

	if (!IsLockedOn() || !LockOn)
	{
		// 没锁定时也持续同步按键状态，避免"锁定期间按住了键、再解锁时立刻又触发一次"
		bSwitchLeftHeld = PC->IsInputKeyDown(SwitchTargetLeftKey);
		bSwitchRightHeld = PC->IsInputKeyDown(SwitchTargetRightKey);
		return;
	}

	const bool bLeftNow = PC->IsInputKeyDown(SwitchTargetLeftKey);
	const bool bRightNow = PC->IsInputKeyDown(SwitchTargetRightKey);

	if (bLeftNow && !bSwitchLeftHeld)
	{
		LockOn->SwitchTarget(/*bToRight=*/false);
		LockOnViewYawOffset = 0.f;
	}
	else if (bRightNow && !bSwitchRightHeld)
	{
		LockOn->SwitchTarget(/*bToRight=*/true);
		LockOnViewYawOffset = 0.f;
	}

	bSwitchLeftHeld = bLeftNow;
	bSwitchRightHeld = bRightNow;
}

void AFightPlayerCharacter::UpdateLockOnCamera(float DeltaTime)
{
	if (!CameraBoom)
	{
		return;
	}

	const AActor* Target = IsLockedOn() ? LockOn->GetLockedTarget() : nullptr;

	// ---- 滞后与锁定互斥（"锁定疯狂抖动 / 没锁在身体中心"的根因之一，2026-09-26）----
	//
	// 下面锁定态的朝向是用【解析模型】解算的：
	//     相机 = 枢轴 + 朝向×臂长 + 朝向旋转后的 SocketOffset
	// 但弹簧臂一旦开着 CameraLag，相机【真实位置】就滞后于这个模型 —— 两者对不上，
	// 于是"让 BOSS 精确居中"的解算结果不成立，BOSS 会偏离画面中心。
	//
	// 而且位置误差 e 换算成角度 ≈ atan(e / 到焦点距离)：**距离越小放大越狠**。
	// 这就是"贴身打疯狂晃、拉远反而正常"的原因，也解释了"没锁在 BOSS 身体中心"。
	// 位移辅助（闪避）期间由它自己管滞后（它已经会临时关掉），这里不插手，避免互相覆盖。
	if (CameraBoom && !bCameraStateSavedByDisplacement)
	{
		CameraBoom->bEnableCameraLag = (Target != nullptr) ? false : CachedDefaultCameraLag;
	}

	// 锁定期间关闭弹簧臂碰撞检测（"出招贴脸时镜头疯狂抖"的根因之一）。
	// 出招突进到 BOSS 跟前时，臂长线会戳进 BOSS 胶囊/场景，碰撞把相机瞬间往回顶一下 → 抖动。
	// 锁定相机是解析模型手动钉位，本就不该再被碰撞推一次；自由模式（没锁）才需要碰撞避免穿墙。
	if (CameraBoom)
	{
		CameraBoom->bDoCollisionTest = (Target == nullptr);
	}

	// ---- 1) 静息镜头参数：锁定 / 未锁定之间平滑过渡 ----
	//
	// ⚠ 锁定臂长【不要】跟着距离自适应 —— 那是我上一轮基于错误前提加的，现已撤销。
	//
	// 原因（2026-09-27 直接查引擎源码确认）：`USpringArmComponent` 把相机放在枢轴【后方】——
	//     DesiredLoc -= DesiredRot.Vector() * TargetArmLength;   // SpringArmComponent.cpp:185
	//     DesiredLoc += FRotationMatrix(DesiredRot).TransformVector(SocketOffset);
	// 而本函数的解算模型一直写成"枢轴 + 朝向×臂长"（符号反了）。
	// 于是所有"相机会越过 BOSS / 臂长越长越危险"的推论全反了：
	//   **相机在后方 ⇒ 臂长越大，相机离 BOSS 越远**，固定臂长本来就安全；
	//   反倒是"贴近时收缩臂长"把相机朝【自己身体里】拽 → 近裁剪面切掉自身模型
	//   （用户反馈的"我的角色慢慢消失"）。
	const float DesiredArm = Target ? LockOnArmLength : CachedDefaultArmLength;
	const FVector DesiredOffset = Target ? LockOnSocketOffset : CachedDefaultSocketOffset;

	RestingArmLength = FMath::FInterpTo(RestingArmLength, DesiredArm, DeltaTime, LockOnCameraBlendSpeed);
	RestingSocketOffset = FMath::VInterpTo(RestingSocketOffset, DesiredOffset, DeltaTime, LockOnCameraBlendSpeed);
	CameraBoom->SocketOffset = RestingSocketOffset;

	const APlayerController* PC = Cast<APlayerController>(GetController());
	if (!PC)
	{
		return;
	}

	if (!Target)
	{
		// 自由模式：相机跟【控制朝向】（鼠标看向）。弹簧臂不再用 bUsePawnControlRotation，
		// 这里手动把世界朝向钉到"控制朝向 + 玩家微调偏移"，松手自动回中。
		LockOnViewYawOffset = FMath::FInterpTo(LockOnViewYawOffset, 0.f, DeltaTime, LockOnViewReturnSpeed);
		LockOnViewPitchOffset = FMath::FInterpTo(LockOnViewPitchOffset, 0.f, DeltaTime, LockOnViewReturnSpeed);

		FRotator Desired = PC->GetControlRotation();
		Desired.Yaw += LockOnViewYawOffset;
		Desired.Pitch += LockOnViewPitchOffset;
		Desired.Roll = 0.f;
		const FRotator Smoothed = FMath::RInterpTo(CameraBoom->GetComponentRotation(), Desired, DeltaTime, LockOnRotationSpeed);
		CameraBoom->SetWorldRotation(Smoothed);
		CameraBaseRotation = Smoothed;
		return;
	}

	// ---- 2) 锁定模式：解算"让 BOSS 焦点精确落在画面正中"的相机朝向 ----
	//
	// ⚠ 不能直接拿【枢轴 → 焦点】的方向当相机朝向。弹簧臂把相机放在
	//      相机 = 枢轴 + 朝向*臂长 + 朝向旋转后的 SocketOffset
	//    锁定用的 SocketOffset 是 (0, +85, +95)，也就是把相机从"臂的视线"上挪开
	//    85cm 右 / 95cm 上。臂长 480 时这就是 atan(95/480) ≈ 11° 俯仰 + atan(85/480) ≈ 10° 偏航
	//    —— 相机朝向对了，但相机本体被挪走了，BOSS 于是落在画面中心的【下方 + 侧边】，
	//    玩家看到的正是"锁定框在 BOSS 身上、可 BOSS 不在画面中心 / 像没锁到"。
	//
	// 解法：迭代解算。给定朝向 R 就有一个确定的相机位置，让 R 对准"该位置 → 焦点"，
	//    重复几次即收敛（SocketOffset 相对距离很小，3 次足够，误差 < 0.2°）。
	//    这样肩位偏移照旧（玩家不被自己的模型挡住），但 BOSS 精确居中。
	//    用解析模型而不是 FollowCamera 的真实位置，是为了不受 CameraLag 软跟随的滞后影响。
	const FVector FocusReal = LockOn->GetFocusLocation();
	const FVector BoomOrigin = CameraBoom->GetComponentLocation() + CameraBoom->TargetOffset;

	// 距离一律用【真实】焦点：肩位偏移是按距离缩放的（见 LockOnShoulderAngleDeg 注释），
	// 拿预测点算距离会让它在绕背过程中忽长忽短，把之前好不容易修掉的抖动又引回来。
	const float DistToFocus = FVector::Dist(FocusReal, BoomOrigin);

	// ---- 绕背预测：只影响"朝向解算"，不影响距离与肩位 ----
	// 玩家横向绕着 BOSS 跑时，把焦点沿速度方向外推一小段，让相机"预判"去向，
	// 于是绕到侧背面时 BOSS 不会被甩到画面边缘（"锁了但没锁住"的典型观感）。
	FVector FocusPoint = FocusReal;
	{
		FVector TargetOffset = FVector::ZeroVector;

		// 角度为 0 = 功能关闭 → 瞄准点恒等于 BOSS 身体中心（绝对居中，最稳的默认值）
		if (LockOnPredictAngleDeg > 0.f)
		{
			const FVector Vel2D = FVector(GetVelocity().X, GetVelocity().Y, 0.f);
			const float Speed2D = Vel2D.Size();
			if (Speed2D > LockOnPredictMinSpeed)
			{
				const FVector VelDir = Vel2D / Speed2D;
				const FVector ToFocusDir = (FocusReal - GetActorLocation()).GetSafeNormal2D();
				const float CosAng = FVector::DotProduct(VelDir, ToFocusDir);
				const float AngDeg = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(CosAng, -1.f, 1.f)));

				// smoothstep 渐变：夹角越大越接近横向绕行，外推【平滑】升到满值。
				// 用渐变而不是 if 硬开关，是因为夹角会在阈值附近来回穿越，
				// 硬开关会让镜头在 0 与满值之间反复跳（贴身缠斗时尤其明显）。
				float Alpha = FMath::Clamp(
					(AngDeg - LockOnPredictStartAngle) / FMath::Max(LockOnPredictRampAngle, 1.f), 0.f, 1.f);
				Alpha = Alpha * Alpha * (3.f - 2.f * Alpha);   // smoothstep

				if (Alpha > 0.f)
				{
					// 关键：按【恒定角度】限制，而不是按厘米数。
					// 这样贴身（距离小）时外推自动变小、远距离自动变大，偏角恒定 → 不会抖，
					// 也保证瞄准点始终落在 BOSS 身体上，不会被推出去。
					const float MaxByAngle = DistToFocus
						* FMath::Tan(FMath::DegreesToRadians(LockOnPredictAngleDeg));
					const float LeadLen = FMath::Min(Speed2D * LockOnPredictLeadTime, MaxByAngle);
					TargetOffset = VelDir * (LeadLen * Alpha);
				}
			}
		}

		// 逐帧平滑：方向和大小一起插值，起步/停步/换向都不会跳
		CurrentPredictOffset = FMath::VInterpTo(
			CurrentPredictOffset, TargetOffset, DeltaTime, LockOnPredictBlendSpeed);
		FocusPoint = FocusReal + CurrentPredictOffset;
	}

	// 臂长（解算与实际渲染共用）—— 提前取出来，下面的肩位补偿要用到它
	const float ArmLen = FMath::Max(CameraBoom->TargetArmLength, 1.f);

	// 肩位偏移按【恒定夹角】缩放（详见 LockOnShoulderAngleDeg 的注释）。
	//
	// ⚠⚠ 缩放用的距离必须是【相机→焦点】，不是【枢轴→焦点】D。
	//   补偿角 = atan(偏移 / 相机到焦点的距离)。相机在枢轴【后方】ArmLen 处（引擎源码见上），
	//   而 BOSS 在枢轴前方 D 处 ⇒ **相机到焦点 = D + ArmLen**（加法）。
	//
	//   （上一轮我按错误的方向模型写成了 D − ArmLen，并由此推出"贴身时分母趋零、角度爆炸"——
	//    那个结论本身是错的：真正的原因是方向符号反了。现在几何正确，
	//    相机永远比枢轴离 BOSS 更远，补偿角在任何距离下都只有几度，±14° 安全闸根本不会参与。）
	const float CamToFocus = FMath::Max(DistToFocus + ArmLen, 50.f);
	const float ShoulderMag = FMath::Min(LockOnSocketOffset.Size(),
		CamToFocus * FMath::Tan(FMath::DegreesToRadians(LockOnShoulderAngleDeg)));
	const FVector TargetSocketOff = LockOnSocketOffset.GetSafeNormal() * ShoulderMag;

	// 平滑过渡，避免锁定/解锁瞬间肩位跳变。解算用【刚写进去的这个真实值】，保证自洽。
	CameraBoom->SocketOffset = FMath::VInterpTo(CameraBoom->SocketOffset, TargetSocketOff, DeltaTime, LockOnCameraBlendSpeed);

	const FVector SocketOff = CameraBoom->SocketOffset;

	FRotator Desired = CameraBoom->GetComponentRotation();
	for (int32 Iter = 0; Iter < 3; ++Iter)
	{
		// 朝向 R 下相机的真实世界位置。
		// ⚠ 这里是【减】ArmLen —— 引擎的真实公式就是减：
		//     SpringArmComponent.cpp:185  DesiredLoc -= DesiredRot.Vector() * TargetArmLength;
		//     同文件:187                  DesiredLoc += FRotationMatrix(DesiredRot).TransformVector(SocketOffset);
		//   相机在枢轴【后方】。之前写成加法，解算出来的朝向并没有真的对准焦点 ——
		//   残余误差正是"锁定后 BOSS 始终偏一点、没锁在身体中心"的来源。
		const FVector CamPos = BoomOrigin - Desired.Vector() * ArmLen + Desired.RotateVector(SocketOff);
		const FVector ToFocus = FocusPoint - CamPos;
		if (ToFocus.IsNearlyZero())
		{
			return;
		}
		Desired = ToFocus.Rotation();
		Desired.Roll = 0.f;
	}

	// 保险闸：解算结果不得偏离"枢轴→焦点"的直接瞄准线太多。
	// 万一目标贴到脸上（距离接近 0）或参数被调得离谱，也不会甩出一个荒唐的角度。
	{
		const FRotator DirectAim = (FocusPoint - BoomOrigin).Rotation();
		FRotator Delta = Desired - DirectAim;
		Delta.Normalize();
		Delta.Pitch = FMath::Clamp(Delta.Pitch, -LockOnShoulderAngleDeg * 2.f, LockOnShoulderAngleDeg * 2.f);
		Delta.Yaw = FMath::Clamp(Delta.Yaw, -LockOnShoulderAngleDeg * 2.f, LockOnShoulderAngleDeg * 2.f);
		Delta.Roll = 0.f;
		Desired = DirectAim + Delta;
		Desired.Normalize();
	}

	// 俯仰限位（先加手动俯仰再夹，否则永远顶不到边界）
	Desired.Pitch = FMath::Clamp(Desired.Pitch + LockOnViewPitchOffset, LockOnMinPitch, LockOnMaxPitch);
	Desired.Yaw += LockOnViewYawOffset;

	// ---- 诊断输出（默认关闭，控制台 `Demo.LockOn.CameraDebug 1` 打开）----
	// 用来定位"抖动 / 不居中"到底出在哪一层：焦点在抖？角色 yaw 在抖？还是解算/插值在抖？
	if (CVarLockOnCameraDebug.GetValueOnGameThread() != 0)
	{
		static float LockOnDebugLastTime = -100.f;
		const float NowTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
		if (NowTime - LockOnDebugLastTime >= 0.1f)
		{
			LockOnDebugLastTime = NowTime;
			const FRotator Cur = CameraBoom->GetComponentRotation();
			UE_LOG(LogTemp, Warning,
				TEXT("[LockOnCam] D=%.0f arm=%.0f sock=%.1f focus=(%.0f,%.0f,%.0f) boom=(%.0f,%.0f,%.0f) des(P=%.1f,Y=%.1f) cur(P=%.1f,Y=%.1f) charYaw=%.1f lag=%d pred=%.1f"),
				DistToFocus, ArmLen, SocketOff.Size(),
				FocusPoint.X, FocusPoint.Y, FocusPoint.Z,
				BoomOrigin.X, BoomOrigin.Y, BoomOrigin.Z,
				Desired.Pitch, Desired.Yaw, Cur.Pitch, Cur.Yaw,
				GetActorRotation().Yaw,
				CameraBoom->bEnableCameraLag ? 1 : 0,
				CurrentPredictOffset.Size());
		}
	}

	// 从【相机当前世界朝向】直接插值到目标朝向，完全不碰控制朝向 / 角色朝向，
	// 所以不会再和锁定组件改角色朝向、或 bUsePawnControlRotation 抢方向盘 —— 锁定即稳定跟随。
	const FRotator Smoothed = FMath::RInterpTo(CameraBoom->GetComponentRotation(), Desired, DeltaTime, LockOnRotationSpeed);
	CameraBoom->SetWorldRotation(Smoothed);
	CameraBaseRotation = Smoothed;

	// ---- 同步【控制器朝向】：让 WASD 的移动基准跟着镜头走 ----
	//
	// ⚠ UE 的 AddMovementInput 是用【控制器朝向】换算世界方向的
	//   （HandleMove 里就是 `Controller->GetControlRotation().Yaw`）。
	//   而锁定后相机是用 CameraBoom->SetWorldRotation 直接钉住的，控制器朝向【从没被更新】——
	//   于是 WASD 仍按"锁定前那个朝向"换算：画面已经转过去了，按 W 却往别的方向跑。
	//   这里每帧把控制器朝向同步成相机朝向，WASD 方向就永远等于屏幕方向。
	//
	// 附带修掉两个副作用：
	//   · 解锁瞬间相机接回 PC->GetControlRotation() 时不会跳（两者本来就是同一个值）；
	//   · LockOnComponent 筛选候选目标用的 ViewForward 也取自控制器朝向，现在同步了才准。
	// 只在锁定时同步：自由模式下控制器朝向由鼠标驱动，反过来才是对的。
	if (APlayerController* LockedPC = Cast<APlayerController>(GetController()))
	{
		LockedPC->SetControlRotation(Smoothed);
	}

	// ---- 3) 手动偏移松手回中 ----
	LockOnViewYawOffset = FMath::FInterpTo(LockOnViewYawOffset, 0.f, DeltaTime, LockOnViewReturnSpeed);
	LockOnViewPitchOffset = FMath::FInterpTo(LockOnViewPitchOffset, 0.f, DeltaTime, LockOnViewReturnSpeed);
}

void AFightPlayerCharacter::CacheDefaultCameraRig()
{
	if (!CameraBoom)
	{
		return;
	}

	// 构造函数里存的只是"类默认值"。这里在 BeginPlay 再取一次：
	// 蓝图子类可能覆盖过 CameraBoom 的 ArmLength / SocketOffset，
	// 而覆盖发生在构造之后 —— 不重新取的话，蓝图的调参会被 380 顶掉。
	CachedDefaultArmLength = CameraBoom->TargetArmLength;
	CachedDefaultSocketOffset = CameraBoom->SocketOffset;
	CachedDefaultCameraLag = CameraBoom->bEnableCameraLag;

	RestingArmLength = CachedDefaultArmLength;
	RestingSocketOffset = CachedDefaultSocketOffset;
}

void AFightPlayerCharacter::UpdateCombatSeparation(float DeltaTime)
{
	if (!bCombatSeparationEnabled || DeltaTime <= 0.f || IsDead())
	{
		return;
	}

	UCharacterMovementComponent* MoveComp = GetCharacterMovement();
	UWorld* World = GetWorld();
	if (!MoveComp || !World || !MoveComp->UpdatedComponent)
	{
		return;
	}

	// 查询半径留"一脚"的余量：正好等于分离半径的话，敌人只有在【已经贴上】的那一帧才被查到，
	// 推挤会变成一次性的急推（抖）。留 80cm 让它提前介入，推挤是渐进的、像被肩膀顶住。
	const float QueryRadius = CombatSeparationRadius + 80.f;
	if (QueryRadius <= KINDA_SMALL_NUMBER)
	{
		return;
	}

	TArray<FOverlapResult> Overlaps;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(DemoCombatSeparation), /*bTraceComplex=*/false, this);

	// ⚠ 必须用 ECC_Pawn 的【Overlap】查询：Pawn 通道对 Pawn 是 Overlap 不是 Block，
	//   这既是"近战能查到敌人"的原因（PerformMeleeTrace 也走它），
	//   也正是"胶囊体之间不会互相挡、玩家能一路走进 BOSS 身体里"的根因。
	//   Sweep 在这里什么都查不到（Sweep 只返回 Blocking 命中）。
	World->OverlapMultiByChannel(Overlaps, GetActorLocation(), FQuat::Identity,
		ECC_Pawn, FCollisionShape::MakeSphere(QueryRadius), Params);

	for (const FOverlapResult& Hit : Overlaps)
	{
		ADemoCharacterBase* Other = Cast<ADemoCharacterBase>(Hit.GetActor());
		if (!Other || Other == this || Other->IsDead())
		{
			continue;
		}
		// 只约束敌对目标：友军 / 中立不该被推开，否则队友会把玩家挤得到处跑。
		if (!IsHostileTo(Other))
		{
			continue;
		}

		// 只算【水平】距离。垂直方向不做约束：站台阶 / 坡地时 Z 差几十厘米很正常，
		// 把它算进距离会让玩家在坡地上被莫名弹开。
		FVector Delta = GetActorLocation() - Other->GetActorLocation();
		Delta.Z = 0.f;

		float Dist = Delta.Size();
		FVector OutDir;   // 由敌人指向自己 = 该被推开的方向
		if (Dist > KINDA_SMALL_NUMBER)
		{
			OutDir = Delta / Dist;
		}
		else
		{
			// 中心完全重合（瞬移 / 重叠生成后偶发）：给一个确定方向，绝不能出 NaN。
			OutDir = GetActorForwardVector().GetSafeNormal2D();
			if (OutDir.IsNearlyZero())
			{
				OutDir = FVector::ForwardVector;
			}
			Dist = 0.f;
		}

		if (Dist >= CombatSeparationRadius)
		{
			continue;
		}

		// ---- ① 先削掉"继续往对方身上挤"的速度分量 ----
		//
		// 不做这一步的话，玩家的移动输入会和下面的位置回推互相打架：
		// 一帧被推出来、下一帧又被输入推进去 → 贴着敌人抖成一团。
		// 削掉之后"往前推"这个输入在这个方向上直接失效，
		// 玩家的手感是"顶住了、进不去"，而不是"被弹簧弹回来"。
		const float InwardSpeed = FVector::DotProduct(MoveComp->Velocity, -OutDir);
		if (InwardSpeed > 0.f)
		{
			MoveComp->Velocity += OutDir * InwardSpeed;
		}

		// ---- ② 再把已经侵入的部分推回去 ----
		//
		// 每帧最多推 PushSpeed × DeltaTime：不是"瞬间弹飞"（那是挨打该有的表现），
		// 而是被稳定地顶在边界上。带 sweep → 不会被推进墙里 / 推下悬崖。
		const float Penetration = CombatSeparationRadius - Dist;
		const float Step = FMath::Min(Penetration, CombatSeparationPushSpeed * DeltaTime);
		if (Step <= 0.05f)
		{
			continue;
		}

		FHitResult MoveHit;
		MoveComp->MoveUpdatedComponent(OutDir * Step,
			MoveComp->UpdatedComponent->GetComponentQuat(), /*bSweep=*/true, &MoveHit);
	}
}

void AFightPlayerCharacter::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// 顺序有讲究：先定「静息镜头」（锁定会改臂长 / 肩位），再让镜头冲击在它之上做减法。
	// 反过来的话，冲击结束还原时会把锁定的臂长覆盖掉 —— 表现就是"弹反之后镜头忽然缩回默认距离"。
	UpdateLockOnInput();
	UpdateUseItemInput();
	UpdateLockOnCamera(DeltaTime);
	UpdateCameraPunch(DeltaTime);

	// ---- 移动锁看门狗（防"永久僵住"的最后一道兜底）----
	// 移动锁已是引用计数，正常情况一定会归零。但只要有【任何一个】技能加了锁却没配对减掉
	//（新加的技能忘了减、异常路径提前 EndAbility、条件分支漏了一处），
	// 计数就永远 > 0 → 玩家彻底动不了，而且没有任何提示 —— 这是最难排查的"僵住"。
	// 这里加一条独立的超时：锁住超过 MoveLockStaleTimeout 秒还没有任何技能在活动，
	// 就强制清零并打 Warning。宁可多放一次（下一帧某个技能会重新锁），
	// 也不能让玩家永久卡死。
	{
		if (MoveInputLockCount > 0)
		{
			// 有任何激活中的战斗技能在跑，就认为锁是合法的，继续计时
			// ⚠ TArray 没有 .Any()，用普通循环（写成 lambda 反而更容易踩到这个编译错误）
			bool bAnyAbilityActive = false;
			if (const UAbilitySystemComponent* ASC = GetAbilitySystemComponent())
			{
				for (const FGameplayAbilitySpec& Spec : ASC->GetActivatableAbilities())
				{
					if (Spec.ActiveCount > 0)
					{
						bAnyAbilityActive = true;
						break;
					}
				}
			}
			if (bAnyAbilityActive)
			{
				StaleMoveLockTimer = 0.f;
			}
			else
			{
				StaleMoveLockTimer += DeltaTime;
				if (StaleMoveLockTimer >= MoveLockStaleTimeout)
				{
					UE_LOG(LogDemoCombat, Warning,
						TEXT("[MoveLock] ⚠⚠ 没有任何技能在活动，但移动锁仍为 %d 层（持续 %.1fs）→ 强制解锁")
						TEXT("（说明某个技能加了锁没配对解锁）"),
						MoveInputLockCount, StaleMoveLockTimer);
					MoveInputLockCount = 0;
					StaleMoveLockTimer = 0.f;
					if (UCharacterMovementComponent* MoveComp = GetCharacterMovement())
					{
						MoveComp->MaxWalkSpeed = GetDesiredMaxWalkSpeed();
					}
				}
			}
		}
		else
		{
			StaleMoveLockTimer = 0.f;
		}
	}

	// ---- 战斗间距（软碰撞）：不许走进敌人身体里 ----
	// 放在镜头之后：它只动胶囊体和速度，不碰任何镜头状态，顺序上和谁都不冲突。
	UpdateCombatSeparation(DeltaTime);

	// ---- 压制动画根位移期间每帧复查 ----
	// 只要有一次被别的东西（AnimBP、其他技能）把 RootMotionMode 改回去，
	// 动画自带的根位移就会重新参与运算 → 表现为“位移忽然变长 / 被拉回”。
	// 每帧兜一次，成本极低（只在出招那一小段内生效），但把这个坑彻底堵死。
	if (bRootMotionModeCached)
	{
		EnsureDodgeRootMotionSuppressed();
	}

	// ---- 左Shift 兜底通道：直接轮询物理按键，完全不依赖 IMC ----
	// 为什么需要它：UE5.7 的 IMC 真实映射数据在 DefaultKeyMappings（旧的 Mappings 已废弃、
	// 运行时不再读取）。一旦编辑器脚本没跑成功，左Shift 可能仍绑在别的 Input Action 上，
	// Enhanced Input 的绑定就收不到事件 → 表现为“按 Shift 闪避没反应”。
	// 这里读控制器记录的原始按键状态（UEnhancedPlayerInput::InputKey 会先调用父类，
	// 所以即使走了 Enhanced Input，原始状态依然被记录），保证闪避一定能触发。
	// 与 Enhanced Input 的绑定互为兜底；同一次按下的重复事件由 bShiftHeld 去重。
	if (const APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		if (PC->WasInputKeyJustPressed(DodgeRunKey))
		{
			HandleRunStart();
		}
		else if (PC->WasInputKeyJustReleased(DodgeRunKey))
		{
			HandleRunEnd();
		}
	}
}

// ------------------------------------------------------------------ 输入绑定
void AFightPlayerCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	UEnhancedInputComponent* EnhancedInput = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (!EnhancedInput)
	{
		UE_LOG(LogTemp, Error,
			TEXT("[Fight] 输入组件不是 UEnhancedInputComponent（当前 %s）。"
				 "检查 Config/DefaultInput.ini 的 DefaultInputComponentClass。"),
			*GetNameSafe(PlayerInputComponent ? PlayerInputComponent->GetClass() : nullptr));
		return;
	}

	// 收集没配置的槽位，一次性报出来——静默跳过是"按键完全没反应"最常见的原因
	TArray<FString> Missing;
	auto Require = [&Missing](const UInputAction* Action, const TCHAR* SlotName)
	{
		if (!Action) { Missing.Add(SlotName); }
		return Action != nullptr;
	};

	if (Require(MoveAction, TEXT("MoveAction")))
	{
		EnhancedInput->BindAction(MoveAction, ETriggerEvent::Triggered, this, &AFightPlayerCharacter::HandleMove);
		// 松开方向键 → 清空“最近移动输入”，这样闪避才能做到“没按方向键就向前闪”
		EnhancedInput->BindAction(MoveAction, ETriggerEvent::Completed, this, &AFightPlayerCharacter::HandleMoveReleased);
	}
	if (Require(LookAction, TEXT("LookAction")))
	{
		EnhancedInput->BindAction(LookAction, ETriggerEvent::Triggered, this, &AFightPlayerCharacter::HandleLook);
	}
	if (Require(JumpAction, TEXT("JumpAction")))
	{
		EnhancedInput->BindAction(JumpAction, ETriggerEvent::Started, this, &AFightPlayerCharacter::HandleJump);
	}
	// ---- 左Shift：短按闪避 / 长按奔跑 ----
	// 同一套分流逻辑同时挂在 3 个 Input Action 上，谁在 IMC 里绑到了 Shift 都能生效：
	//   IA_Run    —— 期望映射（Tools/SetupInputRemap.py 会把 LeftShift 绑到它）
	//   IA_Flash  —— 历史遗留：早期 IMC 里 LeftShift 绑的就是它
	//   IA_Dodge  —— 备用闪避键
	// 原因：UE5.7 的 IMC 真实映射在 DefaultKeyMappings（旧 Mappings 属性已废弃且运行时不再读取），
	//       脚本一旦没跑成功，Shift 就会继续停在 IA_Flash 上 —— 多挂几个就能兜住“按了没反应”。
	// 同一次按下的重复事件由 HandleRunStart / HandleRunEnd 内部按 bShiftHeld 去重。
	auto BindShiftLike = [&](const UInputAction* Action, const TCHAR* SlotName)
	{
		if (!Require(Action, SlotName))
		{
			return;
		}
		EnhancedInput->BindAction(Action, ETriggerEvent::Started,   this, &AFightPlayerCharacter::HandleRunStart);
		EnhancedInput->BindAction(Action, ETriggerEvent::Completed, this, &AFightPlayerCharacter::HandleRunEnd);
		// Canceled 也要接：如果 IMC 里该映射带了 Hold 之类的触发器，
		// 短按松开会走 Canceled 而不是 Completed，不接就会丢掉“短按闪避”。
		EnhancedInput->BindAction(Action, ETriggerEvent::Canceled,  this, &AFightPlayerCharacter::HandleRunEnd);
	};
	BindShiftLike(RunAction,   TEXT("RunAction"));
	// FlashAction 是历史遗留槽位（早期 IMC 里 LeftShift 绑的就是 IA_Flash）。
	// Tools/SetupInputRemap.py 已把 LeftShift 迁到 IA_Run，IA_Flash 不再绑键，
	// 所以这个槽位通常是空的 —— 它空着【不影响闪避】（RunAction/DodgeAction 已接管 Shift）。
	// 这里仍尝试绑定（将来有人重新填 IA_Flash 就能兜底），但空着不报警告，避免误报红字。
	if (FlashAction)
	{
		BindShiftLike(FlashAction, TEXT("FlashAction"));
	}
	else
	{
		UE_LOG(LogTemp, Verbose,
			TEXT("[Fight] FlashAction 槽位为空（历史遗留，IA_Flash 已不再绑键），跳过，不影响闪避。"));
	}
	BindShiftLike(DodgeAction, TEXT("DodgeAction"));

	if (Require(NormalAttackAction, TEXT("NormalAttackAction")))
	{
		EnhancedInput->BindAction(NormalAttackAction, ETriggerEvent::Started, this, &AFightPlayerCharacter::HandleNormalAttack);
	}
	if (Require(AttackSkillAction, TEXT("AttackSkillAction")))
	{
		EnhancedInput->BindAction(AttackSkillAction, ETriggerEvent::Started, this, &AFightPlayerCharacter::HandleAttackSkill);
	}
	if (Require(DefenseAction, TEXT("DefenseAction")))
	{
		// 鼠标右键 = 弹刀：按下即开启一次判定窗（抬剑招架 → 弹到则反击）
		EnhancedInput->BindAction(DefenseAction, ETriggerEvent::Started, this, &AFightPlayerCharacter::HandleDefenseStarted);
		EnhancedInput->BindAction(DefenseAction, ETriggerEvent::Completed, this, &AFightPlayerCharacter::HandleDefenseCompleted);
	}

	if (Missing.Num() > 0)
	{
		UE_LOG(LogTemp, Error,
			TEXT("[Fight] 以下输入槽位为空，对应按键不会生效：%s。"
				 "请在 BP_FightPlayer 的 Demo|Input 分类里补上。"),
			*FString::Join(Missing, TEXT(", ")));
	}
}

void AFightPlayerCharacter::HandleMove(const FInputActionValue& Value)
{
	const FVector2D Axis = Value.Get<FVector2D>();
	if (!Controller)
	{
		return;
	}

	// 松开方向键 → 立刻清空“最近移动输入”。
	// 否则闪避会复用上次按过的旧方向，出现“明明没按 WASD，却朝左边闪”的怪现象。
	if (Axis.IsNearlyZero())
	{
		ClearLastMoveInput();
		return;
	}

	// 普攻期间禁止 WASD 移动（技能用 SetMoveInputLocked 开关）。
	// 注意：解锁发生在“收招窗口”（GA_NormalAttack 的 ComboRecoverRatio，默认 0.70）——
	// 也就是最后 30% 的收招动作期间玩家就能立刻走，不用等整段播完 → 打完→移动的切换不再有滞涩感。
	if (IsMoveInputLocked())
	{
		return;
	}

	// 诊断：把 IA_Move 实际收到的 2D 值打出来（同一个值只打一次，最多 6 条）。
	// 期望值：W=(0,1)、S=(0,-1)、A=(-1,0)、D=(1,0)。
	// 如果四个方向都打出 (X=1.00, Y=0.00) → IMC_FightPlayer 里 IA_Move 的键没加
	// Swizzle Input Axis Values / Negate 修饰器（详见 Docs/Troubleshooting.md 症状 3）。
	if (LoggedMoveAxes.Num() < 6 && !LoggedMoveAxes.Contains(Axis))
	{
		LoggedMoveAxes.Add(Axis);
		UE_LOG(LogTemp, Log, TEXT("[Fight] IA_Move 输入 = (X=%.2f, Y=%.2f)"), Axis.X, Axis.Y);
	}

	const FRotator YawRotation(0.f, Controller->GetControlRotation().Yaw, 0.f);
	const FVector Forward = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X);
	const FVector Right = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y);

	AddMovementInput(Forward, Axis.Y);
	AddMovementInput(Right, Axis.X);

	SetLastMoveInput((Forward * Axis.Y + Right * Axis.X).GetSafeNormal());
}

void AFightPlayerCharacter::HandleLook(const FInputActionValue& Value)
{
	// 鼠标视角灵敏度：值越小转得越慢，越大越快。
	// 嫌快就调小（如 0.3），嫌慢就调大（如 0.8）。
	// 如果你之前在 IA_Look 上加过 Scalar 修饰器，请把它设回 1.0，避免双重缩放。
	static constexpr float LookSensitivity = 0.5f;
	const FVector2D Axis = Value.Get<FVector2D>() * LookSensitivity;

	// 锁定期间鼠标不再直接转控制朝向：
	// 控制朝向每帧都被"对准目标"覆盖，这里再 AddControllerYawInput 就是两边抢方向盘，
	// 表现是镜头被反复来回拽、鼠标怎么拖都被弹回去。
	// 改成喂给"手动偏移"，由 UpdateLockOnCamera 叠加到目标朝向上，松手自动回中。
	if (IsLockedOn())
	{
		AddLockOnViewOffset(-Axis.X * 60.f, -Axis.Y * 40.f);
		return;
	}

	AddControllerYawInput(Axis.X);
	AddControllerPitchInput(-Axis.Y);
}

void AFightPlayerCharacter::HandleJump()
{
	Jump();
}

void AFightPlayerCharacter::HandleRunStart()
{
	// 去重：同一物理键会同时命中多个 Input Action（IA_Run / IA_Flash / IA_Dodge 都挂了这套逻辑），
	// 保证一次“按下”只启动一次判定计时器。
	if (bShiftHeld)
	{
		return;
	}
	bShiftHeld = true;

	// 左Shift 按下：启动计时器。按住超过 RunHoldThreshold → 进入奔跑；
	// 在阈值前松开 → 触发闪避（见 HandleRunEnd）。
	// 即：左Shift = 短按闪避 / 长按奔跑；鼠标右键 = 弹刀。
	bShiftRunActive = false;

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(
			ShiftHoldTimerHandle,
			this,
			&AFightPlayerCharacter::OnShiftHoldExpired,
			RunHoldThreshold,
			/*bLoop=*/ false);
	}
}

void AFightPlayerCharacter::OnShiftHoldExpired()
{
	// 按住超过阈值 → 正式进入奔跑
	bShiftRunActive = true;

	// 攻击期间不动移速（技能正把 MaxWalkSpeed 压低做刹车）。
	// 只记录“玩家想跑”的状态，等攻击收招/结束时由技能按 GetDesiredMaxWalkSpeed() 还原。
	if (IsMoveInputLocked())
	{
		return;
	}

	if (UCharacterMovementComponent* MoveComp = GetCharacterMovement())
	{
		MoveComp->MaxWalkSpeed = RunSpeed;
	}
}

void AFightPlayerCharacter::HandleRunEnd()
{
	// 去重：Completed 和 Canceled 都可能到达（取决于 IMC 里该映射的触发器配置），
	// 没有配对的“按下”就说明是重复/回落事件，直接忽略，避免一次点按闪两次。
	if (!bShiftHeld)
	{
		return;
	}
	bShiftHeld = false;

	// 无论长按还是短按松开，先清掉“长按检测”计时器
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ShiftHoldTimerHandle);
	}

	// 长按后松开 → 停止奔跑
	if (bShiftRunActive)
	{
		bShiftRunActive = false;

		// 攻击期间不动移速（同上，交给技能在收招/结束时统一还原）
		if (IsMoveInputLocked())
		{
			return;
		}

		if (UCharacterMovementComponent* MoveComp = GetCharacterMovement())
		{
			// bShiftRunActive 已置 false → 这里就是走速
			MoveComp->MaxWalkSpeed = GetDesiredMaxWalkSpeed();
		}
		return;
	}

	// 短按松开（没到 RunHoldThreshold）→ 闪避：左Shift 点按 = 闪避
	// 攻击/弹刀进行中（移动被锁）时不响应，避免“按了没反应 + 刷一堆失败日志”。
	if (IsMoveInputLocked())
	{
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("[Dodge] 左Shift 短按 → 触发闪避"));
	TryActivateCombatAbility(ResolveAbilityByTag(DemoTag::Ability_Movement_Dodge));
}

void AFightPlayerCharacter::HandleNormalAttack()
{
	const TSubclassOf<UGameplayAbility> AbilityClass = ResolveAbilityByTag(DemoTag::Ability_Attack_Normal);

	// 连段技能已在运行时，把这次按键当作"接下一段"的输入事件
	if (IsAbilityActive(AbilityClass))
	{
		SendComboInput();
		return;
	}

	// 完美闪避派生：先取消闪避后摇，再起手普攻（"闪A"）
	TryPerfectDodgeFollowupCancel();

	TryActivateCombatAbility(AbilityClass);
}

void AFightPlayerCharacter::HandleAttackSkill()
{
	// 完美闪避派生：先取消闪避后摇，再起手技能
	TryPerfectDodgeFollowupCancel();

	TryActivateCombatAbility(ResolveAbilityByTag(DemoTag::Ability_Attack_Skill));
}

bool AFightPlayerCharacter::TryPerfectDodgeFollowupCancel()
{
	// 必须同时满足：正在闪避 + 处于完美闪避派生窗。
	// 否则普通闪避期间按攻击不会误触发派生（普通闪避没有 State_DodgePerfect）。
	const TSubclassOf<UGameplayAbility> DodgeClass = ResolveAbilityByTag(DemoTag::Ability_Movement_Dodge);
	if (!IsAbilityActive(DodgeClass) || !AbilitySystemComponent)
	{
		return false;
	}
	if (!AbilitySystemComponent->HasMatchingGameplayTag(DemoTag::State_DodgePerfect))
	{
		return false;
	}

	// 取消闪避：它的 EndAbility 会还原根位移/解锁移动/撤掉无敌帧，
	// 让普攻/技能能正常起手（闪避原本用 BlockAbilitiesWithTag 挡着攻击，取消后就放行了）。
	FGameplayTagContainer DodgeTags;
	DodgeTags.AddTag(DemoTag::Ability_Movement_Dodge);
	AbilitySystemComponent->CancelAbilities(&DodgeTags);

	UE_LOG(LogTemp, Log, TEXT("[Dodge] 完美闪避派生：取消闪避后摇，直接接招（闪A）"));
	return true;
}

void AFightPlayerCharacter::HandleDefenseStarted()
{
	const TSubclassOf<UGameplayAbility> AbilityClass = ResolveAbilityByTag(DemoTag::Ability_Defense_Parry);

	// 弹刀技能已经在跑 → 这次按键是【连弹】：补发一个"重开判定窗"事件。
	//
	// 为什么不能直接再 TryActivateAbility：技能是 InstancedPerActor，
	// 已经激活时 GAS 会直接拒掉第二次激活（日志只会看到"失败"）。
	// 于是弹刀成功后的收势期间按第二下右键石沉大海 ——
	// BOSS 的三段连击只能弹掉第一段，剩下的全吃。这就是"连弹弹不起来"的根因。
	if (IsAbilityActive(AbilityClass))
	{
		FGameplayEventData Payload;
		Payload.Instigator = this;
		Payload.Target = this;
		UAbilitySystemBlueprintLibrary::SendGameplayEventToActor(
			this, DemoTag::Event_Ability_ParryRewindow, Payload);
		return;
	}

	// 没在跑 → 正常起手：播抬剑动作 + 开判定窗（默认 0.30s）
	TryActivateCombatAbility(AbilityClass);
}

void AFightPlayerCharacter::HandleDefenseCompleted()
{
	// 弹刀是瞬时窗口、自动结束，无需取消；保留空实现以兼容 IA_Defense 的 Completed 事件
}

// ------------------------------------------------------------------ 道具：回血"酒"
bool AFightPlayerCharacter::TryConsumeHealItem()
{
	if (HealItemCount <= 0)
	{
		return false;
	}
	HealItemCount--;
	return true;
}

void AFightPlayerCharacter::LockMoveInput()
{
	++MoveInputLockCount;
	// ⚠ 重复 Lock 是调用方的 bug（同一技能加锁两次却只 Unlock 一次）→ 计数会永远不归零。
	// 所以这里主动兜底：同帧同技能重复加锁时打 Warning，方便一眼看出是谁多锁了。
	if (MoveInputLockCount > 1)
	{
		UE_LOG(LogDemoCombat, Warning,
			TEXT("[MoveLock] ⚠ 锁层数 = %d（上一个技能可能没解锁！）技能=%s"),
			MoveInputLockCount, *GetName());
	}
}

void AFightPlayerCharacter::UnlockMoveInput()
{
	// 幂等：同一个技能可能在【收招窗口】和【EndAbility】各调一次（那是有意的幂等），
	// 也可能因为异常路径多调一次。这里夹到 0，保证不会变成负数。
	if (MoveInputLockCount <= 0)
	{
		MoveInputLockCount = 0;
		return;
	}
	--MoveInputLockCount;
}

void AFightPlayerCharacter::HandleUseItem()
{
	TryActivateCombatAbility(ResolveAbilityByTag(DemoTag::Ability_UseItem_Heal));
}

void AFightPlayerCharacter::UpdateUseItemInput()
{
	const APlayerController* PC = Cast<APlayerController>(GetController());
	if (!PC)
	{
		return;
	}

	// 边沿检测（与 LockOnKey / SwitchTarget 同一套思路）：只在"按下那一帧"触发一次，
	// 避免按住 R 反复重喝。上一帧状态自己记，不依赖 WasInputKeyJustPressed（被多处消费会不稳定）。
	const bool bNow = PC->IsInputKeyDown(UseItemKey);
	if (bNow && !bUseItemKeyHeld)
	{
		HandleUseItem();
	}
	bUseItemKeyHeld = bNow;
}

void AFightPlayerCharacter::SendComboInput()
{
	FGameplayEventData Payload;
	Payload.Instigator = this;
	Payload.Target = this;
	UAbilitySystemBlueprintLibrary::SendGameplayEventToActor(this, DemoTag::Event_Ability_ComboInput, Payload);
}

TSubclassOf<UGameplayAbility> AFightPlayerCharacter::ResolveAbilityByTag(const FGameplayTag& AbilityTag) const
{
	// 1) 永远优先从“已授予的技能”里按标签匹配。
	//    只有真正被授予的技能才能激活，这样无论 BP 的显式槽 / DefaultAbilities
	//    怎么填错、填反、留空，都能命中可激活的类（根治“授予链错配”）。
	if (AbilitySystemComponent)
	{
		for (const FGameplayAbilitySpec& Spec : AbilitySystemComponent->GetActivatableAbilities())
		{
			// 5.7：Spec.Ability 是 TObjectPtr<UGameplayAbility>（实例），GetClass() 取类
			if (const UGameplayAbility* Inst = Spec.Ability.Get())
			{
				if (const UDemoGameplayAbility* Def = Cast<UDemoGameplayAbility>(Inst->GetClass()->GetDefaultObject()))
				{
					if (Def->GetAssetTags().HasTag(AbilityTag))
					{
						return Inst->GetClass();
					}
				}
			}
		}
	}

	// 2) 兜底：显式槽 / DefaultAbilities（极少数授予失败时的最后手段）
	if (AbilityTag == DemoTag::Ability_Attack_Normal && NormalAttackAbility)	{ return NormalAttackAbility; }
	if (AbilityTag == DemoTag::Ability_Attack_Skill && AttackSkillAbility)		{ return AttackSkillAbility; }
	if (AbilityTag == DemoTag::Ability_Defense_Block && DefenseAbility)			{ return DefenseAbility; }
	if (AbilityTag == DemoTag::Ability_Defense_Parry && ParryAbility)			{ return ParryAbility; }
	if (AbilityTag == DemoTag::Ability_Movement_Dodge && DodgeAbility)			{ return DodgeAbility; }

	for (const TSubclassOf<UDemoGameplayAbility>& AbilityClass : DefaultAbilities)
	{
		if (AbilityClass && AbilityClass->GetDefaultObject<UDemoGameplayAbility>()->GetAssetTags().HasTag(AbilityTag))
		{
			return AbilityClass;
		}
	}

	// 3) 终极兜底：按角色标签直接加载蓝图子类，保证一定能解析到可激活的类
	if (const FString* BPPath = GetCombatAbilityPaths().Find(AbilityTag))
	{
		if (TSubclassOf<UGameplayAbility> BP = LoadAbilityBP(**BPPath))
		{
			return BP;
		}
	}

	UE_LOG(LogTemp, Warning, TEXT("[Fight] ResolveAbilityByTag：未找到匹配 %s 的已授予技能（确认 BP 的 DefaultAbilities / 显式槽，或 BeginPlay 自动授予是否生效）"), *AbilityTag.ToString());
	return nullptr;
}

// ------------------------------------------------------------------ 工具
bool AFightPlayerCharacter::IsAbilityActive(TSubclassOf<UGameplayAbility> AbilityClass) const
{
	if (!AbilitySystemComponent || !AbilityClass)
	{
		return false;
	}

	if (const FGameplayAbilitySpec* Spec = AbilitySystemComponent->FindAbilitySpecFromClass(AbilityClass))
	{
		return Spec->IsActive();
	}
	return false;
}

bool AFightPlayerCharacter::IsNormalAttacking() const
{
	return IsAbilityActive(ResolveAbilityByTag(DemoTag::Ability_Attack_Normal));
}

void AFightPlayerCharacter::HandleMoveReleased()
{
	// 松开方向键就清掉“最近移动输入”，闪避读到的就是“无输入 → 默认向前”
	ClearLastMoveInput();
}

void AFightPlayerCharacter::SetIgnoreAnimRootMotion(bool bIgnore)
{
	UAnimInstance* AnimInstance = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr;
	if (!AnimInstance)
	{
		return;
	}

	if (bIgnore)
	{
		if (!bRootMotionModeCached)
		{
			CachedRootMotionMode = AnimInstance->RootMotionMode;
			bRootMotionModeCached = true;
			UE_LOG(LogTemp, Log, TEXT("[Dodge] 原 RootMotionMode=%d，闪避期间切到 IgnoreRootMotion(3)"),
				static_cast<int32>(CachedRootMotionMode.GetValue()));
		}

		// IgnoreRootMotion = “把根位移抽出来、但不施加” → 动画在原地播，
		// 角色位移完全交给代码（闪避技能里的 DodgeDistance），距离精确可控、不会翻倍、不会拉回。
		if (AnimInstance->RootMotionMode != ERootMotionMode::IgnoreRootMotion)
		{
			AnimInstance->SetRootMotionMode(ERootMotionMode::IgnoreRootMotion);
		}
	}
	else if (bRootMotionModeCached)
	{
		// 还原时兜一道：NoRootMotionExtraction 会让“根骨骼不重置（位移留在 Mesh 上）
		// + 蒙太奇位移不进胶囊体”，正是“位移像动画、播完拉回”的成因，
		// 本项目一律不允许停留在该模式。
		ERootMotionMode::Type Restore = CachedRootMotionMode.GetValue();
		if (Restore == ERootMotionMode::NoRootMotionExtraction)
		{
			Restore = ERootMotionMode::RootMotionFromMontagesOnly;
			UE_LOG(LogTemp, Warning,
				TEXT("[RootMotion] 原模式是 NoRootMotionExtraction，还原为 RootMotionFromMontagesOnly(1)"
					 "（否则动画位移会留在 Mesh 上、播完弹回）"));
		}
		AnimInstance->SetRootMotionMode(Restore);
		bRootMotionModeCached = false;
	}
}

int32 AFightPlayerCharacter::GetAnimRootMotionMode() const
{
	const UAnimInstance* AnimInstance = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr;
	return AnimInstance ? static_cast<int32>(AnimInstance->RootMotionMode.GetValue()) : -1;
}

void AFightPlayerCharacter::EnsureRootMotionExtraction(const TCHAR* Reason)
{
	UAnimInstance* AnimInstance = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr;
	if (!AnimInstance)
	{
		UE_LOG(LogTemp, Warning, TEXT("[RootMotion] %s：拿不到动画实例，跳过（Mesh 或 AnimBP 缺失？）"), Reason);
		return;
	}

	const int32 Before = static_cast<int32>(AnimInstance->RootMotionMode.GetValue());
	if (AnimInstance->RootMotionMode == ERootMotionMode::RootMotionFromMontagesOnly)
	{
		UE_LOG(LogTemp, Log, TEXT("[RootMotion] %s：根位移模式已是 RootMotionFromMontagesOnly(1)"), Reason);
		return;
	}

	// RootMotionFromMontagesOnly：
	//   - 蒙太奇（攻击 / 闪避 / 弹刀）的根位移会被提取 → 真实推动胶囊体（不会拉回）；
	//   - 同时根骨骼在姿势里被重置 → 动画不会推着 Mesh 滑出去；
	//   - 走 / 跑的根运动动画不参与（等效原地播，角色不会自己漂移）。
	AnimInstance->SetRootMotionMode(ERootMotionMode::RootMotionFromMontagesOnly);

	UE_LOG(LogTemp, Warning,
		TEXT("[RootMotion] %s：根位移模式 %d → RootMotionFromMontagesOnly(1)。"
			 "（0=Everything 1=MontagesOnly 2=NoExtraction 3=Ignore；"
			 "此前的模式会让蒙太奇位移留在 Mesh 上 → 看着在动、播完弹回，即“拉回”）"),
		Reason, Before);
}

// ------------------------------------------------------------------ 蒙太奇根位移硬屏蔽
bool AFightPlayerCharacter::SuppressMontageRootMotion(UAnimMontage* Montage)
{
	UAnimInstance* AnimInstance = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr;
	if (!AnimInstance)
	{
		return false;
	}

	// 指定了蒙太奇就精确取它的实例；没指定就取当前活跃的那段（闪避期间就是它）
	FAnimMontageInstance* Instance = Montage
		? AnimInstance->GetActiveInstanceForMontage(Montage)
		: AnimInstance->GetActiveMontageInstance();

	if (!Instance)
	{
		return false;   // 蒙太奇这一帧还没真正开始播 → Tick 里下一帧再补
	}

	if (RootMotionSuppressedInstance == Instance)
	{
		return true;    // 已经挂过，不要重复挂（计数式 API，重复挂会漏 Pop）
	}

	RestoreMontageRootMotion();   // 挂到别的实例上了：先把旧的摘干净，避免计数泄漏

	Instance->PushDisableRootMotion();
	RootMotionSuppressedInstance = Instance;
	RootMotionSuppressedAnimInstance = AnimInstance;

	// 注意：不能直接写 `Montage ? Montage : Instance->Montage` —— 裸指针与 TObjectPtr 混用
	// 会让条件表达式类型有歧义（C2445），先取成同一种类型。
	UAnimMontage* Suppressed = Montage ? Montage : Instance->Montage.Get();
	UE_LOG(LogTemp, Log, TEXT("[Dodge] 已屏蔽该蒙太奇自身的根位移（%s）"),
		*GetNameSafe(Suppressed));
	return true;
}

void AFightPlayerCharacter::RestoreMontageRootMotion()
{
	if (!RootMotionSuppressedInstance)
	{
		return;
	}

	if (UAnimInstance* AnimInstance = RootMotionSuppressedAnimInstance.Get())
	{
		// 只在“这个实例还活着”时 Pop：蒙太奇实例被销毁时计数随之消失，
		// 若对着新实例 Pop 会把它变成 -1（后续判断全部失真）。
		for (FAnimMontageInstance* Inst : AnimInstance->MontageInstances)
		{
			if (Inst == RootMotionSuppressedInstance)
			{
				RootMotionSuppressedInstance->PopDisableRootMotion();
				break;
			}
		}
	}

	RootMotionSuppressedInstance = nullptr;
	RootMotionSuppressedAnimInstance = nullptr;
}

void AFightPlayerCharacter::EnsureDodgeRootMotionSuppressed()
{
	// 第 1 层：AnimInstance 的根位移模式（挡住所有根位移的施加）
	SetIgnoreAnimRootMotion(true);

	// 第 2 层：蒙太奇实例级开关。引擎 AnimInstance.cpp 里判定根位移提取的第一条件就是
	//         !MontageInstance->IsRootMotionDisabled()，所以这一层比 ABP 的 Root Motion Mode 更硬。
	//         蒙太奇实例是“开始播之后”才建好的，窗口期一帧，这里没挂上就等下一帧。
	if (!RootMotionSuppressedInstance)
	{
		SuppressMontageRootMotion(nullptr);
	}
}

void AFightPlayerCharacter::BeginDisplacementAssist()
{
	++DisplacementAssistDepth;
	if (DisplacementAssistDepth > 1)
	{
		// 已经有技能开着辅助（例如普攻与闪避重叠）：共用同一份状态，不要重复记录起点/改相机
		return;
	}

	DisplacementAssistStartLocation = GetActorLocation();

	// 镜头：与角色刚性同步（视角跟着角色走）。
	// 位移期间若还留着 CameraLag，画面会变成“角色先窜出去、镜头再慢慢追上”，
	// 角色在屏幕上先偏离中心再回到中心 —— 看起来就像位移被“拉回”，其实只是镜头在追。
	if (CameraBoom && !bCameraStateSavedByDisplacement)
	{
		DisplacementAssistSavedCameraLagSpeed = CameraBoom->CameraLagSpeed;
		DisplacementAssistSavedCameraLagEnabled = CameraBoom->bEnableCameraLag;
		bCameraStateSavedByDisplacement = true;

		if (bRigidCameraDuringDisplacement)
		{
			CameraBoom->bEnableCameraLag = false;
		}
		else if (DisplacementCameraLagSpeed > 0.f)
		{
			CameraBoom->bEnableCameraLag = true;
			CameraBoom->CameraLagSpeed = DisplacementCameraLagSpeed;
		}
	}
}

float AFightPlayerCharacter::EndDisplacementAssist()
{
	if (DisplacementAssistDepth <= 0)
	{
		return 0.f;
	}

	--DisplacementAssistDepth;
	if (DisplacementAssistDepth > 0)
	{
		// 还有别的技能在用这份辅助：先不还原
		return 0.f;
	}

	// 实际位移（只取水平分量，垂直方向的落地抖动不算）
	const FVector Delta = GetActorLocation() - DisplacementAssistStartLocation;
	const float ActualDistance = FVector(Delta.X, Delta.Y, 0.f).Size();

	// 还原镜头
	if (CameraBoom && bCameraStateSavedByDisplacement)
	{
		bCameraStateSavedByDisplacement = false;
		CameraBoom->CameraLagSpeed = DisplacementAssistSavedCameraLagSpeed;
		CameraBoom->bEnableCameraLag = DisplacementAssistSavedCameraLagEnabled;
	}

	return ActualDistance;
}

bool AFightPlayerCharacter::TryActivateCombatAbility(TSubclassOf<UGameplayAbility> AbilityClass)
{
	if (!AbilitySystemComponent)
	{
		UE_LOG(LogTemp, Error, TEXT("[Fight] 激活失败：ASC 为空（BeginPlay/InitAbilityActorInfo 未执行？）"));
		return false;
	}
	if (!AbilityClass)
	{
		UE_LOG(LogTemp, Error, TEXT("[Fight] 激活失败：解析到的技能类为空（BP 没配 / 也没自动授予）"));
		return false;
	}

	const bool bOk = AbilitySystemComponent->TryActivateAbilityByClass(AbilityClass);
	UE_LOG(LogTemp, Log, TEXT("[Fight] 尝试激活 %s → %s"),
		*GetNameSafe(AbilityClass.Get()),
		bOk ? TEXT("成功") : TEXT("失败（未授予/冷却/消耗/标签阻挡/已在激活中）"));

	return bOk;
}

// ------------------------------------------------------------------ 表现
void AFightPlayerCharacter::OnDamagedReaction(EDemoHitDirection Direction, float Damage)
{
	if (HitReactMontage)
	{
		PlayAnimMontage(HitReactMontage);
	}

	// 受击 FOV 脉冲：BOSS 突进 / 收尾重击（HeavyImpact）命中玩家瞬间，短暂把 FOV 扩开再弹回，
	// 构成"砸脸"的镜头冲击 —— 这是 Spec 要求的"突进命中瞬间 FOV 脉冲(+3~5°, 0.1s 回弹)"。
	// 普通近战（Melee）不扩，否则每次挨打镜头都晃，反而读不清招。
	// LastDamageSource 是基类受击结算时记下的这一击来源，本函数由它之后调用，值已就绪。
	if (LastDamageSource == EDemoDamageSource::HeavyImpact)
	{
		PulseCameraFOV(DemoBalance::BossHeavyHitFOVPulse, DemoBalance::BossHeavyHitFOVPulseDuration);
	}
}

void AFightPlayerCharacter::OnDeathReaction()
{
	// 死了就把锁定解开：否则镜头会一直钉在 BOSS 身上，
	// 玩家看不到自己倒下的样子，也无法在复活后立刻自由转视角。
	SetLockOnEnabled(false);
	ClearCameraPunch();

	if (DeathMontage)
	{
		PlayAnimMontage(DeathMontage);
	}
}
