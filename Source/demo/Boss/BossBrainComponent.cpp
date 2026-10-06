#include "Boss/BossBrainComponent.h"

#include "Boss/BossCombatData.h"
#include "Boss/BossCharacterBase.h"
#include "Abilities/BossAbilities.h"
#include "Core/DemoCharacterBase.h"
#include "Core/DemoAbilitySystemLibrary.h"
#include "Core/DemoCombatTuning.h"
#include "Core/DemoGameplayTags.h"

#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystemComponent.h"
#include "AIController.h"
#include "Navigation/PathFollowingComponent.h"	// EPathFollowingRequestResult 完整枚举定义（AIController.h 只是前向声明）
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "DrawDebugHelpers.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"

// 按约定路径加载蒙太奇（Boss 的蒙太奇由 Tools/SetupBossMontages.py 生成）
static UAnimMontage* LoadBossMontage(const TCHAR* Path)
{
	if (!Path)
	{
		return nullptr;
	}
	return Cast<UAnimMontage>(StaticLoadObject(UAnimMontage::StaticClass(), nullptr, Path));
}

UBossBrainComponent::UBossBrainComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
}

void UBossBrainComponent::BeginPlay()
{
	Super::BeginPlay();

	// 没配阶段就用内置默认的两阶段（零配置也能打）
	EnsureDefaultPhases();
	ApplyPhaseSettings();

	SetState(EBossState::Idle);

	UE_LOG(LogDemoCombat, Log, TEXT("[Boss][Brain] 就绪：阶段数=%d 出招距离=%.0f 大招冷却=%.0fs"),
		Phases.Num(), AggroRange, GetCurrentPhase() ? GetCurrentPhase()->UltimateCooldown : 0.f);
}

ABossCharacterBase* UBossBrainComponent::GetBoss() const
{
	return Cast<ABossCharacterBase>(GetOwner());
}

UAbilitySystemComponent* UBossBrainComponent::GetBossASC() const
{
	const ABossCharacterBase* Boss = GetBoss();
	return Boss ? Boss->GetAbilitySystemComponent() : nullptr;
}

AAIController* UBossBrainComponent::GetAIController() const
{
	const APawn* OwnerPawn = Cast<APawn>(GetOwner());
	return OwnerPawn ? Cast<AAIController>(OwnerPawn->GetController()) : nullptr;
}

UBossPhaseData* UBossBrainComponent::GetCurrentPhase() const
{
	return Phases.IsValidIndex(CurrentPhaseIndex) ? Phases[CurrentPhaseIndex].Get() : nullptr;
}

float UBossBrainComponent::GetDistanceToTarget() const
{
	const AActor* Owner = GetOwner();
	if (!Owner || !CachedTarget)
	{
		return TNumericLimits<float>::Max();
	}
	return FVector::Dist(Owner->GetActorLocation(), CachedTarget->GetActorLocation());
}

float UBossBrainComponent::GetTelegraphProgress() const
{
	if (CurrentState != EBossState::Telegraph || CachedTelegraphTime <= KINDA_SMALL_NUMBER)
	{
		return 0.f;
	}

	return FMath::Clamp(StateTimer / CachedTelegraphTime, 0.f, 1.f);
}

bool UBossBrainComponent::CanCurrentAttackBeParried() const
{
	return CurrentAttack && CurrentAttack->bCanBeParried;
}

// ==================================================================================
// 主循环
// 状态名（心跳诊断用：BOSS 卡住时靠它看出卡在哪个状态）
static const TCHAR* BossStateName(EBossState State)
{
	switch (State)
	{
	case EBossState::Idle:				return TEXT("Idle待机");
	case EBossState::Chase:				return TEXT("Chase追击");
	case EBossState::Reposition:		return TEXT("Reposition绕位");
	case EBossState::Telegraph:			return TEXT("Telegraph起手");
	case EBossState::Attack:			return TEXT("Attack出招中");
	case EBossState::Recover:			return TEXT("Recover后摇");
	case EBossState::Stagger:			return TEXT("Stagger硬直");
	case EBossState::PhaseTransition:	return TEXT("PhaseTransition阶段转换");
	case EBossState::Dead:				return TEXT("Dead死亡");
	}
	return TEXT("未知状态");
}

// ==================================================================================
void UBossBrainComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	StateTimer += DeltaTime;
	PhaseElapsedTime += DeltaTime;
	UltimateCooldownRemaining = FMath::Max(UltimateCooldownRemaining - DeltaTime, 0.f);

	// ---- 心跳诊断（2026-09-30 加）：BOSS 卡在某个状态时，靠它一眼看出卡在哪 ----
	// 之前"打完第一招就再也不出招"完全没有日志，只能靠这条心跳定位。
	HeartbeatTimer += DeltaTime;
	if (HeartbeatTimer >= 1.5f)
	{
		HeartbeatTimer = 0.f;
		const UAbilitySystemComponent* ASC = GetBossASC();
		UE_LOG(LogDemoCombat, Warning,
			TEXT("[Boss][心跳] 状态=%s 已停留=%.1fs 距离=%.0f 出招间隔剩余=%.1fs 攻击标签=%d 硬直标签=%d 阶段=%d"),
			BossStateName(CurrentState), StateTimer,
			CachedTarget ? GetDistanceToTarget() : -1.f,
			AttackIntervalRemaining,
			(ASC && ASC->HasMatchingGameplayTag(DemoTag::State_Attacking)) ? 1 : 0,
			(ASC && ASC->HasMatchingGameplayTag(DemoTag::State_Stagger)) ? 1 : 0,
			CurrentPhaseIndex);
	}

	// 招式冷却递减（到 0 就删掉，避免 TMap 越积越大）
	for (auto It = AttackCooldowns.CreateIterator(); It; ++It)
	{
		It->Value -= DeltaTime;
		if (It->Value <= 0.f)
		{
			It.RemoveCurrent();
		}
	}

	ABossCharacterBase* Boss = GetBoss();
	if (!Boss)
	{
		return;
	}

	// ---------------- 目标维护 ----------------
	ADemoCharacterBase* Cached = Cast<ADemoCharacterBase>(CachedTarget.Get());
	if (!CachedTarget || !IsValid(CachedTarget) || (Cached && Cached->IsDead()))
	{
		CachedTarget = UGameplayStatics::GetPlayerPawn(GetWorld(), 0);
	}

	UAbilitySystemComponent* ASC = Boss->GetAbilitySystemComponent();

	// ---------------- 死亡优先一切 ----------------
	if (ASC && ASC->HasMatchingGameplayTag(DemoTag::State_Dead))
	{
		if (CurrentState != EBossState::Dead)
		{
			SetState(EBossState::Dead);
		}
		UpdateDebugDraw();
		return;
	}

	// ---------------- 阶段推进（演出进行中不打断自己）----------------
	if (CurrentState != EBossState::PhaseTransition)
	{
		UpdatePhase();
	}

	// ---------------- 硬直优先于出招 ----------------
	// 破防（韧性清零）/ 被弹反都会打上 State.Stagger
	if (ASC && ASC->HasMatchingGameplayTag(DemoTag::State_Stagger)
		&& CurrentState != EBossState::Stagger
		&& CurrentState != EBossState::PhaseTransition)
	{
		SetState(EBossState::Stagger);
	}

	switch (CurrentState)
	{
	case EBossState::Idle:				UpdateIdle(DeltaTime);				break;
	case EBossState::Chase:				UpdateChase(DeltaTime);				break;
	case EBossState::Reposition:		UpdateReposition(DeltaTime);		break;
	case EBossState::Telegraph:			UpdateTelegraph(DeltaTime);			break;
	case EBossState::Attack:			UpdateAttack(DeltaTime);			break;
	case EBossState::Recover:			UpdateRecover(DeltaTime);			break;
	case EBossState::Stagger:			UpdateStagger(DeltaTime);			break;
	case EBossState::PhaseTransition:	UpdatePhaseTransition(DeltaTime);	break;
	case EBossState::Dead:				StopMovement();						break;
	default: break;
	}

	UpdateDebugDraw();
}

void UBossBrainComponent::SetState(EBossState NewState)
{
	if (CurrentState == NewState)
	{
		return;
	}

	const EBossState OldState = CurrentState;
	CurrentState = NewState;
	StateTimer = 0.f;

	switch (NewState)
	{
	case EBossState::Chase:
	{
		ChaseTimer = 0.f;
		FollowUpTelegraphScale = 1.f;

		// 掷一次出招间隔。
		// ⚠ AttackIntervalMin/Max 之前只在阶段配置里写了、【从来没有人读】：
		// BOSS 打完一招只等招式自己的后摇（最短 0.55s）就立刻再打，
		// 于是冷却长的招（重砸 6~9s、突进斩 6s、扑击 5s）永远在冷却里，
		// 能用的只剩冷却最短的那一招 —— 画面上就是"一直在重复同一个动作"。
		const UBossPhaseData* ChasePhase = GetCurrentPhase();
		if (ChasePhase)
		{
			const float MinInterval = FMath::Max(ChasePhase->AttackIntervalMin, 0.f);
			const float MaxInterval = FMath::Max(ChasePhase->AttackIntervalMax, MinInterval);
			AttackIntervalRemaining = FMath::FRandRange(MinInterval, MaxInterval);
		}
		break;
	}

	case EBossState::Reposition:
	{
		if (AAIController* AI = GetAIController())
		{
			if (const AActor* Target = CachedTarget.Get())
			{
				// 绕到侧后方，而不是原地打转：看起来像"找角度"而不是"发呆"
				const FVector Offset(FMath::RandRange(-400.f, 400.f), FMath::RandRange(-400.f, 400.f), 0.f);
				AI->MoveToLocation(Target->GetActorLocation() + Offset, 60.f);
			}
		}
		break;
	}

	case EBossState::Telegraph:
		// ⚠⚠⚠ 这里【不再】StopMovement()（2026-10-02 第十八轮）
		//
		// 玩家需求："boss出招时不移动，在预警红光时跟随玩家，也要有移动动作动画"。
		// 原来进预警就站死，是因为怕"动来动去玩家读不出招"——
		// 但代价是预警期间 BOSS 完全静止、行走动画被 IsAttacking() 一起关掉，
		// 画面上就是"红光闪着但模型一动不动"，很呆，而且和玩家脱节。
		//
		// 现在改成：预警期间【继续跟随玩家移动 + 播行走动画】（压迫感，玩家能预判落点），
		// 真正挥出那一瞬间（Attack 状态）才由 DriveMoveNoNav 里的 IsStrikeActive() 定身。
		// 招式本身该有的位移仍然由它的根位移负责，不会和移动指令叠加。
		if (CurrentAttack)
		{
			// 缓存本次起手总时长，供 HUD 的预警条算进度（见头文件 CachedTelegraphTime 注释）。
			// 公式必须和 UpdateTelegraph 里判定出招的那一份完全一致，
			// 否则预警条走完的瞬间和真正出招的瞬间会差几十毫秒 —— 弹反就会时准时不准。
			const UBossPhaseData* Phase = GetCurrentPhase();
			const float PhaseScale = Phase ? Phase->TelegraphTimeScale : 1.f;
			CachedTelegraphTime = CurrentAttack->GetEffectiveTelegraphTime(IsPhase2())
				* PhaseScale * FollowUpTelegraphScale;

			OnAttackTelegraph.Broadcast(CurrentAttack);
		}
		break;

	case EBossState::Recover:
		// 进入后摇先决定"要不要接下一招"（只掷一次骰子，不是在每帧里反复掷）
		bFollowUpDecided = false;
		break;

	case EBossState::PhaseTransition:
		TransitionStage = -1;
		CurrentAttack = nullptr;
		PendingFollowUp = nullptr;
		FollowUpTelegraphScale = 1.f;
		AttackCooldowns.Empty();
		StopMovement();
		break;

	default:
		break;
	}

	OnStateChanged.Broadcast(OldState, NewState);
}

// ==================================================================================
// 各状态
// ==================================================================================
void UBossBrainComponent::UpdateIdle(float DeltaTime)
{
	if (!CachedTarget)
	{
		return;
	}

	if (GetDistanceToTarget() <= AggroRange)
	{
		SetState(EBossState::Chase);
	}
}

void UBossBrainComponent::UpdateChase(float DeltaTime)
{
	if (!CachedTarget)
	{
		SetState(EBossState::Idle);
		return;
	}

	ChaseTimer += DeltaTime;
	const float Distance = GetDistanceToTarget();

	// 太远：先跑过去（这个距离硬丢弹幕很出戏）
	if (Distance > MaxEngageRange)
	{
		MoveToTarget(150.f);
		return;
	}

	// 出招间隔：这段时间是给玩家的"对峙 / 走位窗口"，同时也是让各招冷却回落的时间。
	// 没有它，冷却长的招会被永久跳过，能用的只剩冷却最短的那一招 → BOSS 看起来只会一招。
	if (AttackIntervalRemaining > 0.f)
	{
		AttackIntervalRemaining -= DeltaTime;

		// 间隔里也在动：追近 / 后撤 / 绕圈，别站着发呆
		//
		// ⚠⚠ 为什么加"绕圈"（StrafeAroundTarget）：原来只有"追近"和"后撤"两条，
		//   而 MoveToTarget(150) 一旦追到 150cm 以内就直接 StopMovement 站住 ——
		//   于是 BOSS 走到交战距离后就【钉在原地站到下一次出招】。
		//   表现上就是"BOSS 几乎不走 / 移动动画看不到"（实测：一次 55 秒的跑测里
		//   待机 Idle 占 65%、行走 Jog 只占 3%）。
		//   绕圈让它在对峙窗口里一直挪动，既让 Jog 真的看得见，观感也从"站桩"变成"在找角度"。
		UBossPhaseData* ChasePhase = GetCurrentPhase();
		if (ChasePhase && ChasePhase->BackoffRange > 0.f && Distance < ChasePhase->BackoffRange)
		{
			MoveAwayFromTarget(ChasePhase->BackoffDistance);
		}
		else if (Distance > StrafeDistance + 80.f)
		{
			// 还在交战圈外：直冲过去
			MoveToTarget(StrafeDistance);
		}
		else
		{
			// 已在交战圈内：绕着玩家走位（保持距离 + 侧移）
			StrafeAroundTarget(StrafeDistance, DeltaTime);
		}

		// ⚠⚠⚠ Chase（追击/走位）也必须【转向玩家】——2026-10-03 决定性修复
		//
		// 视频实证：BOSS 全程背对玩家站着不动，影子都朝着侧面。
		// 原因：`TrackTargetDuringCombat` 只在 Telegraph / Attack / Recover 三个状态里被调用，
		//   **Chase 状态从来没调用过** → 追击时朝向完全冻结在初始值。
		//   而移动方向是【每帧重新算】的（追近/绕圈会变），朝向却不动 ——
		//   于是就出现"侧着走"甚至"背对站着"（因为绕圈是切线方向，本该配 Strafe 动画，
		//   但动画选择又依赖朝向，算出来就成了 Fwd，前后完全对不上）。
		//
		// 动作游戏通行做法：**追击时始终面向玩家**（不然玩家绕到背后就白打了）。
		// 速率给中等（比出招时的 6.0 慢、比出招中追踪的慢速快），让它"跟得上但不僵硬"。
		TrackTargetDuringCombat(DeltaTime, ChaseTrackYawRate, /*bUseDeadZone=*/false);
		return;
	}

	// 大招优先（它是一个正常招式，走同样的起手预警流程）
	if (UBossAttackData* Attack = PickAttack())
	{
		CurrentAttack = Attack;
		SetState(EBossState::Telegraph);
		return;
	}

	// 弹幕型 BOSS 的"拉开距离"：贴太近先后撤
	UBossPhaseData* Phase = GetCurrentPhase();
	if (Phase && Phase->BackoffRange > 0.f && Distance < Phase->BackoffRange)
	{
		MoveAwayFromTarget(Phase->BackoffDistance);
		return;
	}

	MoveToTarget(90.f);

	// 一直进不了招式距离就换个位置绕一下
	if (ChaseTimer > RepositionDelay)
	{
		SetState(EBossState::Reposition);
	}
}

void UBossBrainComponent::UpdateReposition(float DeltaTime)
{
	if (StateTimer > 1.5f)
	{
		SetState(EBossState::Chase);
	}
}

void UBossBrainComponent::UpdateTelegraph(float DeltaTime)
{
	// 起手期间持续转向玩家 —— 起手是"BOSS 正在瞄准你"的读条窗口，转得要明显（6.0）
	TrackTargetDuringCombat(DeltaTime, TelegraphTrackYawRate, /*bUseDeadZone=*/false);

	const UBossPhaseData* Phase = GetCurrentPhase();
	const float PhaseScale = Phase ? Phase->TelegraphTimeScale : 1.f;
	// 用 SetState 时缓存下来的时长，保证和预警条看到的是同一个数
	const float TelegraphTime = CachedTelegraphTime > 0.f
		? CachedTelegraphTime
		: (CurrentAttack
			? CurrentAttack->GetEffectiveTelegraphTime(IsPhase2()) * PhaseScale * FollowUpTelegraphScale
			: 0.6f);

	if (StateTimer >= TelegraphTime)
	{
		UE_LOG(LogDemoCombat, Log, TEXT("[Boss][Brain] 起手结束（%.2fs）→ 出招「%s」"),
			TelegraphTime, CurrentAttack ? *CurrentAttack->AttackName.ToString() : TEXT("无效招式"));

		if (ExecuteAttack(CurrentAttack))
		{
			SetState(EBossState::Attack);
		}
		else
		{
			SetState(EBossState::Recover);
		}
	}
}

void UBossBrainComponent::UpdateAttack(float DeltaTime)
{
	// 出招过程中继续小幅追玩家：这一招已经打出去了，玩家绕后能骗刀是对的，
	// 但完全不修正就成了"BOSS 对着空气砍完一整套"，所以给一个很慢的追踪速率 + 死区。
	TrackTargetDuringCombat(DeltaTime, AttackTrackYawRate, /*bUseDeadZone=*/true);

	// 技能活着的时候会一直挂着 State.Attacking，标签消失 = 这一招放完了
	UAbilitySystemComponent* ASC = GetBossASC();
	const bool bStillAttacking = ASC && ASC->HasMatchingGameplayTag(DemoTag::State_Attacking);

	if (!bStillAttacking)
	{
		SetState(EBossState::Recover);
		return;
	}

	if (StateTimer > 12.f)
	{
		UE_LOG(LogDemoCombat, Warning, TEXT("[Boss][Brain] 出招超过 12s 仍未结束，强制进入后摇（检查技能是否卡住）"));
		SetState(EBossState::Recover);
	}
}

void UBossBrainComponent::UpdateRecover(float DeltaTime)
{
	// 后摇是"收招回正"的窗口：转得比出招时快，好让下一招是从正对玩家开始的
	TrackTargetDuringCombat(DeltaTime, RecoverTrackYawRate, /*bUseDeadZone=*/true);

	// 后摇一开始先掷一次"要不要接下一招"（2~3 连，压迫感比单发强得多）
	if (!bFollowUpDecided)
	{
		bFollowUpDecided = true;
		if (TryStartFollowUp())
		{
			return;
		}
	}

	const float Duration = CurrentAttack ? CurrentAttack->GetEffectiveRecoverTime(IsPhase2()) : 0.8f;
	if (StateTimer >= Duration)
	{
		CurrentAttack = nullptr;
		SetState(EBossState::Chase);
	}
}

void UBossBrainComponent::UpdateStagger(float DeltaTime)
{
	UAbilitySystemComponent* ASC = GetBossASC();
	if (!ASC || !ASC->HasMatchingGameplayTag(DemoTag::State_Stagger))
	{
		CurrentAttack = nullptr;
		PendingFollowUp = nullptr;
		SetState(EBossState::Chase);
	}
}

void UBossBrainComponent::UpdatePhaseTransition(float DeltaTime)
{
	UBossPhaseData* Phase = GetCurrentPhase();
	const float Total = Phase ? FMath::Max(Phase->EnterDuration, 0.2f) : 0.6f;

	// 演出按时间分三段推进：进入动作 → 循环动作 → 收力动作。
	// 用时间轴而不是"等蒙太奇播完"，是因为演出时长是配出来的（玩家能不能白打由它决定）。
	if (Phase)
	{
		const int32 Stage = (StateTimer < Total * 0.5f) ? 0 : (StateTimer < Total * 0.85f ? 1 : 2);
		if (Stage != TransitionStage)
		{
			TransitionStage = Stage;

			UAnimMontage* Montage = nullptr;
			switch (Stage)
			{
			case 0:	Montage = Phase->EnterMontage;		break;
			case 1:	Montage = Phase->EnterLoopMontage;	break;
			case 2:	Montage = Phase->EnterOutMontage;	break;
			default: break;
			}
			if (Montage)
			{
				if (ABossCharacterBase* Boss = GetBoss())
				{
					Boss->PlayBossMontage(Montage);
				}
			}
		}
	}

	// 演出期间也盯着玩家（但慢慢转，别像雷达）
	if (ABossCharacterBase* Boss = GetBoss())
	{
		if (CachedTarget)
		{
			FVector Dir = CachedTarget->GetActorLocation() - Boss->GetActorLocation();
			Dir.Z = 0.f;
			if (!Dir.IsNearlyZero())
			{
				const FRotator Desired = FRotationMatrix::MakeFromX(Dir.GetSafeNormal()).Rotator();
				Boss->SetActorRotation(FMath::RInterpTo(Boss->GetActorRotation(), Desired, DeltaTime, 2.f));
			}
		}
	}

	if (StateTimer >= Total)
	{
		FinishPhaseTransition();
	}
}

// ==================================================================================
// 阶段
// ==================================================================================
void UBossBrainComponent::UpdatePhase()
{
	ABossCharacterBase* Boss = GetBoss();
	if (!Boss || Phases.Num() == 0)
	{
		return;
	}

	const float MaxHealth = Boss->GetMaxHealth();
	if (MaxHealth <= 0.f)
	{
		return;
	}

	const float Ratio = Boss->GetHealth() / MaxHealth;

	int32 NewIndex = 0;
	for (int32 i = 0; i < Phases.Num(); ++i)
	{
		if (Phases[i] && Ratio <= Phases[i]->HealthRatioThreshold)
		{
			NewIndex = i;
		}
	}

	// 只允许"往后推进"：被治疗回血不该把 BOSS 打回一阶段（那会让狂化效果反复触发）
	if (NewIndex > CurrentPhaseIndex)
	{
		StartPhaseTransition(NewIndex);
	}
}

void UBossBrainComponent::StartPhaseTransition(int32 NewPhaseIndex)
{
	ABossCharacterBase* Boss = GetBoss();
	if (!Boss)
	{
		return;
	}

	CurrentPhaseIndex = NewPhaseIndex;
	PhaseElapsedTime = 0.f;
	UltimateUseCount = 0;
	PatternCursor = 0;   // 每个阶段的背板都从第一招开始（玩家能记住"开局是什么"）

	UBossPhaseData* Phase = GetCurrentPhase();
	const bool bIsFinalPhase = (NewPhaseIndex == Phases.Num() - 1);

	// 大招计时：进阶段后要等 UltimateInitialDelay 才允许放，但也别让玩家等太久
	if (Phase)
	{
		UltimateCooldownRemaining = FMath::Max(Phase->UltimateInitialDelay, Phase->UltimateCooldown * 0.5f);
	}

	// 进入演出状态（会顺带清掉当前招式、冷却，并站定）
	SetState(EBossState::PhaseTransition);

	// 打上二阶段标记 + 施加狂化 GE
	Boss->EnterPhase(NewPhaseIndex, bIsFinalPhase, Phase ? Phase->EnrageEffect : nullptr);

	// 演出期间的无敌（可选）：留空 = 给玩家一个白打的输出窗口
	if (PhaseTransitionInvincibleEffect)
	{
		TransitionInvincibleHandle = UDemoAbilitySystemLibrary::ApplyEffectToSelf(Boss, PhaseTransitionInvincibleEffect);
	}

	UE_LOG(LogDemoCombat, Warning, TEXT("[Boss][Brain] ===== 进入阶段 %d/%d（%s）演出 %.1fs ====="),
		NewPhaseIndex + 1, Phases.Num(), bIsFinalPhase ? TEXT("最终阶段") : TEXT("过渡阶段"),
		Phase ? Phase->EnterDuration : 0.f);

	// 通知蓝图（切 BGM / 打提示 / 震屏）
	OnPhaseChanged.Broadcast(NewPhaseIndex);

	FGameplayEventData Payload;
	Payload.Instigator = Boss;
	Payload.Target = Boss;
	UAbilitySystemBlueprintLibrary::SendGameplayEventToActor(Boss, DemoTag::Event_Boss_PhaseChanged, Payload);
}

void UBossBrainComponent::FinishPhaseTransition()
{
	if (TransitionInvincibleHandle.IsValid())
	{
		UDemoAbilitySystemLibrary::RemoveActiveEffect(GetBoss(), TransitionInvincibleHandle);
		TransitionInvincibleHandle = FActiveGameplayEffectHandle();
	}

	TransitionStage = -1;
	PhaseElapsedTime = 0.f;

	ApplyPhaseSettings();

	UE_LOG(LogDemoCombat, Warning, TEXT("[Boss][Brain] 阶段演出结束，恢复战斗（招式池 %d 招 + 大招「%s」）"),
		GetCurrentPhase() ? GetCurrentPhase()->Attacks.Num() : 0,
		(GetCurrentPhase() && GetCurrentPhase()->UltimateAttack)
			? *GetCurrentPhase()->UltimateAttack->AttackName.ToString() : TEXT("无"));

	SetState(EBossState::Chase);
}

void UBossBrainComponent::ApplyPhaseSettings()
{
	ABossCharacterBase* Boss = GetBoss();
	const UBossPhaseData* Phase = GetCurrentPhase();
	if (!Boss || !Phase)
	{
		return;
	}

	if (UCharacterMovementComponent* MoveComp = Boss->GetCharacterMovement())
	{
		MoveComp->MaxWalkSpeed = Phase->MoveSpeed * FMath::Max(Phase->MoveSpeedScale, 0.01f);
	}
}

// ==================================================================================
// 选招 / 出招
// ==================================================================================
bool UBossBrainComponent::CanUseAttack(const UBossAttackData* Attack, float Distance, bool bIgnoreCooldown) const
{
	if (!Attack)
	{
		return false;
	}

	// 只在二阶段解锁的招式（大招一般勾这个）
	if (Attack->bOnlyInPhase2 && !IsPhase2())
	{
		return false;
	}

	// 近战没蒙太奇 = 挥不出去，直接跳过（弹幕/突进斩没有主体蒙太奇也能放）
	if (Attack->AttackType == EBossAttackType::MeleeSlash && !Attack->Montage)
	{
		return false;
	}

	// 重复冷却（固定背板取招时跳过：见头文件里 bIgnoreCooldown 的说明）
	if (!bIgnoreCooldown)
	{
		if (const float* Remaining = AttackCooldowns.Find(Attack->AttackName))
		{
			if (*Remaining > 0.f)
			{
				return false;
			}
		}
	}

	return true;
}

UBossAttackData* UBossBrainComponent::PickAttack()
{
	UBossPhaseData* Phase = GetCurrentPhase();
	if (!Phase || Phase->Attacks.Num() == 0)
	{
		return nullptr;
	}

	const float Distance = GetDistanceToTarget();

	// ---------- 0) 调试：强制某一类招式 ----------
	if (DebugForceAttackType >= 0)
	{
		for (UBossAttackData* Candidate : Phase->Attacks)
		{
			if (Candidate && static_cast<int32>(Candidate->AttackType) == DebugForceAttackType
				&& CanUseAttack(Candidate, Distance))
			{
				return Candidate;
			}
		}
	}

	// ---------- 1) 大招：满足门槛就优先放（它有最长的起手预警，玩家来得及跑位）----------
	if (ShouldUseUltimate() && Phase->UltimateAttack)
	{
		if (Phase->UltimateAttack->bIgnoreRangeCheck
			|| Phase->UltimateAttack->IsInRange(Distance)
			|| Distance <= 1800.f)
		{
			UE_LOG(LogDemoCombat, Warning, TEXT("[Boss][Brain] 大招条件满足 → 准备释放「%s」（已放 %d 次，冷却剩余 %.1fs）"),
				*Phase->UltimateAttack->AttackName.ToString(), UltimateUseCount,
				FMath::Max(Phase->UltimateCooldown - UltimateCooldownRemaining, 0.f));
			return Phase->UltimateAttack;
		}
	}

	// ---------- 1.5) 固定背板：确定性循环出招 ----------
	// 只要阶段配了 AttackPattern，就【按顺序循环】出招，而不是加权随机。
	// 这是"固定背板"的落点：玩家能记住顺序、预判下一招、练成肌肉记忆；
	// 而加权随机的对手永远只能靠即时反应去接，学不会 —— 那是另一种体验，不是这种。
	// 大招不放进这里：它仍走上面自己的冷却门，作为可预期的"插曲"。
	if (Phase->AttackPattern.Num() > 0)
	{
		const int32 PatternCount = Phase->AttackPattern.Num();
		for (int32 Step = 0; Step < PatternCount; ++Step)
		{
			const int32 Idx = (PatternCursor + Step) % PatternCount;
			const FName WantName = Phase->AttackPattern[Idx];

			UBossAttackData* Found = nullptr;
			for (UBossAttackData* Candidate : Phase->Attacks)
			{
				if (Candidate && Candidate->AttackName == WantName)
				{
					Found = Candidate;
					break;
				}
			}

			if (!Found)
			{
				// 名字对不上 = 这一拍永远放不出来，而且没有任何画面上的异常表现
				UE_LOG(LogDemoCombat, Error,
					TEXT("[Boss][Brain] 背板第 %d/%d 拍「%s」在招式池里找不到（名字拼错了？）"),
					Idx + 1, PatternCount, *WantName.ToString());
				continue;
			}

			// 固定背板取招【不看重复冷却】：顺序是设计好的，冷却插进来只会把节奏打乱
			if (CanUseAttack(Found, Distance, /*bIgnoreCooldown=*/true))
			{
				// 游标停在"选中这招"的下一格 —— 跳招不会打乱背板顺序
				PatternCursor = (Idx + 1) % PatternCount;

				UE_LOG(LogDemoCombat, Warning,
					TEXT("[Boss][Brain] 背板 第%d/%d拍 →「%s」（距 %.0fcm / 理想 %.0f±%.0f）"),
					Idx + 1, PatternCount, *Found->AttackName.ToString(),
					Distance, Found->IdealRange, Found->RangeTolerance);
				return Found;
			}
		}

		// 整轮都不可用 → 游标【必须】往前走一格。
		// 原来这里不动游标，背板会永远冻结在同一拍上，之后每一次都掉进随机兜底 ——
		// 表现就是"BOSS 一直在重复同一个动作"，而且从画面上完全看不出为什么。
		PatternCursor = (PatternCursor + 1) % PatternCount;
		UE_LOG(LogDemoCombat, Warning,
			TEXT("[Boss][Brain] 背板整轮 %d 拍都不可用 → 落到随机兜底（游标已推进，不会冻结）"), PatternCount);
	}

	// ---------- 2) 常规招式：优先选"距离落在舒适区"的 ----------
	TArray<UBossAttackData*> InRange;
	TArray<UBossAttackData*> AnyReady;

	for (UBossAttackData* Candidate : Phase->Attacks)
	{
		if (!CanUseAttack(Candidate, Distance))
		{
			continue;
		}

		AnyReady.Add(Candidate);

		if (Candidate->IsInRange(Distance))
		{
			InRange.Add(Candidate);
		}
	}

	// 不连着放同一招：兜底是随机抽的，随机最容易抽成 A A A，
	// 玩家看到的就是"它只会这一招"。固定背板不需要这条（顺序本来就是设计好的）。
	if (InRange.Num() > 1 && LastAttackName != NAME_None)
	{
		TArray<UBossAttackData*> Filtered;
		for (UBossAttackData* Candidate : InRange)
		{
			if (Candidate && Candidate->AttackName != LastAttackName)
			{
				Filtered.Add(Candidate);
			}
		}
		if (Filtered.Num() > 0)
		{
			InRange = MoveTemp(Filtered);
		}
	}

	if (InRange.Num() > 0)
	{
		float TotalWeight = 0.f;
		for (const UBossAttackData* Candidate : InRange)
		{
			TotalWeight += FMath::Max(Candidate->Weight, 0.01f);
		}

		float Roll = FMath::FRandRange(0.f, TotalWeight);
		for (UBossAttackData* Candidate : InRange)
		{
			Roll -= FMath::Max(Candidate->Weight, 0.01f);
			if (Roll <= 0.f)
			{
				UE_LOG(LogDemoCombat, Warning,
					TEXT("[Boss][Brain] 兜底（距离内随机）→「%s」（距 %.0fcm）"),
					*Candidate->AttackName.ToString(), Distance);
				return Candidate;
			}
		}
		return InRange.Last();
	}

	// ---------- 3) 没有一招的距离是合适的 → 退而求其次，用"最接近舒适区"的那一招 ----------
	// 否则 BOSS 会站在奇怪的距离上发呆，玩家会觉得 AI 坏了
	if (AnyReady.Num() > 0)
	{
		UBossAttackData* Best = nullptr;
		float BestDelta = TNumericLimits<float>::Max();
		for (UBossAttackData* Candidate : AnyReady)
		{
			const float Delta = FMath::Abs(Distance - Candidate->IdealRange) - Candidate->RangeTolerance;
			if (Delta < BestDelta)
			{
				BestDelta = Delta;
				Best = Candidate;
			}
		}
		return Best;
	}

	return nullptr;
}

bool UBossBrainComponent::ShouldUseUltimate() const
{
	const UBossPhaseData* Phase = GetCurrentPhase();
	if (!Phase || !Phase->UltimateAttack)
	{
		return false;
	}

	if (UltimateCooldownRemaining > 0.f)
	{
		return false;
	}

	if (Phase->UltimateMaxCount > 0 && UltimateUseCount >= Phase->UltimateMaxCount)
	{
		return false;
	}

	const ABossCharacterBase* Boss = GetBoss();
	const float MaxHealth = Boss ? Boss->GetMaxHealth() : 0.f;
	const float Ratio = (Boss && MaxHealth > 0.f) ? Boss->GetHealth() / MaxHealth : 1.f;

	// 濒死爆发：血量跌破这条线时无视"进阶段延迟"，立刻丢大招制造戏剧点
	const bool bDesperate = Phase->UltimateDesperateHealthRatio > 0.f
		&& Ratio <= Phase->UltimateDesperateHealthRatio;

	if (!bDesperate && PhaseElapsedTime < Phase->UltimateInitialDelay)
	{
		return false;
	}

	// 血量解锁门槛
	if (Phase->UltimateHealthRatioGate < 1.f && Ratio > Phase->UltimateHealthRatioGate)
	{
		return false;
	}

	return true;
}

bool UBossBrainComponent::ExecuteAttack(UBossAttackData* Attack)
{
	ABossCharacterBase* Boss = GetBoss();
	if (!Boss || !Attack)
	{
		return false;
	}

	UAbilitySystemComponent* ASC = GetBossASC();
	if (!ASC)
	{
		return false;
	}

	const TSubclassOf<UGameplayAbility> AbilityClass = Boss->GetAbilityClassForAttackType(Attack->AttackType);
	if (!AbilityClass)
	{
		UE_LOG(LogDemoCombat, Error, TEXT("[Boss][Brain] 招式「%s」找不到对应技能（类型 %d）"),
			*Attack->AttackName.ToString(), static_cast<int32>(Attack->AttackType));
		return false;
	}

	// 把招式数据挂到角色上，技能激活时自己来取。
	// 为什么不用"取能力实例再 SetPendingAttack"：InstancedPerActor 的实例是【首次激活时】才创建的，
	// 首次出招时 GetPrimaryInstance() 很可能是 null → 招式数据塞不进去，技能空放。
	Boss->SetPendingAttackData(Attack);

	FGameplayAbilitySpec* Spec = ASC->FindAbilitySpecFromClass(AbilityClass);
	if (!Spec)
	{
		// 兜底：技能还没授予（BP_Boss 的 DefaultAbilities 没配全）→ 现场授予，别让 BOSS 站桩
		FGameplayAbilitySpec NewSpec(AbilityClass, 1, INDEX_NONE, Boss);
		ASC->GiveAbility(NewSpec);
		Spec = ASC->FindAbilitySpecFromClass(AbilityClass);
		UE_LOG(LogDemoCombat, Warning, TEXT("[Boss][Brain] 技能 %s 未授予，已现场补授"), *AbilityClass->GetName());
	}

	if (!Spec)
	{
		return false;
	}

	// 兼容路径：实例已经存在时也直接塞一份（两条路都通，谁先到都行）
	if (UGameplayAbility* Instance = Spec->GetPrimaryInstance())
	{
		if (UGA_BossAttackBase* BossAbility = Cast<UGA_BossAttackBase>(Instance))
		{
			BossAbility->SetPendingAttack(Attack);
		}
	}

	if (!ASC->TryActivateAbility(Spec->Handle))
	{
		UE_LOG(LogDemoCombat, Warning, TEXT("[Boss][Brain] 激活技能 %s 失败（可能被标签阻挡 / 冷却中）"),
			*AbilityClass->GetName());
		return false;
	}

	// 冷却按二阶段强化后的值走（二阶段出招更密）
	AttackCooldowns.Add(Attack->AttackName, FMath::Max(Attack->GetEffectiveCooldown(IsPhase2()), 0.1f));

	// 记下这一招：随机兜底时靠它避免连着放同一招
	LastAttackName = Attack->AttackName;

	if (Attack->AttackType == EBossAttackType::Ultimate)
	{
		++UltimateUseCount;
		const UBossPhaseData* Phase = GetCurrentPhase();
		UltimateCooldownRemaining = Phase ? FMath::Max(Phase->UltimateCooldown, 1.f) : 20.f;
		OnUltimateStarted.Broadcast();
	}

	return true;
}

bool UBossBrainComponent::RequestRevengeAttack()
{
	// 已经在出招 / 起手 / 硬直 / 死亡 / 阶段转换中 → 不打断，下次再说
	if (CurrentState == EBossState::Telegraph || CurrentState == EBossState::Attack ||
		CurrentState == EBossState::Stagger || CurrentState == EBossState::Dead ||
		CurrentState == EBossState::PhaseTransition)
	{
		return false;
	}

	if (!CachedTarget)
	{
		return false;
	}

	// 复用正常的选招逻辑（按阶段招式池 + 距离 + 权重），保证复仇出的招也是"合理的那一招"
	UBossAttackData* Attack = PickAttack();
	if (!Attack)
	{
		UE_LOG(LogDemoCombat, Log, TEXT("[Boss][Brain] 复仇：当前距离没有可用招式，放弃本次复仇"));
		return false;
	}

	CurrentAttack = Attack;
	bFollowUpDecided = false;
	PendingFollowUp = nullptr;
	FollowUpTelegraphScale = 1.f;

	UE_LOG(LogDemoCombat, Warning, TEXT("[Boss][Brain] 复仇触发：被玩家打满 → 强制起手「%s」"),
		*Attack->AttackName.ToString());

	SetState(EBossState::Telegraph);
	return true;
}

bool UBossBrainComponent::TryStartFollowUp()
{
	UBossAttackData* Previous = CurrentAttack;
	if (!Previous || Previous->FollowUpAttacks.Num() == 0)
	{
		return false;
	}

	const float Chance = Previous->GetEffectiveFollowUpChance(IsPhase2());
	if (Chance <= 0.f || FMath::FRand() > Chance)
	{
		return false;
	}

	const float Distance = GetDistanceToTarget();
	TArray<UBossAttackData*> Usable;
	for (UBossAttackData* Next : Previous->FollowUpAttacks)
	{
		if (Next && CanUseAttack(Next, Distance))
		{
			Usable.Add(Next);
		}
	}

	if (Usable.Num() == 0)
	{
		return false;
	}

	UBossAttackData* Chosen = Usable[FMath::RandRange(0, Usable.Num() - 1)];

	CurrentAttack = Chosen;
	PendingFollowUp = nullptr;

	// 接续时把起手压短（已经贴身连打了，再给一次长预警会很拖沓）
	FollowUpTelegraphScale = FMath::Clamp(Previous->FollowUpTelegraphScale, 0.05f, 1.f);

	UE_LOG(LogDemoCombat, Log, TEXT("[Boss][Brain] 连招链：%s → %s（接续概率 %.2f，起手压到 %.0f%%）"),
		*Previous->AttackName.ToString(), *Chosen->AttackName.ToString(), Chance, FollowUpTelegraphScale * 100.f);

	SetState(EBossState::Telegraph);
	return true;
}

// ==================================================================================
// 移动
// ==================================================================================
void UBossBrainComponent::MoveToTarget(float AcceptRadius)
{
	const AActor* Target = CachedTarget.Get();
	if (!Target)
	{
		return;
	}

	const AActor* Owner = GetOwner();
	const FVector OwnerLoc = Owner ? Owner->GetActorLocation() : FVector::ZeroVector;
	const float Dist = FVector::Dist2D(OwnerLoc, Target->GetActorLocation());

	// 已经够近：停手，避免贴脸抖动（同时撤销 AI 可能还在跑的寻路）
	if (Dist <= FMath::Max(AcceptRadius, 1.f))
	{
		if (AAIController* AI = GetAIController())
		{
			AI->StopMovement();
		}
		return;
	}

	// 优先走 AI 寻路（有 NavMesh 时最顺滑、会绕障碍）
	if (!bNavPathUnavailable)
	{
		if (AAIController* AI = GetAIController())
		{
			const EPathFollowingRequestResult::Type Result =
				AI->MoveToActor(const_cast<AActor*>(Target), AcceptRadius);
			if (Result == EPathFollowingRequestResult::RequestSuccessful
				|| Result == EPathFollowingRequestResult::AlreadyAtGoal)
			{
				return;
			}

			// 寻路失败（关卡没有 NavMesh / 找不到路径）→ 之后一律走无导航直驱。
			// ⚠ 关键：失败一次就【记住】，别每帧再请求。反复请求寻路会触发
			//   PathFollowingComponent 的 AbortMove → StopActiveMovement()，
			//   把下面刚下发的 RequestDirectMove 请求速度一起清掉 → BOSS 纹丝不动。
			bNavPathUnavailable = true;
			UE_LOG(LogDemoCombat, Warning,
				TEXT("[Boss][移动] 关卡没有可用导航数据（缺 NavMesh）→ 之后一律走无导航直驱，不再调用 AI 寻路"));
		}
	}

	// 无导航直驱：直接朝目标 AddMovementInput，让 BOSS 真走起来。
	// ⚠ 这是修复"BOSS 不动 / 没有移动动画 / 一直站着发呆"的关键：
	//   原实现只调 AIController->MoveToActor，而 MoveToActor 在没有 NavMesh 时会【静默失败】
	//   （返回 Failed，不产生任何速度）→ BOSS 速度恒 0 → 永远切不到 Jog（看不到移动动画），
	//   而且永远走不到攻击距离 → 只能在远处站着，看起来就是"重复一个动作 / 没有招式"。
	//   用角色自带的移动输入兜底，不依赖任何寻路数据，任何关卡都能跑。
	if (ABossCharacterBase* Boss = GetBoss())
	{
		FVector Dir = Target->GetActorLocation() - OwnerLoc;
		Dir.Z = 0.f;
		Dir = Dir.GetSafeNormal2D();
		if (!Dir.IsZero())
		{
			DriveMoveNoNav(Boss, Dir, TEXT("朝目标"));
		}
		else
		{
			// ⚠⚠ 方向为 0（两者水平坐标完全重合）→ 以前【什么都不做】，
			//   BOSS 就站着不动，且没有任何日志 → 极难定位。
			//   现在退回到"背对目标方向走"，并打日志。
			UE_LOG(LogDemoCombat, Warning,
				TEXT("[Boss][移动] ⚠ 与目标水平坐标重合，方向为 0 → 改为朝 Boss 当前朝向直走"));
			DriveMoveNoNav(Boss, Owner->GetActorForwardVector().GetSafeNormal2D(), TEXT("方向退化"));
		}
	}
}

void UBossBrainComponent::MoveAwayFromTarget(float Distance)
{
	const AActor* Owner = GetOwner();
	const AActor* Target = CachedTarget.Get();
	if (!Owner || !Target)
	{
		return;
	}

	FVector Away = Owner->GetActorLocation() - Target->GetActorLocation();
	Away.Z = 0.f;
	Away = Away.GetSafeNormal2D();
	if (Away.IsNearlyZero())
	{
		Away = -Owner->GetActorForwardVector().GetSafeNormal2D();
	}

	const FVector Dest = Owner->GetActorLocation() + Away * FMath::Max(Distance, 100.f);

	// 优先走 AI 寻路（有 NavMesh 时最顺滑）
	if (!bNavPathUnavailable)
	{
		if (AAIController* AI = GetAIController())
		{
			const EPathFollowingRequestResult::Type Result = AI->MoveToLocation(Dest, 60.f);
			if (Result == EPathFollowingRequestResult::RequestSuccessful
				|| Result == EPathFollowingRequestResult::AlreadyAtGoal)
			{
				return;
			}
			bNavPathUnavailable = true;
		}
	}

	// 无导航兜底：背离目标直驱（逻辑同上）
	if (ABossCharacterBase* Boss = GetBoss())
	{
		if (!Away.IsZero())
		{
			DriveMoveNoNav(Boss, Away, TEXT("背离目标"));
		}
	}
}

void UBossBrainComponent::StrafeAroundTarget(float DesiredDistance, float DeltaTime)
{
	ABossCharacterBase* Boss = GetBoss();
	const AActor* Target = CachedTarget.Get();
	if (!Boss || !Target)
	{
		return;
	}

	const FVector ToTarget = Target->GetActorLocation() - Boss->GetActorLocation();
	const FVector Radial = FVector(ToTarget.X, ToTarget.Y, 0.f);
	const float Dist = Radial.Size();
	if (Dist < KINDA_SMALL_NUMBER)
	{
		return;
	}

	const FVector RadialDir = Radial / Dist;

	// 切线（垂直于"BOSS→玩家"），绕着玩家平移
	FVector Tangent(-RadialDir.Y, RadialDir.X, 0.f);

	// ⚠⚠ 换边时机（2026-10-02 修"移动动画断续"）
	// 原来每 StrafeFlipInterval 秒【随机】决定一次绕圈方向，而且是不带过渡的硬切。
	// 于是 BOSS 每隔几秒就把移动方向整个反向 → 行走动画被迫换向 → 播放时间清零、
	// 腿刚迈出一步就被打断重来，画面上就是"动一下、停一下、再动一下"。
	// 现在只在【真的需要换边】时才换，并且换边后给一段冷却，避免连续抖动；
	// 方向用 bStrafeClockwise 保持稳定，不做逐帧随机。
	StrafeFlipTimer += DeltaTime;
	if (StrafeFlipTimer >= StrafeFlipInterval)
	{
		StrafeFlipTimer = 0.f;
		// 只在"已经贴着理想距离"时才换边（此时换边观感最自然）；
		// 距离还差很多时优先去修正距离，避免一边调距离一边换向。
		if (FMath::Abs(Dist - DesiredDistance) < 120.f)
		{
			bStrafeClockwise = !bStrafeClockwise;
		}
	}
	if (!bStrafeClockwise)
	{
		Tangent *= -1.f;
	}

	// 再把距离拉回理想值：太近就往外、太远就往里（叠加在切线方向上，走位是斜的）
	// ⚠ 用【平滑的径向修正】而不是硬 clamp：原来 /200.f 的一次性 clamp 会让
	//   "径向修正量"在距离误差大时直接打满 ±1，方向瞬间从"纯切线"变成"纯径向" → 又是硬转向。
	//   这里加一个死区 + 斜率更缓的曲线，让方向连续变化。
	const float RadialError = Dist - DesiredDistance;
	float RadialScale = 0.f;
	if (FMath::Abs(RadialError) > 40.f)
	{
		// 死区 40cm 内不修正；超出后按误差的 1/400 缓慢增加，最大 0.8
		RadialScale = FMath::Clamp((FMath::Abs(RadialError) - 40.f) / 400.f, 0.f, 0.8f)
			* FMath::Sign(RadialError);
	}
	const FVector RadialFix = RadialDir * RadialScale;

	DriveMoveNoNav(Boss, (Tangent + RadialFix).GetSafeNormal2D(), TEXT("绕圈走位"));
}

void UBossBrainComponent::TrackTargetDuringCombat(float DeltaTime, float YawRate, bool bUseDeadZone)
{
	ABossCharacterBase* Boss = GetBoss();
	if (!Boss || !CachedTarget)
	{
		return;
	}

	// 让路 1：突进/冲刺位移期间朝向归位移任务管，持续追踪会把直线劈拧成弧线
	if (Boss->bDashFacingLocked)
	{
		return;
	}

	// 让路 2：角色自身正在执行一次 FaceTarget 转向时别插手，避免两个写同一 rotator 互相拉扯
	if (Boss->IsSmoothFacingActive())
	{
		return;
	}

	FVector Dir = CachedTarget->GetActorLocation() - Boss->GetActorLocation();
	Dir.Z = 0.f;
	if (Dir.IsNearlyZero())
	{
		return;
	}

	const FRotator Current = Boss->GetActorRotation();
	const FRotator Desired = FRotationMatrix::MakeFromX(Dir.GetSafeNormal()).Rotator();

	// 死区：已经基本对着了就别每帧微调。出招动作本身的根位移会带动朝向，
	// 这时再叠加持续追踪会产生高频抖动（看起来像 BOSS 在哆嗦）。
	if (bUseDeadZone)
	{
		const float DeltaYaw = FMath::FindDeltaAngleDegrees(Current.Yaw, Desired.Yaw);
		if (FMath::Abs(DeltaYaw) < 2.5f)
		{
			return;
		}
	}

	Boss->SetActorRotation(FMath::RInterpTo(Current, Desired, DeltaTime, YawRate));
}

void UBossBrainComponent::DriveMoveNoNav(ABossCharacterBase* Boss, const FVector& Dir, const TCHAR* Reason)
{
	if (!Boss || Dir.IsZero())
	{
		return;
	}

	// ⚠⚠ 2026-09-30 定案：AI 角色【不能】用 AddMovementInput 驱动。
	// 引擎 CharacterMovementComponent.cpp:1723 —— 只有
	//   CharacterOwner->IsLocallyControlled()（或没有 Controller 时）才会执行
	//   ControlledCharacterMove(InputVector, ...)。
	// BOSS 由 AIController 接管、属于服务端 AI 角色，不是"本地控制" →
	// 累积进来的输入每帧在 TickComponent 开头被 ConsumeInputVector() 吃掉后【直接丢弃】，
	// 速度恒为 0。实测：无头跑 17 秒只挪动了 14cm（那点位移还是攻击根位移带的）。
	//
	// RequestDirectMove 是寻路组件给 AI 角色下速度的标准通道（PathFollowingComponent 自己就是这么用的），
	// 会在 PerformMovement 里真正写进 Velocity，服务端 AI 同样生效。
	UCharacterMovementComponent* MoveComp = Boss->GetCharacterMovement();
	if (!MoveComp)
	{
		return;
	}

	// ⚠⚠⚠ 出招中【不要下发移动】（2026-10-02 决定性修复，"移动时只是模型平移"的最后一块）
	//
	// 实测整局资产分布（94 个采样）：
	//   Idle_NonAdditive 57.4% / Jog_Fwd 只有 8.5% / 其余全是攻击蒙太奇
	// 也就是说 BOSS 几乎从不走路。而 `UpdateBossLocomotion` 在 `IsAnyMontagePlaying()` 为真时
	// 直接 return（"不打扰表现动画"）→ 攻击期间根本不播行走动画。
	// 可这段期间 BOSS 还在【物理位移】上：
	//   ① 上一帧残留的 RequestDirectMove 速度仍在生效（最多滑出 380cm/s × 一段出招时长）
	//   ② 攻击蒙太奇自带的根位移
	// 两者叠加 = 玩家看到的"整体模型在地面上平移、腿一动不动"。
	//
	// 修法：【真正出招】期间直接【清零速度并撤销移动请求】。招式本身该有的位移由它的根位移负责，
	// 走路动画不参与，两者不叠加 → 画面上要么"有腿在走"，要么"站着出招"，不会再出现"平移无腿"。
	//
	// ⚠⚠⚠ 2026-10-02 第十八轮：只有【Active（真正打出来的那一下）】才定身，
	//   【Telegraph（预警红光）】必须【继续跟随玩家移动】（带行走动画）。
	// 玩家需求："boss出招时不移动，在预警红光时跟随玩家，也要有移动动作动画"。
	// 理由（也是动作游戏通行做法）：预警红光是"我要打你了"的信号，
	//   此时 BOSS 继续向玩家逼近 = 压迫感 + 玩家能预判落点；
	//   如果预警时就站死，玩家会觉得 BOSS 卡住不动、很呆；
	//   而真正挥出那一瞬间必须定身，否则"整体平移无腿"的问题会回来。
	if (Boss->IsStrikeActive())
	{
		MoveComp->StopMovementImmediately();
		MoveComp->Velocity = FVector::ZeroVector;
		return;
	}

	const FVector Desired = Dir.GetSafeNormal2D() * MoveComp->MaxWalkSpeed;
	MoveComp->RequestDirectMove(Desired, /*bForceMaxSpeed=*/true);

	// 告诉 BOSS 移动方向：起步第一帧位移还小到算不出方向时，
	// 方向判定要用它兜底（GetVelocity() 对 AI 直驱恒为 0，用不了）。
	Boss->NotifyBossMoveCommand(Desired.Size2D());
	Boss->NotifyBossMoveDirection(Dir);

	// ⚠⚠⚠ 把【下令的方向】记下来给动画用（2026-10-02 决定性修复）
	//
	// 方向化行走（前后左右四向）需要知道"此刻往哪个方向走"，
	// 但角色自己的 GetVelocity() 在 AI 直驱下恒为 0（已被 Movement 消费），
	// 实测导致方向判定永远退化成"前冲"→ 整局只播 Walk_Fwd → 绕圈也在往前跑 = 溜冰。
	// Brain 是方向的【下达者】，由它把方向传下去，作为位移测不出来时的兜底。
	Boss->NotifyBossMoveDirection(Dir.GetSafeNormal2D());

	// 双保险：如果速度请求在本次物理前被别人（AI 寻路中止等）清掉了，
	// 直接把速度写进去。对服务端 AI 角色这是合法做法 —— 反正它没有本地输入可用。
	if (MoveComp->Velocity.Size2D() < 1.f)
	{
		MoveComp->Velocity = Desired;
	}

	// 告诉 BOSS"我这一帧在下令移动"——底层行走动画据此强制播 Jog，
	// 不再被 AI 直驱的速度抖动（逐帧 0↔最大）冻成 Idle（那样画面就是"身体僵着整体滑过去"）。
	// ⚠ 必须把【本帧下令的速度】一起传过去：BOSS 自己在 Tick 里读 GetVelocity() 永远读到 0
	//   （RequestDirectMove 的速度已被 CharacterMovement 消费掉），
	//   拿不到真值播放倍速就会长期钉在下限 → 腿慢动作爬、胶囊 380cm/s 平移 = 看着像"只有模型平移"。
	Boss->NotifyBossMoveCommand(Desired.Size2D());

	// 节流日志：每 ~3s 一次，方便验收（也用来看真实速度有没有起来）
	// ⚠⚠ 2026-10-03 原来是 `static float s_LastNoNavLog` —— 那是【跨实例共享的全局】，
	//   多个 BOSS / 重开关卡时会把彼此的日志节流掉，而且"第一次调用"还会被
	//   Now - (-999) 的巨大差值吃掉。改成实例成员，避免诊断信息丢失。
	if (UWorld* W = GetWorld())
	{
		const float Now = W->GetTimeSeconds();
		if (Now - LastNoNavLogTime > 3.f)
		{
			LastNoNavLogTime = Now;
			UE_LOG(LogDemoCombat, Warning,
				TEXT("[Boss][移动] 无导航直驱（%s）：请求=%.0f 速度=%.0f 上次生效请求速度=%.0f 最大加速=%.0f 踩地=%d 模式=%d 位置=(%.0f,%.0f,%.0f) 距目标=%.0f"),
				Reason, Desired.Size2D(), Boss->GetVelocity().Size2D(),
				MoveComp->GetLastUpdateRequestedVelocity().Size2D(),
				MoveComp->GetMaxAcceleration(),
				MoveComp->IsMovingOnGround() ? 1 : 0,
				static_cast<int32>(MoveComp->MovementMode),
				Boss->GetActorLocation().X, Boss->GetActorLocation().Y, Boss->GetActorLocation().Z,
				GetDistanceToTarget());
		}
	}
}

void UBossBrainComponent::StopMovement()
{
	if (AAIController* AI = GetAIController())
	{
		AI->StopMovement();
	}
}

// ==================================================================================
// 调试
// ==================================================================================
void UBossBrainComponent::UpdateDebugDraw() const
{
	if (!bDrawDebugBrain)
	{
		return;
	}

	const AActor* Owner = GetOwner();
	UWorld* World = GetWorld();
	if (!Owner || !World)
	{
		return;
	}

	const FVector Origin = Owner->GetActorLocation();

	if (CachedTarget)
	{
		DrawDebugLine(World, Origin, CachedTarget->GetActorLocation(), FColor::Yellow, false, -1.f, 0, 1.5f);
		DrawDebugString(World, CachedTarget->GetActorLocation() + FVector(0.f, 0.f, 120.f),
			FString::Printf(TEXT("距离 %.0f"), GetDistanceToTarget()), nullptr, FColor::Yellow, 0.f, true);
	}

	static const UEnum* StateEnum = StaticEnum<EBossState>();
	const FString StateName = StateEnum ? StateEnum->GetNameStringByValue(static_cast<int64>(CurrentState)) : TEXT("?");

	DrawDebugString(World, Origin + FVector(0.f, 0.f, 200.f),
		FString::Printf(TEXT("BOSS [%s]  阶段%d  大招冷却%.1fs"),
			*StateName, CurrentPhaseIndex + 1, UltimateCooldownRemaining),
		nullptr, FColor::Red, 0.f, true);

	// 各招的距离带：绿环 = 舒适区，红环 = 容差边界
	const UBossPhaseData* Phase = GetCurrentPhase();
	if (Phase)
	{
		for (const UBossAttackData* Attack : Phase->Attacks)
		{
			if (!Attack)
			{
				continue;
			}
			const FColor Color = Attack->IsInRange(GetDistanceToTarget()) ? FColor::Green : FColor::Silver;
			DrawDebugCircle(World, Origin, Attack->IdealRange, 48, Color, false, -1.f, 0, 0.6f,
				FVector(1.f, 0.f, 0.f), FVector(0.f, 1.f, 0.f), false);
		}
	}
}

// ==================================================================================
// 内置默认阶段 / 招式（零配置也能打）
// ==================================================================================
UBossAttackData* UBossBrainComponent::MakeDefaultAttack(FName AttackName, EBossAttackType AttackType,
	const TCHAR* MontagePath, float IdealRange, float RangeTolerance, float Damage, float Cooldown)
{
	UAnimMontage* Montage = LoadBossMontage(MontagePath);

	UBossAttackData* Data = NewObject<UBossAttackData>(this, AttackName);
	Data->AttackName = AttackName;
	Data->AttackType = AttackType;
	Data->Montage = Montage;
	Data->IdealRange = IdealRange;
	Data->RangeTolerance = RangeTolerance;
	Data->Damage = Damage;
	Data->Cooldown = Cooldown;

	if (!Montage && AttackType == EBossAttackType::MeleeSlash)
	{
		UE_LOG(LogDemoCombat, Warning,
			TEXT("[Boss][Brain] 默认招式「%s」找不到蒙太奇 %s → 跑了 Tools/SetupBossMontages.py 吗？"),
			*AttackName.ToString(), MontagePath);
	}

	return Data;
}

void UBossBrainComponent::EnsureDefaultPhases()
{
	if (Phases.Num() > 0 || !bAutoBuildDefaultPhases)
	{
		// 排查要点：招式池和固定背板都来自【这份配置】，不是 C++ 里的默认值。
		// 如果新加的招式一直没出现，先看有没有这一行 —— 有，就说明改 C++ 默认值是无效的，
		// 得改 BP_Boss 上 Brain 组件的 Phases（或者清空它，让 C++ 默认值接管）。
		UE_LOG(LogDemoCombat, Warning,
			TEXT("[Boss][Brain] 已配置 %d 个阶段 → 跳过内置默认招式（招式池/背板以这份配置为准）"),
			Phases.Num());
		return;
	}

	const TCHAR* AnimDir = TEXT("/Game/Boss/Animation/");

	// ---------------- 招式 1：近战挥砍（三段，可接成连招链）----------------
	// 伤害 / 削韧全部取自 Core/DemoCombatTuning.h —— 那里有"为什么是这个数"的推导。
	// 一阶段的基准是"玩家 2000 血，挨近战 28 下死"。
	UBossAttackData* Slash1 = MakeDefaultAttack(TEXT("Slash_01"), EBossAttackType::MeleeSlash,
		*FString::Printf(TEXT("%sBoss_Slash_01_Montage.Boss_Slash_01_Montage"), AnimDir),
		220.f, 130.f, DemoBalance::BossSlash1Damage, 2.2f);
	UBossAttackData* Slash2 = MakeDefaultAttack(TEXT("Slash_02"), EBossAttackType::MeleeSlash,
		*FString::Printf(TEXT("%sBoss_Slash_02_Montage.Boss_Slash_02_Montage"), AnimDir),
		230.f, 140.f, DemoBalance::BossSlash2Damage, 3.0f);
	UBossAttackData* Slash3 = MakeDefaultAttack(TEXT("Slash_03"), EBossAttackType::MeleeSlash,
		*FString::Printf(TEXT("%sBoss_Slash_03_Montage.Boss_Slash_03_Montage"), AnimDir),
		260.f, 150.f, DemoBalance::BossSlash3Damage, 5.0f);

	if (Slash1)
	{
		Slash1->PoiseDamage = DemoBalance::BossSlash1Poise;
		Slash1->TelegraphTime = 0.55f;
		Slash1->RecoverTime = 0.55f;
		Slash1->HitWindowStartRatio = 0.26f;
		Slash1->HitWindowEndRatio = 0.50f;
		Slash1->Weight = 3.f;

		// 挥砍 → 挥砍 的二连（二阶段接续概率更高）
		Slash1->FollowUpAttacks.Add(Slash2);
		Slash1->FollowUpChance = 0.35f;
		Slash1->FollowUpTelegraphScale = 0.30f;
	}

	if (Slash2)
	{
		Slash2->PoiseDamage = DemoBalance::BossSlash2Poise;
		Slash2->TelegraphTime = 0.50f;
		Slash2->RecoverTime = 0.65f;
		Slash2->HitWindowStartRatio = 0.26f;
		Slash2->HitWindowEndRatio = 0.52f;
		Slash2->Weight = 2.5f;
	}

	if (Slash3)
	{
		// 三段重击：起手更长、伤害更高、可以接突进斩
		Slash3->PoiseDamage = DemoBalance::BossSlash3Poise;
		Slash3->TelegraphTime = 0.85f;
		Slash3->RecoverTime = 1.0f;
		Slash3->HitWindowStartRatio = 0.30f;
		Slash3->HitWindowEndRatio = 0.56f;
		Slash3->Weight = 1.2f;
		Slash3->bOnlyInPhase2 = false;
	}

	// ---------------- 招式 1.4~1.6：狂化快爪三连（二阶段才有）----------------
	// 取材 Khaimera 的 Melee_A_Fast / Melee_B_Fast / Melee_C_Fast —— 和 01~03 的
	// 普通三连是【同一套动作的加速版】：摆手更小、收招更快，视觉上就是"狂化后连得更快"。
	// ⚠ 这三招必须【同一套变体】：脚本里曾经把 Melee_A_Slow（慢起手重砸）混进 06，
	//   结果前两下飞快、第三下突然变慢，观感像卡帧 —— 已在 SetupBossKhaimera.py 里修掉。
	// 设计意图：一阶段只有朴素的 01~03 三连，玩家能背下来；二阶段解锁这套更密的快爪，
	//   "BOSS 变快了"这件事用【动作本身】传达，而不是只靠加快出招间隔。
	UBossAttackData* Slash4 = MakeDefaultAttack(TEXT("Slash_04"), EBossAttackType::MeleeSlash,
		*FString::Printf(TEXT("%sBoss_Slash_04_Montage.Boss_Slash_04_Montage"), AnimDir),
		240.f, 140.f, DemoBalance::BossSlash4Damage, 3.5f);
	if (Slash4)
	{
		Slash4->PoiseDamage = DemoBalance::BossSlash4Poise;
		Slash4->TelegraphTime = 0.34f;          // Fast 变体摆手短 → 预警也得跟着短
		Slash4->RecoverTime = 0.38f;
		Slash4->HitWindowStartRatio = 0.22f;
		Slash4->HitWindowEndRatio = 0.50f;
		Slash4->Weight = 2.f;
		Slash4->bOnlyInPhase2 = true;
	}

	// ---------------- 招式 1.5：狂化快爪·二 ----------------
	UBossAttackData* Slash5 = MakeDefaultAttack(TEXT("Slash_05"), EBossAttackType::MeleeSlash,
		*FString::Printf(TEXT("%sBoss_Slash_05_Montage.Boss_Slash_05_Montage"), AnimDir),
		210.f, 120.f, DemoBalance::BossSlash5Damage, 4.0f);
	if (Slash5)
	{
		Slash5->PoiseDamage = DemoBalance::BossSlash5Poise;
		Slash5->TelegraphTime = 0.32f;
		Slash5->RecoverTime = 0.38f;
		Slash5->HitWindowStartRatio = 0.22f;
		Slash5->HitWindowEndRatio = 0.50f;
		Slash5->Weight = 1.8f;
		Slash5->bOnlyInPhase2 = true;
	}

	// ---------------- 招式 1.6：狂化快爪·三（三连收招）----------------
	// Melee_C_Fast 是快爪三连的最后一下，比前两下略沉 —— 给一点点收招后摇，
	// 让玩家在这三连打完之后有一个明确的"可以插一刀"的窗口。
	UBossAttackData* Slash6 = MakeDefaultAttack(TEXT("Slash_06"), EBossAttackType::MeleeSlash,
		*FString::Printf(TEXT("%sBoss_Slash_06_Montage.Boss_Slash_06_Montage"), AnimDir),
		280.f, 160.f, DemoBalance::BossSlash6Damage, 5.5f);
	if (Slash6)
	{
		Slash6->PoiseDamage = DemoBalance::BossSlash6Poise;
		Slash6->TelegraphTime = 0.40f;
		Slash6->RecoverTime = 0.62f;            // 收招比前两下长：这是三连的"呼吸口"
		Slash6->HitWindowStartRatio = 0.24f;
		Slash6->HitWindowEndRatio = 0.52f;
		Slash6->Weight = 1.2f;
		Slash6->bOnlyInPhase2 = true;
	}

	// ---------------- 招式 1.7~1.9：重砸三连（Melee_A/B/C_Slow）----------------
	//
	// 补这一类招的理由：原来整套招式的起手都在 0.32~0.85s 之间，速度差太小，
	// 玩家的应对永远是"看到红就弹"，读不到"这一招多重、值不值得冒险去弹"。
	// Slow 变体的摆手是"抬起整个上半身再砸下"，动作幅度大、前摇长（0.80~0.95s）：
	//   · 对玩家 = 最容易读、最该弹的招 → 弹反的成功率与"敢不敢读"真正挂钩
	//   · 代价   = 伤害/削韧全场最高，无视它就要付最重的代价
	// 于是招式表第一次有了【节奏差】：快招逼反应，重砸逼判断。
	//
	// ⚠ 判定窗口必须往后挪：Slow 变体"刀还没落下来"的前半段不能判定，
	//   窗口取 0.34~0.62（快招是 0.22~0.50），否则会出现"挥到一半就打完了"。
	UBossAttackData* Heavy1 = MakeDefaultAttack(TEXT("Heavy_01"), EBossAttackType::MeleeSlash,
		*FString::Printf(TEXT("%sBoss_Heavy_01_Montage.Boss_Heavy_01_Montage"), AnimDir),
		250.f, 150.f, DemoBalance::BossHeavy1Damage, 6.f);
	if (Heavy1)
	{
		Heavy1->PoiseDamage = DemoBalance::BossHeavy1Poise;
		Heavy1->TelegraphTime = 0.95f;          // 全场最长的前摇：这是"送分题"，要让人读得出来
		Heavy1->RecoverTime = 1.15f;            // 后摇也长 → 弹/闪成功后的输出窗口最大
		Heavy1->HitWindowStartRatio = 0.34f;
		Heavy1->HitWindowEndRatio = 0.62f;
		Heavy1->Weight = 1.5f;
		Heavy1->bOnlyInPhase2 = false;          // 一阶段就给：让玩家先在"慢招"上练会弹反
	}

	// ---------------- 招式 1.8：重砸·横扫（覆盖面最广的一记）----------------
	// IdealRange 比另外两记重砸更远（300），专门治"站在中距离刮痧"的玩家。
	UBossAttackData* Heavy2 = MakeDefaultAttack(TEXT("Heavy_02"), EBossAttackType::MeleeSlash,
		*FString::Printf(TEXT("%sBoss_Heavy_02_Montage.Boss_Heavy_02_Montage"), AnimDir),
		300.f, 170.f, DemoBalance::BossHeavy2Damage, 7.f);
	if (Heavy2)
	{
		Heavy2->PoiseDamage = DemoBalance::BossHeavy2Poise;
		Heavy2->TelegraphTime = 0.85f;
		Heavy2->RecoverTime = 1.05f;
		Heavy2->HitWindowStartRatio = 0.34f;
		Heavy2->HitWindowEndRatio = 0.62f;
		Heavy2->Weight = 1.3f;
		Heavy2->bOnlyInPhase2 = true;
	}

	// ---------------- 招式 1.9：重砸·收招（全场最重的单发近战）----------------
	// 200 伤害 = 玩家血量的 10%，但前摇 0.80s、后摇 1.35s 全场最长 ——
	// 把它放在连段的末尾：玩家扛过三连的重砸，就拿到一个完整的输出回合。
	UBossAttackData* Heavy3 = MakeDefaultAttack(TEXT("Heavy_03"), EBossAttackType::MeleeSlash,
		*FString::Printf(TEXT("%sBoss_Heavy_03_Montage.Boss_Heavy_03_Montage"), AnimDir),
		270.f, 160.f, DemoBalance::BossHeavy3Damage, 9.f);
	if (Heavy3)
	{
		Heavy3->PoiseDamage = DemoBalance::BossHeavy3Poise;
		Heavy3->TelegraphTime = 0.80f;
		Heavy3->RecoverTime = 1.35f;            // 全场最长的空档 = 对读完招的玩家的奖励
		Heavy3->HitWindowStartRatio = 0.36f;
		Heavy3->HitWindowEndRatio = 0.64f;
		Heavy3->Weight = 1.0f;
		Heavy3->bOnlyInPhase2 = true;
	}

	// ---------------- 招式 2.5：腾空扑击（中距离专用）----------------
	//
	// 补的是距离带上的洞：突进斩从 640cm 外起手，近战只到 280cm，
	// 中间 400~500cm 这段 BOSS 只能干走位 —— 观感就是"它犹豫了一下"。
	// 扑击专门填这个带：起跳 → 扑过来 → 落地砸。
	// 和突进斩的区分（两者不抢戏）：
	//   · 突进斩 = 长距离贴地冲刺（1100cm/s）+ 拖刀与收尾【两段】判定
	//   · 扑击   = 中距离腾空（1250cm/s，更短更快）+ 只有落地【一段】判定
	// 类型仍走 ChargeSlash（位移由代码推，动画只负责"扑"的姿势），
	// 所以不需要新增招式类型、也不需要新的技能类。
	// ---------------- 招式 2.1：回旋横扫（2026-09-30 新增）----------------
	// 招式表此前有一个结构性缺陷：Melee_A / Melee_A_Fast / Melee_A_Slow 是
	// 【同一个挥击动作的三个速度版本】，所以 Slash_01、Slash_04、Heavy_01 在玩家眼里
	// 就是"同一个挥砍，只是快中慢" —— 这是"招式单一"的真正来源，不是出招频率。
	// Spin（转体一圈）是唯一一个和所有直线挥砍都不同的动作，补的就是这一类。
	// 手感定位：转体本身要花时间 → 前摇长（0.78s，好读）→ 后摇也长（1.0s，好罚），
	//          覆盖面广（Tolerance 180）→ 治"贴身绕后"的玩家。
	UBossAttackData* Spin1 = MakeDefaultAttack(TEXT("Spin_01"), EBossAttackType::MeleeSlash,
		*FString::Printf(TEXT("%sBoss_Spin_01_Montage.Boss_Spin_01_Montage"), AnimDir),
		280.f, 180.f, DemoBalance::BossSpin1Damage, 8.f);
	if (Spin1)
	{
		Spin1->PoiseDamage = DemoBalance::BossSpin1Poise;
		Spin1->TelegraphTime = 0.78f;
		Spin1->RecoverTime = 1.0f;
		Spin1->HitWindowStartRatio = 0.30f;   // 转体前半圈还没扫到 → 判定从 30% 才开始
		Spin1->HitWindowEndRatio = 0.70f;     // 转满一圈，判定段比普通挥砍长
		Spin1->Weight = 1.4f;
		Spin1->bOnlyInPhase2 = false;         // 一阶段就有：新动作要让玩家早点见到
	}

	// ---------------- 招式 2.2：回旋横扫·反向（二阶段）----------------
	// 反向转体：同一个概念换个方向，观感上仍是"另一个动作"而不是同一招加速。
	UBossAttackData* Spin2 = MakeDefaultAttack(TEXT("Spin_02"), EBossAttackType::MeleeSlash,
		*FString::Printf(TEXT("%sBoss_Spin_02_Montage.Boss_Spin_02_Montage"), AnimDir),
		300.f, 190.f, DemoBalance::BossSpin2Damage, 9.f);
	if (Spin2)
	{
		Spin2->PoiseDamage = DemoBalance::BossSpin2Poise;
		Spin2->TelegraphTime = 0.62f;         // 二阶段更快，但仍是"看得见"的量级
		Spin2->RecoverTime = 0.88f;
		Spin2->HitWindowStartRatio = 0.28f;
		Spin2->HitWindowEndRatio = 0.72f;
		Spin2->Weight = 1.3f;
		Spin2->bOnlyInPhase2 = true;
	}

	// ---------------- 招式 2.3：大幅挥出（2026-09-30 新增）----------------
	// E_Ability_Throw 的 MSA 变体：大开大合、手抬得高、收招慢。
	// 和"贴身挥砍""突进""扑击""回旋"四者观感都不重叠 → 第五类动作。
	// 定位：打中远距离（420cm），伤害偏低但收招慢，作用是"别让玩家站远了白嫖"。
	UBossAttackData* Throw1 = MakeDefaultAttack(TEXT("Throw_01"), EBossAttackType::MeleeSlash,
		*FString::Printf(TEXT("%sBoss_Throw_01_Montage.Boss_Throw_01_Montage"), AnimDir),
		420.f, 220.f, DemoBalance::BossThrow1Damage, 6.5f);
	if (Throw1)
	{
		Throw1->PoiseDamage = DemoBalance::BossThrow1Poise;
		Throw1->TelegraphTime = 0.72f;
		Throw1->RecoverTime = 0.90f;
		Throw1->HitWindowStartRatio = 0.36f;  // 抬手那半段没杀伤 → 判定靠后
		Throw1->HitWindowEndRatio = 0.66f;
		Throw1->Weight = 1.2f;
		Throw1->bOnlyInPhase2 = false;
	}

	UBossAttackData* Pounce = MakeDefaultAttack(TEXT("Pounce"), EBossAttackType::ChargeSlash,
		*FString::Printf(TEXT("%sBoss_Pounce_Dash_Montage.Boss_Pounce_Dash_Montage"), AnimDir),
		480.f, 260.f, DemoBalance::BossPounceImpactDamage, 5.f);
	if (Pounce)
	{
		Pounce->PoiseDamage = DemoBalance::BossPounceImpactPoise;
		Pounce->TelegraphTime = 0.62f;          // 比突进斩(0.70)短：扑击靠"突然"，不靠"蓄势"
		Pounce->RecoverTime = 0.85f;
		Pounce->Weight = 1.8f;
		Pounce->bCanBeParried = true;
		Pounce->bCanBeBlocked = true;
		Pounce->HitWindowStartRatio = 0.42f;    // 判定落在"落地那一砸"，不是腾空途中
		Pounce->HitWindowEndRatio = 0.72f;

		FBossChargeParams& P = Pounce->Charge;
		P.ChargeDistance = DemoBalance::BossPounceChargeDistance;
		P.ChargeSpeed = DemoBalance::BossPounceChargeSpeed;
		P.ChargeDuration = 0.24f;
		P.MinChargeDuration = 0.18f;
		P.MaxChargeDuration = 0.50f;
		P.MaxChargeDistance = DemoBalance::BossPounceMaxChargeDistance;
		P.StopDistance = DemoBalance::BossPounceStopDistance;

		// 扑击途中不判定：腾空段扫到人会很怪（人在半空、判定却在身前扫）。
		// 只有落地那一下算数 —— 于是"躲扑击"就一个答案：横向让开落点。
		P.bDamageAlongDash = false;
		P.DashDamage = DemoBalance::BossPounceDashDamage;
		P.DashPoiseDamage = DemoBalance::BossPounceDashPoise;

		P.ImpactMontage = LoadBossMontage(TEXT("/Game/Boss/Animation/Boss_Pounce_Impact_Montage.Boss_Pounce_Impact_Montage"));
		P.ImpactRadius = 280.f;                 // 落地砸的范围比突进斩收尾(320)小一圈
		P.ImpactHalfAngleDeg = 110.f;           // 但更宽：落地是"砸一圈"，不是"劈一条线"
		P.ImpactDamage = DemoBalance::BossPounceImpactDamage;
		P.ImpactPoiseDamage = DemoBalance::BossPounceImpactPoise;
		P.bImpactCanBeParried = true;
		P.bImpactCanBeBlocked = true;
		P.KnockbackStrength = DemoBalance::BossMaxKnockbackSpeed;
		P.KnockbackUpward = DemoBalance::BossMaxKnockbackUpward;
		P.bSpawnShockwave = false;              // 远程元素一律不开（同突进斩）

		// 二阶段：扑得更远、落地砸更大
		Pounce->Phase2ChargeDistanceScale = 1.25f;
		Pounce->Phase2ImpactRadiusScale = 1.2f;
		Pounce->Phase2TelegraphScale = 0.8f;
		Pounce->bUpgradeInPhase2 = true;
	}

	// ---------------- 招式 2：远程弹幕（已按需求移除）----------------
	// 2026-09-28：用户决定暂不做 BOSS 远程攻击，整段删除。
	// 若日后要恢复，把此段加回来、并在下面 Phase1/Phase2 的 Attacks 与 AttackPattern 里加回 Barrage 即可。
	// ⚠ 注意：大招（Ultimate）自身仍带一段"三圈环形弹幕"，那是大招演出的一部分，不在本次移除范围。

	// ---------------- 招式 3：突进斩（拉近距离 + 收尾重斩）----------------
	//
	// 这一招取代了原来的「跳砸」。换掉的理由：
	//   跳砸是"离地飞过去砸"，判定点在落地瞬间，玩家读招成本高、命中反馈又偏钝；
	//   突进斩是"贴地冲过去砍"，全程都在地上、拖刀 + 收尾两段判定，
	//   距离可控（匀速推力，填多少走多少）、方向也始终朝着玩家，压迫感更直接。
	UBossAttackData* Charge = MakeDefaultAttack(TEXT("ChargeSlash"), EBossAttackType::ChargeSlash,
		*FString::Printf(TEXT("%sBoss_Charge_Dash_Montage.Boss_Charge_Dash_Montage"), AnimDir),
		640.f, 380.f, DemoBalance::BossChargeDamage, 6.f);

	if (Charge)
	{
		Charge->PoiseDamage = DemoBalance::BossChargePoise;
		Charge->TelegraphTime = 0.70f;
		Charge->RecoverTime = 0.95f;
		Charge->Weight = 2.f;
		Charge->bCanBeParried = true;          // 蓄势那一段可以弹 —— 这是它和"红招"的区别
		Charge->bCanBeBlocked = true;
		Charge->HitWindowStartRatio = 0.30f;   // 收尾斩的判定窗口
		Charge->HitWindowEndRatio = 0.58f;

		FBossChargeParams& C = Charge->Charge;
		C.ChargeDistance = 560.f;
		C.ChargeDuration = 0.28f;
		// 速度上限是"别把玩家撞飞"的关键：以前固定 0.28s 冲完，
		// 玩家站 961cm 外时速度高达 3431cm/s（34 米/秒），BOSS 是一堵墙撞过来。
		// 现在恒定 1100cm/s，距离远只是冲得久；再远的超长距离直接截断，分两次走位。
		C.ChargeSpeed = DemoBalance::BossChargeSpeed;
		C.MinChargeDuration = DemoBalance::BossChargeMinDuration;
		C.MaxChargeDuration = DemoBalance::BossChargeMaxDuration;
		C.MaxChargeDistance = DemoBalance::BossChargeMaxDistance;
		C.StopDistance = 175.f;

		// 拖刀：突进途中碰到就吃一次伤害。完整的一招是"两段判定"，
		// 玩家横向闪开第一段、再往侧后方绕开第二段，操作空间比单段大得多。
		C.bDamageAlongDash = true;
		C.DashDamage = DemoBalance::BossChargeDashDamage;
		C.DashPoiseDamage = DemoBalance::BossChargeDashPoise;

		C.ImpactMontage = LoadBossMontage(TEXT("/Game/Boss/Animation/Boss_Charge_Impact_Montage.Boss_Charge_Impact_Montage"));
		C.ImpactRadius = 320.f;
		C.ImpactHalfAngleDeg = 78.f;           // 前方 ±78°：绕到侧后方就能躲掉
		C.ImpactDamage = DemoBalance::BossChargeImpactDamage;
		C.ImpactPoiseDamage = DemoBalance::BossChargeImpactPoise;
		C.bImpactCanBeParried = true;
		C.bImpactCanBeBlocked = true;
		// 收尾冲击击退：最初 560 水平 + 170 上抛会把玩家"砍飞"，
		// 上抛让玩家离地 → 空中没有地面摩擦 → 水平速度一路带着人飞出去。
		// 现在上抛归零、水平压到 110（并被 ApplyAreaImpact 的硬上限再夹一次），
		// 只留一下"被震开"的反馈。真正的"打飞"其实来自突进速度，见下面的 ChargeSpeed。
		C.KnockbackStrength = DemoBalance::BossMaxKnockbackSpeed;
		C.KnockbackUpward = DemoBalance::BossMaxKnockbackUpward;

		// 二阶段：冲得更远、砍得更大，并且收尾带一圈震荡波（逼玩家二次走位）
		Charge->Phase2ChargeDistanceScale = 1.3f;
		Charge->Phase2ImpactRadiusScale = 1.25f;
		Charge->Phase2TelegraphScale = 0.75f;
		Charge->bUpgradeInPhase2 = true;

		FBossBarrageParams Shock;
		Shock.bRingPattern = true;
		// 一圈 8 发（以前 10）：8 发的间隙已经足够玩家侧身钻过去，
		// 10 发会把"找缺口"变成"纯运气"。
		Shock.ProjectileCount = 8;
		Shock.SpawnSocket = NAME_None;
		Shock.SpawnForwardOffset = 0.f;
		Shock.SpawnHeightOffset = 40.f;
		Shock.bAimAtTarget = false;
		Shock.Projectile.Speed = 620.f;
		Shock.Projectile.Damage = DemoBalance::BossChargeShockwaveDamage;
		Shock.Projectile.PoiseDamage = DemoBalance::BossChargeShockwavePoise;
		Shock.Projectile.LifeSpan = 3.f;
		Shock.Projectile.CollisionRadius = 14.f;
		Shock.Projectile.bCanBeParried = false;
		Shock.Projectile.bCanBeBlocked = true;

		// 震荡波是一圈同时铺开，同样按"表现预算"压一下：
		// 拖尾短一截够读出扩散方向就行，点光关掉（它只存在 3s，看不到照明收益）
		Shock.Projectile.TrailSampleCount = 6;
		Shock.Projectile.TrailColor = FLinearColor(0.95f, 0.55f, 0.18f, 1.f);   // 橙色：和普通弹幕的蓝区分开
		Shock.Projectile.CoreColor = FLinearColor(1.f, 0.62f, 0.22f, 1.f);
		Shock.Projectile.CoreVisualScale = 0.6f;
		Shock.Projectile.bCastGlowLight = false;
		Shock.Projectile.ImpactBurstRadiusScale = 1.4f;
		Shock.Projectile.ImpactBurstDuration = 0.20f;
		// ⚠ 2026-09-29：突进斩收尾的"一圈震荡波弹幕"关掉了。
		// 两个原因：① 远程攻击按需求整体移除（只剩大招自带的环形弹幕属于大招演出）；
		//          ② 弹体的核心球 / 拖尾用的 MI_BossGlow 父材质 M_BossSpriteAdd 编译失败 →
		//             回退默认材质 → 场上会多出一堆灰球（用户反馈的"BOSS 头上那个球"）。
		// 收尾判定本身（前方扇形 ApplyAreaImpact）完全保留，招式强度不受影响。
		// 想恢复：把这里改回 true，并确保材质已修好。
		C.bSpawnShockwave = false;
		C.Shockwave = Shock;
	}

	// ---------------- 大招：蓄力 → 三圈环形弹幕 → 突进斩收尾 ----------------
	UBossAttackData* Ultimate = MakeDefaultAttack(TEXT("Ultimate"), EBossAttackType::Ultimate,
		*FString::Printf(TEXT("%sBoss_Ult_Charge_Montage.Boss_Ult_Charge_Montage"), AnimDir),
		0.f, 100000.f, DemoBalance::BossUltimateBarrageDamage, 24.f);

	if (Ultimate)
	{
		Ultimate->PoiseDamage = DemoBalance::BossUltimatePoise;
		Ultimate->TelegraphTime = 1.0f;         // 大招的"起手预警"再长一点，给足跑位时间
		Ultimate->RecoverTime = 1.4f;
		Ultimate->bIgnoreRangeCheck = true;     // 任何距离都能放
		Ultimate->bOnlyInPhase2 = false;        // 一阶段也能放（由 Phase 的开关决定要不要给）
		Ultimate->bCanBeParried = false;
		Ultimate->bCanBeBlocked = true;
		Ultimate->bInterruptibleByParry = false;  // 大招不可打断（本来也不可弹反）
		Ultimate->Weight = 5.f;

		Ultimate->ChargeMontage = Ultimate->Montage;
		Ultimate->ChargeLoopMontage = LoadBossMontage(TEXT("/Game/Boss/Animation/Boss_Ult_Loop_Montage.Boss_Ult_Loop_Montage"));
		Ultimate->ChargeOutMontage = LoadBossMontage(TEXT("/Game/Boss/Animation/Boss_Ult_Out_Montage.Boss_Ult_Out_Montage"));
		Ultimate->ChargeDuration = 1.6f;
		Ultimate->bInvincibleWhileCharging = false;   // 玩家可以在蓄力时打断（打空韧性）

		// 2 圈 × 8 发 = 16 颗（以前 3 圈 × 12 发 = 36 颗）。
		// 环形弹幕的难点应该是"读圈与圈之间的空隙"，而不是"屏幕上太多球"。
		// 16 颗时每圈的空隙肉眼可辨，玩家能规划出一条穿过去的路线。
		Ultimate->RingCount = 2;
		Ultimate->ProjectilesPerRing = 8;
		Ultimate->RingInterval = 0.42f;
		Ultimate->RingAngleStepDeg = 22.5f;

		Ultimate->Barrage.Projectile.Speed = 700.f;
		Ultimate->Barrage.Projectile.Damage = DemoBalance::BossUltimateBarrageDamage;
		Ultimate->Barrage.Projectile.PoiseDamage = DemoBalance::BossUltimatePoise;
		Ultimate->Barrage.Projectile.LifeSpan = 6.f;
		Ultimate->Barrage.Projectile.CollisionRadius = 14.f;
		Ultimate->Barrage.Projectile.bCanBeParried = false;
		Ultimate->Barrage.Projectile.bCanBeBlocked = true;

		// ---- 环形弹幕的"表现预算"要单独压 ----
		// 16 颗同时在飞，如果每颗都带长拖尾 + 一盏动态点光，就是 16 个点光 + 上百个实例在刷新。
		// 折中：拖尾留 5 段（够读出方向），点光关掉，核心球也缩小一号
		// —— 环形弹幕靠"数量"表达压迫感，不需要每颗都做得又大又亮。
		Ultimate->Barrage.Projectile.TrailSampleCount = 5;
		Ultimate->Barrage.Projectile.bCastGlowLight = false;
		Ultimate->Barrage.Projectile.CoreVisualScale = 0.5f;
		Ultimate->Barrage.Projectile.ImpactBurstRadiusScale = 1.4f;
		Ultimate->Barrage.Projectile.ImpactBurstDuration = 0.22f;

		Ultimate->bEndWithChargeFinisher = true;
		if (Charge)
		{
			// 收尾突进复用突进斩那套参数（但收尾是整圈冲击，见 UGA_BossUltimate::OnDashFinished）
			Ultimate->Charge = Charge->Charge;
			// 收尾的蓄势短一点：大招已经在前面蓄了 1.6s，再站桩玩家会失去耐心
			Ultimate->Charge.ChargeDuration = 0.24f;
			Ultimate->Charge.StopDistance = 150.f;
		}

		// 二阶段：多一圈、弹速更快
		Ultimate->Phase2ExtraRings = 1;
		Ultimate->Phase2ExtraProjectiles = 4;
		Ultimate->Phase2ProjectileSpeedScale = 1.2f;
		Ultimate->bUpgradeInPhase2 = true;
	}

	// ---------------- 阶段 1：血量 100% ~ 55%，两招（近战挥砍 / 突进斩），无大招、无远程 ----------------
	UBossPhaseData* Phase1 = NewObject<UBossPhaseData>(this, TEXT("BossPhase1"));
	Phase1->PhaseName = FText::FromString(TEXT("第一阶段"));
	Phase1->HealthRatioThreshold = 1.0f;
	Phase1->Attacks.Add(Slash1);
	Phase1->Attacks.Add(Slash2);
	Phase1->Attacks.Add(Slash3);
	Phase1->Attacks.Add(Heavy1);
	Phase1->Attacks.Add(Charge);
	Phase1->Attacks.Add(Pounce);
	// 新增的两类动作（回旋 / 大幅挥出）—— 一阶段就给，让"不同动作"早点被玩家看到
	Phase1->Attacks.Add(Spin1);
	Phase1->Attacks.Add(Throw1);
	// 出招间隔放宽：原来 1.4~2.6s 太密（叠加各招自己的后摇后几乎不停手），
	// 玩起来像"BOSS 一直在出招"。放到 2.2~3.4s，中间留出明确的走位 / 试探窗口。
	// 出招【间隔】保持 2.2~3.4：这是给玩家的对峙/走位窗口。
	// ⚠ 别为了"看起来招式多"去缩短它 —— 用户要的是【招式种类不同】，不是出招更密
	//   （2026-09-30 曾误收到 1.5~2.3 被明确否掉，已还原）。
	Phase1->AttackIntervalMin = 2.2f;
	Phase1->AttackIntervalMax = 3.4f;

	// 固定背板：一阶段八拍循环。
	// 顺序固定，玩家能背下来、能预判下一招，从而"练"出来而不是纯靠反应。
	//
	// ⚠⚠ 2026-09-30 从六拍扩到八拍，并补进两类【全新动作】：
	//   原来六拍里的 Slash_01(Melee_A)、Slash_04(Melee_A_Fast)、Heavy_01(Melee_A_Slow)
	//   本质是同一个挥击的三个速度版本 → 玩家眼里"招式单一"的根源在这，不在出招频率。
	//   现在八拍覆盖 7 类不同动作：
	//     挥砍A / 挥砍B / 回旋转体 / 重砸 / 突进 / 大幅挥出 / 挥砍C / 腾空扑击
	//   排法上刻意把同源动作（A 与 A_Slow）隔开三拍，
	//   并把最慢最好读的重砸放第 4 拍（教学：这招能弹）、最突然的扑击收尾（教学：横向让开落点）。
	Phase1->AttackPattern = {
		FName(TEXT("Slash_01")),      // Melee_A        直线挥砍
		FName(TEXT("Slash_02")),      // Melee_B        直线挥砍（另一套）
		FName(TEXT("Spin_01")),       // 转体一圈       ← 全新动作
		FName(TEXT("Heavy_01")),      // Melee_A_Slow   慢重砸（最好读，送分题）
		FName(TEXT("ChargeSlash")),   // 长距离突进斩
		FName(TEXT("Throw_01")),      // 大开大合挥出   ← 全新动作
		FName(TEXT("Slash_03")),      // Melee_C        直线挥砍（第三套）
		FName(TEXT("Pounce"))         // 中距离腾空扑击
	};
	Phase1->MoveSpeed = 360.f;
	Phase1->TelegraphTimeScale = 1.f;
	Phase1->DamageScale = 1.f;
	Phase1->UltimateAttack = nullptr;           // 一阶段不给大招

	// ---------------- 阶段 2：血量 < 55%，复用同一批招式（自动强化）+ 大招 ----------------
	UBossPhaseData* Phase2 = NewObject<UBossPhaseData>(this, TEXT("BossPhase2"));
	Phase2->PhaseName = FText::FromString(TEXT("第二阶段"));
	Phase2->HealthRatioThreshold = 0.55f;

	// 复用同一批招式资产：二阶段不再另建一套，只是把大招也放进池子。
	// "强化"由 UBossAttackData 的 Phase2* 倍率自动完成（伤害↑ 动作更快 起手更短 后摇更短 弹更多）。
	Phase2->Attacks.Add(Slash1);
	Phase2->Attacks.Add(Slash2);
	Phase2->Attacks.Add(Slash3);
	Phase2->Attacks.Add(Slash4);
	Phase2->Attacks.Add(Slash5);
	Phase2->Attacks.Add(Slash6);
	Phase2->Attacks.Add(Heavy1);
	Phase2->Attacks.Add(Heavy2);
	Phase2->Attacks.Add(Heavy3);
	Phase2->Attacks.Add(Charge);
	Phase2->Attacks.Add(Pounce);
	Phase2->Attacks.Add(Ultimate);
	Phase2->Attacks.Add(Spin1);
	Phase2->Attacks.Add(Spin2);   // 反向回旋：二阶段专属
	Phase2->Attacks.Add(Throw1);

	// 二阶段依旧更快（压迫感来自"更短的空档 + 更快的起手"），但不再密到不停手。
	Phase2->AttackIntervalMin = 1.6f;
	Phase2->AttackIntervalMax = 2.6f;

	// 固定背板：十一拍循环（2026-09-30 从八拍扩到十一拍）。
	// 二阶段的招式更多，排成更长的序列 —— 玩家得换一套背，换阶段的同时也换了"考题"。
	// 大招不写进背板：它由自己的冷却门调度，作为可预期的插曲出现，不破坏常规节奏的记忆。
	//
	// 排法上的讲究（不是随便排的）：
	//   · 前四拍是"快 → 慢 → 回旋 → 快"，和一阶段同构但换了动作 → 换阶段不换语法，学得会
	//   · 扑击夹在两个动作中间 → 突然性最强
	//   · 反向回旋（Spin_02）放第 9 拍：同一概念换方向，考玩家"方向反了要重新读"
	//   · 最后两拍 Heavy_02 → Heavy_03 连着重砸二连：整套循环的收尾是"最重、后摇最长"的
	//     两招，也就是玩家每轮循环结束时固定拿到的那个大输出窗口 ——
	//     让它出现在【固定位置】，玩家才敢规划"在这儿开一套连段"。
	Phase2->AttackPattern = {
		FName(TEXT("Slash_04")),      // Melee_A_Fast   快爪
		FName(TEXT("Heavy_01")),      // Melee_A_Slow   慢重砸
		FName(TEXT("Spin_01")),       // 回旋转体        ← 全新动作
		FName(TEXT("Slash_05")),      // Melee_B_Fast   快爪
		FName(TEXT("ChargeSlash")),   // 长距离突进斩
		FName(TEXT("Throw_01")),      // 大开大合挥出    ← 全新动作
		FName(TEXT("Pounce")),        // 中距离腾空扑击
		FName(TEXT("Slash_06")),      // Melee_C_Fast   快爪
		FName(TEXT("Spin_02")),       // 反向回旋        ← 二阶段专属
		FName(TEXT("Heavy_02")),
		FName(TEXT("Heavy_03"))
	};
	Phase2->MoveSpeed = 420.f;                  // 跑得更快（狂化）
	Phase2->TelegraphTimeScale = 0.85f;         // 起手更快 = 更难读招
	Phase2->DamageScale = 1.2f;
	Phase2->MoveSpeedScale = 1.f;

	// 阶段转换演出：蹲下蓄力 → 循环 → 爆发起身（给玩家一个"喘息 + 输出"的窗口）
	Phase2->EnterMontage = LoadBossMontage(TEXT("/Game/Boss/Animation/Boss_Phase2_Enter_Montage.Boss_Phase2_Enter_Montage"));
	Phase2->EnterLoopMontage = LoadBossMontage(TEXT("/Game/Boss/Animation/Boss_Phase2_Loop_Montage.Boss_Phase2_Loop_Montage"));
	Phase2->EnterOutMontage = LoadBossMontage(TEXT("/Game/Boss/Animation/Boss_Phase2_Out_Montage.Boss_Phase2_Out_Montage"));
	Phase2->EnterDuration = 3.0f;
	Phase2->bInvincibleDuringEnter = false;

	// 大招调度：进二阶段后至少等 5s，之后每 20s 最多一次，血量低于 20% 强制爆发
	Phase2->UltimateAttack = Ultimate;
	Phase2->UltimateCooldown = 20.f;
	Phase2->UltimateInitialDelay = 5.f;
	Phase2->UltimateMaxCount = 0;               // 0 = 不限次数
	Phase2->UltimateDesperateHealthRatio = 0.2f;

	// 走位：贴太近会主动后撤（弹幕型 BOSS 的距离管理）
	// 收着点：以前 320cm 内就退 480cm，玩家往前走两步 BOSS 就滑出去半张图，
	// 看起来像"我能推着 BOSS 走"。现在改成贴得更近才退、且只退一小步。
	Phase2->BackoffRange = 240.f;
	Phase2->BackoffDistance = 280.f;

	Phases.Add(Phase1);
	Phases.Add(Phase2);

	// ---- 新增招式的资产自检 ----
	// 蒙太奇是【资产】，C++ 只能加载、没法凭空造。重砸三连 / 扑击这四招的蒙太奇由
	// Tools/SetupBossKhaimera.py 生成；没跑过脚本时，这里一次性把缺的报出来 ——
	// 否则表现为"BOSS 这一拍站着不动"，光看画面完全查不出原因。
	{
		TArray<FString> MissingNew;
		auto CheckMontage = [&MissingNew](const UBossAttackData* Attack, const TCHAR* AssetName)
		{
			if (Attack && !Attack->Montage) { MissingNew.Add(AssetName); }
		};
		CheckMontage(Heavy1, TEXT("Boss_Heavy_01_Montage"));
		CheckMontage(Heavy2, TEXT("Boss_Heavy_02_Montage"));
		CheckMontage(Heavy3, TEXT("Boss_Heavy_03_Montage"));
		CheckMontage(Pounce, TEXT("Boss_Pounce_Dash_Montage"));
		if (Pounce && !Pounce->Charge.ImpactMontage)
		{
			MissingNew.Add(TEXT("Boss_Pounce_Impact_Montage"));
		}

		if (MissingNew.Num() > 0)
		{
			UE_LOG(LogDemoCombat, Warning,
				TEXT("[Boss][Brain] 新增招式缺 %d 个蒙太奇：%s —— 在编辑器 Output Log 跑一次 "
					 "K:/selfgame/demo/Tools/SetupBossKhaimera.py 即可生成"
					 "（缺的招式会被跳过，BOSS 不会因此卡住，只是这一拍不出招）"),
				MissingNew.Num(), *FString::Join(MissingNew, TEXT(", ")));
		}
	}

	// ---- 蒙太奇降级：宁可用同族的标准动作，也不要"这一拍站着不动" ----
	// 脚本没跑 / 新蒙太奇加载失败时，画面上的表现是 BOSS 这一拍不动，光看画面查不出原因。
	// 这里给每个新招配一个兜底动画：招式照出（前摇 / 后摇 / 伤害 / 判定窗口都还是新招的数值），
	// 只是摆手用的是标准变体 —— 节奏差还在，玩家至少能读到"这一招更慢更重"。
	auto FallbackMontage = [this](UBossAttackData* Attack, const TCHAR* FallbackPath, const TCHAR* AssetName)
	{
		if (Attack && !Attack->Montage)
		{
			Attack->Montage = LoadBossMontage(FallbackPath);
			if (Attack->Montage)
			{
				UE_LOG(LogDemoCombat, Warning,
					TEXT("[Boss][Brain] 招式「%s」缺 %s → 已降级用 %s（动作能出，但摆手不是重砸）。"
						 "跑一次 K:/selfgame/demo/Tools/SetupBossKhaimera.py 生成真正的动画"),
					*Attack->AttackName.ToString(), AssetName, FallbackPath);
			}
		}
	};
	FallbackMontage(Heavy1, TEXT("/Game/Boss/Animation/Boss_Slash_01_Montage.Boss_Slash_01_Montage"), TEXT("Boss_Heavy_01_Montage"));
	FallbackMontage(Heavy2, TEXT("/Game/Boss/Animation/Boss_Slash_02_Montage.Boss_Slash_02_Montage"), TEXT("Boss_Heavy_02_Montage"));
	FallbackMontage(Heavy3, TEXT("/Game/Boss/Animation/Boss_Slash_03_Montage.Boss_Slash_03_Montage"), TEXT("Boss_Heavy_03_Montage"));
	FallbackMontage(Pounce, TEXT("/Game/Boss/Animation/Boss_Charge_Dash_Montage.Boss_Charge_Dash_Montage"), TEXT("Boss_Pounce_Dash_Montage"));
	if (Pounce && !Pounce->Charge.ImpactMontage)
	{
		Pounce->Charge.ImpactMontage = LoadBossMontage(TEXT("/Game/Boss/Animation/Boss_Charge_Impact_Montage.Boss_Charge_Impact_Montage"));
	}

	UE_LOG(LogDemoCombat, Warning,
		TEXT("[Boss][Brain] 未配置阶段数据 → 已构建内置默认两阶段：阶段1 %d 招（无大招）/ 阶段2 %d 招（含大招），阈值 1.0 / 0.55"),
		Phase1->Attacks.Num(), Phase2->Attacks.Num());
}
