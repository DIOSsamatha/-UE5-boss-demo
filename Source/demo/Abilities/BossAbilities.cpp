#include "Abilities/BossAbilities.h"

#include "Boss/BossCharacterBase.h"
#include "Core/DemoCharacterBase.h"
#include "Core/DemoAbilitySystemLibrary.h"
#include "Core/DemoGameplayTags.h"
#include "Combat/DemoProjectile.h"

#include "AbilitySystemComponent.h"
#include "Abilities/Tasks/AbilityTask_PlayMontageAndWait.h"
#include "Abilities/Tasks/AbilityTask_WaitDelay.h"
#include "Abilities/Tasks/AbilityTask_ApplyRootMotionConstantForce.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"

// ==================================================================================
// 基类
// ==================================================================================
UGA_BossAttackBase::UGA_BossAttackBase()
{
	// 由 AI 直接激活，不做预测；一个能力实例负责整段出招
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::ServerOnly;

	// UE5.7：AbilityTags 已废弃，用 SetAssetTags 设默认标签
	SetAssetTags(FGameplayTagContainer());

	// 出招期间挂上 State.Attacking —— Brain 靠这个标签判断"这一招放完了没"
	ActivationOwnedTags.AddTag(DemoTag::State_Attacking);
}

void UGA_BossAttackBase::SetPendingAttack(UBossAttackData* InAttack)
{
	PendingAttack = InAttack;
}

ABossCharacterBase* UGA_BossAttackBase::GetBoss() const
{
	return Cast<ABossCharacterBase>(GetAvatarCharacter());
}

UCharacterMovementComponent* UGA_BossAttackBase::GetMoveComp() const
{
	const ACharacter* Character = GetAvatarCharacter();
	return Character ? Character->GetCharacterMovement() : nullptr;
}

bool UGA_BossAttackBase::IsPhase2() const
{
	const UAbilitySystemComponent* ASC = GetAvatarASC();
	return ASC && ASC->HasMatchingGameplayTag(DemoTag::State_Boss_Phase2);
}

float UGA_BossAttackBase::GetDamage() const
{
	return PendingAttack ? PendingAttack->GetEffectiveDamage(IsPhase2()) : 0.f;
}

float UGA_BossAttackBase::GetPoiseDamage() const
{
	return PendingAttack ? PendingAttack->GetEffectivePoiseDamage(IsPhase2()) : 0.f;
}

float UGA_BossAttackBase::GetPlayRate() const
{
	return PendingAttack ? FMath::Max(PendingAttack->GetEffectivePlayRate(IsPhase2()), 0.05f) : 1.f;
}

// ------------------------------------------------------------------ 激活 / 结束
void UGA_BossAttackBase::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	Super::ActivateAbility(Handle, ActorInfo, ActivationInfo, TriggerEventData);

	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	ABossCharacterBase* Boss = GetBoss();

	// 招式数据优先从角色身上取 —— Brain 在激活技能前把它写在 Boss 上。
	// 这条路径比"Brain 去取能力实例再 Set"可靠：InstancedPerActor 的实例是首次激活时才创建的，
	// 首次出招时实例还是 null，那条路会静默失败。
	if (!PendingAttack && Boss)
	{
		PendingAttack = Boss->GetPendingAttackData();
	}

	if (!Boss || !PendingAttack)
	{
		UE_LOG(LogDemoCombat, Error, TEXT("[Boss] 出招失败：Avatar=%s 招式数据=%s（检查 Brain 是否先调了 SetPendingAttackData）"),
			*GetNameSafe(GetAvatarCharacter()), *GetNameSafe(PendingAttack));
		EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
		return;
	}

	// 出招前把根位移模式掰正 —— 否则动画位移留在 Mesh 上，播完会"弹回去"，胶囊体反而不动
	Boss->EnsureRootMotionExtraction(TEXT("Boss 出招"));

	// 每次起手清零复仇计数：复仇数的是"上一次出招之后"又被揍了几下
	Boss->ResetRevengeCounter();

	// 整段出招期间贴地：招式蒙太奇来自 Root-Motion 目录，冲刺 / 重击类动作的根位移带垂直分量，
	// 被提取后会把胶囊体抬离地面（"BOSS 飘着打"），而且一浮空玩家的武器扫掠就够不着它。
	// 锁在一个水平面上比逐招改动画安全，也不影响水平突进 / 转向 / 寻路。
	SetBossGrounded(true);

	// 把本招的伤害 / 攻守规则写进角色：近战扫掠 PerformMeleeTrace 直接用这几个值
	Boss->SetCurrentAttackParams(GetDamage(), GetPoiseDamage(),
		PendingAttack->bCanBeParried, PendingAttack->bCanBeBlocked);

	// 是否可被完美弹反打断：ApplyHit 读取它决定"完美弹反是打断动画还是只免伤"
	Boss->SetCurrentAttackInterruptible(PendingAttack->bInterruptibleByParry);

	UE_LOG(LogDemoCombat, Log,
		TEXT("[Boss] 出招「%s」类型=%d %s 伤害=%.1f 削韧=%.1f 速率=%.2f"),
		*PendingAttack->AttackName.ToString(), static_cast<int32>(PendingAttack->AttackType),
		IsPhase2() ? TEXT("[二阶段·强化]") : TEXT("[一阶段]"),
		GetDamage(), GetPoiseDamage(), GetPlayRate());

	OnAttackStart();
}

void UGA_BossAttackBase::EndAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	bool bReplicateEndAbility, bool bWasCancelled)
{
	// 1) 关掉近战判定窗口（万一是在窗口开着的时候被打断的）
	if (ADemoCharacterBase* Character = Cast<ADemoCharacterBase>(GetAvatarCharacter()))
	{
		Character->SetMeleeHitWindowEnabled(false);
	}

	// 2) 停掉所有异步任务（先清回调再 EndTask，避免回调打到已经结束的逻辑上）
	StopActiveMontageTask();

	if (ActiveHitOnTask) { ActiveHitOnTask->EndTask(); ActiveHitOnTask = nullptr; }
	if (ActiveHitOffTask) { ActiveHitOffTask->EndTask(); ActiveHitOffTask = nullptr; }
	if (ActiveDelayTask) { ActiveDelayTask->EndTask(); ActiveDelayTask = nullptr; }
	if (ActiveDashTimeoutTask) { ActiveDashTimeoutTask->EndTask(); ActiveDashTimeoutTask = nullptr; }
	ActiveRootMotionTask = nullptr;

	// 3) 还回移速
	PinMovement(false);
	bDashActive = false;

	// 4) 解除贴地约束（还原成进入前的设置）
	SetBossGrounded(false);

	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}

void UGA_BossAttackBase::FinishAttack()
{
	// 突进还在进行：绝对不能在冲完之前结束技能，否则收尾判定就没机会结算了
	if (bDashActive)
	{
		return;
	}
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
}

// ------------------------------------------------------------------ 蒙太奇
void UGA_BossAttackBase::StopActiveMontageTask()
{
	// 单节点下不再用 UAbilityTask_PlayMontageAndWait（其蒙太奇实例不推进、永不回调）；
	// 改为让 Boss 停掉单节点 CurrentAsset 蒙太奇，并取消"播完"定时器回调。
	if (ABossCharacterBase* Boss = GetBoss())
	{
		Boss->StopBossMontage();
	}
	bMontageTaskActive = false;
}

bool UGA_BossAttackBase::PlayMontageTask(UAnimMontage* Montage, float Rate,
	FName TaskName, float RootMotionScale)
{
	if (!Montage)
	{
		return false;
	}

	// 切段（蓄力→循环→收力；蓄势→突进→收尾斩）时先干净地停掉上一段：
	// 否则新蒙太奇会把旧的顶掉、旧"播完"回调会误触发 → 技能被误判成"被打断"提前结束。
	StopActiveMontageTask();

	ABossCharacterBase* Boss = GetBoss();
	if (!Boss)
	{
		return false;
	}

	// ★ 单节点关键修复：改用 Boss->PlayBossMontage —— 它以 PlayAnimation 把蒙太奇设为当前资产（可见），
	// 并用定时器在其播完时触发回调（等价原 UAbilityTask_PlayMontageAndWait 的 OnCompleted）。
	// 原 UAbilityTask 在单节点下其蒙太奇实例不推进、永不回调 → 能力卡死在 Attack 状态。
	Boss->PlayBossMontage(Montage, Rate, RootMotionScale,
		FOnBossMontageFinished::CreateUObject(this, &UGA_BossAttackBase::HandleMontageTaskFinished));

	bMontageTaskActive = true;

	if (Montage->GetPlayLength() <= 0.f)
	{
		UE_LOG(LogDemoCombat, Warning,
			TEXT("[Boss] 蒙太奇 %s 播放时长为 0（空壳资产）→ 跑 Tools/SetupBossMontages.py 按源动画重建"),
			*GetNameSafe(Montage));
	}

	return true;
}

void UGA_BossAttackBase::HandleMontageTaskFinished(UAnimMontage* FinishedMontage)
{
	bMontageTaskActive = false;
	// 等价原 UAbilityTask_PlayMontageAndWait 的 OnCompleted：推进到下一阶段 / 收招。
	OnMontageCompleted();
}

void UGA_BossAttackBase::OnMontageCompleted()
{
	// 先摘掉自己的引用再往下走：
	// 蒙太奇任务是在自己的"播完回调"里自销毁的，这里若再去 EndTask 它属于重入调用。
	// 之后 PlayMontageTask 里的 StopActiveMontageTask 就不会去碰这个正在收尾的任务了。
	bMontageTaskActive = false;

	// 突进期间主蒙太奇播完了也不能结束技能 —— 收尾判定还没做
	if (bDashActive)
	{
		return;
	}

	FinishAttack();
}

void UGA_BossAttackBase::OnMontageCancelled()
{
	bMontageTaskActive = false;

	// 突进中"被中断"也不能就这么结束：收尾判定还必须走完
	if (bDashActive)
	{
		return;
	}
	FinishAttack();
}

void UGA_BossAttackBase::FaceTarget()
{
	ABossCharacterBase* Boss = GetBoss();
	if (!Boss)
	{
		return;
	}

	AActor* Target = Boss->GetCombatTarget();
	if (!Target)
	{
		return;
	}

	FVector Dir = Target->GetActorLocation() - Boss->GetActorLocation();
	Dir.Z = 0.f;
	if (Dir.IsNearlyZero())
	{
		return;
	}

	// 交给角色 Tick 恒速转过去，而不是在这里一下拧到位。
	// 起手时长通常 0.5~0.85s，720°/s 能在出招瞬间之前转完，所以不影响命中判定；
	// 但视觉上从"啪地闪现转向"变成了"转身面对你"，出招读得清楚得多。
	Boss->SetFacingTarget(Target->GetActorLocation(), /*TurnSpeedDegPerSec=*/720.f, /*MaxTurnTime=*/0.40f);
}

void UGA_BossAttackBase::PinMovement(bool bPin)
{
	UCharacterMovementComponent* MoveComp = GetMoveComp();
	if (!MoveComp)
	{
		return;
	}

	if (bPin)
	{
		if (!bMovementPinned)
		{
			SavedMaxWalkSpeed = MoveComp->MaxWalkSpeed;
			bMovementPinned = true;
		}
		MoveComp->MaxWalkSpeed = 0.f;
	}
	else if (bMovementPinned)
	{
		bMovementPinned = false;
		MoveComp->MaxWalkSpeed = SavedMaxWalkSpeed > 0.f ? SavedMaxWalkSpeed : 380.f;
	}
}

void UGA_BossAttackBase::SetBossGrounded(bool bGrounded)
{
	UCharacterMovementComponent* MoveComp = GetMoveComp();
	if (!MoveComp)
	{
		return;
	}

	if (bGrounded)
	{
		// 只在第一次进入时保存原值 —— 嵌套调用（比如大招里再走突进）会把
		// "我们改过的值"当成"原始值"存下来，退出时就还原不回去了。
		if (!bGroundedApplied)
		{
			bSavedConstrainToPlane = MoveComp->bConstrainToPlane;
			SavedPlaneConstraintNormal = MoveComp->GetPlaneConstraintNormal();
			SavedPlaneConstraintOrigin = MoveComp->GetPlaneConstraintOrigin();
			bGroundedApplied = true;
		}

		// ⚠ 关键修复：平面约束的原点必须显式设为 BOSS 当前位置！
		// UE 默认平面过世界原点 (0,0,0)，法线朝上 = 把胶囊中心压到 Z=0（脚下 130cm 处），
		// 招式结束后解除约束 BOSS 再"弹回"正常高度 —— 这就是"飞天"的来源。
		// 把原点钉在 BOSS 进入出招那一刻的位置，平面就水平切过它脚下，Z 不会再漂。
		if (ABossCharacterBase* BossActor = GetBoss())
		{
			MoveComp->SetPlaneConstraintOrigin(BossActor->GetActorLocation());
		}

		// 法线朝上 = 只允许在水平面内移动，Z 分量被完全吃掉。
		// 这是"锁 Z"而不是"关重力"：重力照常，只是无法产生垂直位移，
		// 所以踩地、贴地、寻路都不受影响，飞起来这件事则从物理上不可能。
		MoveComp->SetPlaneConstraintNormal(FVector::UpVector);
		MoveComp->SetPlaneConstraintEnabled(true);
	}
	else if (bGroundedApplied)
	{
		MoveComp->SetPlaneConstraintEnabled(bSavedConstrainToPlane);
		MoveComp->SetPlaneConstraintNormal(SavedPlaneConstraintNormal);
		MoveComp->SetPlaneConstraintOrigin(SavedPlaneConstraintOrigin);
		bGroundedApplied = false;
	}
}

// ------------------------------------------------------------------ 判定窗口
void UGA_BossAttackBase::SetupMeleeHitWindow(UAnimMontage* Montage, float PlayRate)
{
	ADemoCharacterBase* Character = Cast<ADemoCharacterBase>(GetAvatarCharacter());
	if (!Montage || !Character || !PendingAttack)
	{
		return;
	}

	const float SafeRate = FMath::Max(PlayRate, 0.05f);
	const float Length = Montage->GetPlayLength() / SafeRate;
	if (Length <= 0.f)
	{
		// 空壳蒙太奇：至少给一个兜底窗口，不然 BOSS 一刀都砍不到人
		UE_LOG(LogDemoCombat, Warning, TEXT("[Boss] 蒙太奇时长为 0，命中窗口退化为 0.05~0.20s"));
	}

	const float EffectiveLength = Length > 0.f ? Length : 0.4f;
	const float StartTime = EffectiveLength * FMath::Clamp(PendingAttack->HitWindowStartRatio, 0.f, 1.f);
	const float EndTime = EffectiveLength * FMath::Clamp(PendingAttack->HitWindowEndRatio, 0.f, 1.f);

	// ⚠⚠⚠ 命中窗口【必须保证一个可用的最小时长】（2026-10-02 第二十轮）
	//
	// 原来这一行是：
	//     WaitDelay(this, FMath::Max(EndTime, StartTime + 0.05f));
	// 那行把"开窗"和"关窗"都排进了【同一个 WaitDelay 任务】，
	// 两个任务的 OnFinish 靠【同一个 StartTime 的相对延迟】竞争 ——
	// 开窗任务在 StartTime 触发，关窗任务在 max(EndTime, StartTime+0.05) 触发。
	// 一旦某招的 EndTime <= StartTime（招式数据没配好、或比例被夹到同一位置），
	// 关窗就会紧跟着开窗在同一帧触发 → 窗口【开 0 帧就关】→ 玩家看到
	// "BOSS 挥了一下但没伤害、也弹不了" —— 正是用户反馈的无意义攻击。
	//
	// 现在：① 先夹出一个非空区间；② 强制保证 MinHitWindow 秒的时长；
	//      ③ 上限不超过蒙太奇总长，避免"窗口开到招式结束之后"。
	const float MinWindow = 0.12f;
	float OpenTime = FMath::Clamp(StartTime, 0.f, EffectiveLength * 0.9f);
	float CloseTime = FMath::Clamp(EndTime, OpenTime + MinWindow, EffectiveLength);
	// 招式太短导致放不下最小窗口时，就把开窗点整体前移（宁可早开一点，也不能没有伤害）
	if (CloseTime - OpenTime < MinWindow)
	{
		OpenTime = FMath::Max(0.f, CloseTime - MinWindow);
	}

	UE_LOG(LogDemoCombat, Log,
		TEXT("[Boss] 命中窗口「%s」: 开=%.2fs 关=%.2fs 时长=%.2fs（蒙太奇总长 %.2fs，比率 %.2f~%.2f）"),
		*PendingAttack->AttackName.ToString(), OpenTime, CloseTime, CloseTime - OpenTime,
		EffectiveLength, PendingAttack->HitWindowStartRatio, PendingAttack->HitWindowEndRatio);

	ActiveHitOnTask = UAbilityTask_WaitDelay::WaitDelay(this, FMath::Max(OpenTime, 0.01f));
	if (ActiveHitOnTask)
	{
		ActiveHitOnTask->OnFinish.AddDynamic(this, &UGA_BossAttackBase::OnHitWindowOpen);
		ActiveHitOnTask->ReadyForActivation();
	}

	ActiveHitOffTask = UAbilityTask_WaitDelay::WaitDelay(this, FMath::Max(CloseTime, OpenTime + 0.02f));
	if (ActiveHitOffTask)
	{
		ActiveHitOffTask->OnFinish.AddDynamic(this, &UGA_BossAttackBase::OnHitWindowClose);
		ActiveHitOffTask->ReadyForActivation();
	}
}

void UGA_BossAttackBase::OnHitWindowOpen()
{
	SetMeleeHitWindow(true);
}

void UGA_BossAttackBase::OnHitWindowClose()
{
	SetMeleeHitWindow(false);
}

void UGA_BossAttackBase::SetMeleeHitWindow(bool bOpen)
{
	if (ADemoCharacterBase* Character = Cast<ADemoCharacterBase>(GetAvatarCharacter()))
	{
		Character->SetMeleeHitWindowEnabled(bOpen);
	}
}

// ------------------------------------------------------------------ 弹幕
int32 UGA_BossAttackBase::FireBarrage(const FBossBarrageParams& Params, int32 CountOverride,
	float SpeedScale, float AngleOffsetDeg)
{
	ABossCharacterBase* Boss = GetBoss();
	if (!Boss || !GetWorld())
	{
		return 0;
	}

	const FVector Forward = Boss->GetActorForwardVector().GetSafeNormal2D();

	// 发射点：优先骨骼插槽（剑尖 / 手掌），没有就"身前 + 上方"兜底
	FVector Origin = FVector::ZeroVector;
	bool bUsedSocket = false;
	if (Params.SpawnSocket != NAME_None)
	{
		if (const USkeletalMeshComponent* MeshComp = Boss->GetMesh())
		{
			if (MeshComp->DoesSocketExist(Params.SpawnSocket))
			{
				Origin = MeshComp->GetSocketLocation(Params.SpawnSocket);
				bUsedSocket = true;
			}
		}
	}
	if (!bUsedSocket)
	{
		Origin = Boss->GetActorLocation()
			+ Forward * Params.SpawnForwardOffset
			+ FVector(0.f, 0.f, Params.SpawnHeightOffset);
	}

	// 基准方向：瞄玩家（可带提前量）或朝正前方
	FVector BaseDir = Forward;
	if (Params.bAimAtTarget)
	{
		if (const AActor* Target = Boss->GetCombatTarget())
		{
			FVector AimLocation = Target->GetActorLocation();
			if (Params.AimLeadScale > 0.f)
			{
				const float Distance = FVector::Dist(Origin, AimLocation);
				const float ProjectileSpeed = FMath::Max(Params.Projectile.Speed * SpeedScale, 1.f);
				const float LeadTime = (Distance / ProjectileSpeed) * Params.AimLeadScale;
				if (const APawn* TargetPawn = Cast<APawn>(Target))
				{
					AimLocation += TargetPawn->GetVelocity() * LeadTime;
				}
			}
			const FVector AimDelta = AimLocation - Origin;
			if (!AimDelta.IsNearlyZero())
			{
				BaseDir = AimDelta.GetSafeNormal();
			}
		}
	}

	// 弹数
	int32 Count = CountOverride >= 0 ? CountOverride : Params.ProjectileCount;
	Count = FMath::Max(Count, 1);

	// 方向集合
	TArray<FVector> Directions;
	Directions.Reserve(Count);

	if (Params.bRingPattern)
	{
		// 360° 均匀一圈（大招 / 落地震荡波）：绕世界 Z 轴铺开，天然水平
		for (int32 i = 0; i < Count; ++i)
		{
			const float Yaw = AngleOffsetDeg + Params.RingAngleOffsetDeg
				+ (360.f * static_cast<float>(i) / static_cast<float>(Count));
			Directions.Add(FRotator(0.f, Yaw, 0.f).RotateVector(FVector::ForwardVector));
		}
	}
	else if (Count == 1)
	{
		Directions.Add(FRotator(0.f, AngleOffsetDeg, 0.f).RotateVector(BaseDir));
	}
	else
	{
		// 扇形：以 BaseDir 为中轴左右均分
		const float Spread = FMath::Abs(Params.SpreadAngleDeg);
		const float Step = Spread / static_cast<float>(Count - 1);
		const float Start = -Spread * 0.5f;
		for (int32 i = 0; i < Count; ++i)
		{
			const float Yaw = Start + Step * static_cast<float>(i) + AngleOffsetDeg;
			Directions.Add(FRotator(0.f, Yaw, 0.f).RotateVector(BaseDir));
		}
	}

	// 单颗弹的参数（二阶段的弹速倍率在这里生效）
	FBossProjectileParams ProjectileParams = Params.Projectile;
	ProjectileParams.Speed = FMath::Max(ProjectileParams.Speed * FMath::Max(SpeedScale, 0.01f), 1.f);

	int32 Spawned = 0;
	for (const FVector& Dir : Directions)
	{
		if (ADemoProjectile::SpawnProjectile(this, Params.ProjectileClass, Boss,
			Origin + Dir * 10.f, Dir, ProjectileParams))
		{
			++Spawned;
		}
	}

	UE_LOG(LogDemoCombat, Log,
		TEXT("[Boss] 弹幕齐射：%s 弹数=%d/%d 张角=%.0f 弹速=%.0f 发射点=%s"),
		Params.bRingPattern ? TEXT("环形") : TEXT("扇形"), Spawned, Count,
		Params.bRingPattern ? 360.f : FMath::Abs(Params.SpreadAngleDeg),
		ProjectileParams.Speed,
		bUsedSocket ? *Params.SpawnSocket.ToString() : TEXT("身前兜底点"));

	return Spawned;
}

// ------------------------------------------------------------------ 突进
//
// 为什么整套位移都走"匀速推固定距离"而不是给初速度：
//   给初速度（LaunchCharacter）时，实际位移 = v²/(2a)，而 a 由
//   BrakingDecelerationWalking + GroundFriction + BrakingFriction 共同决定，
//   在招式数据里看不见、也没法一次调准 —— 同一个 v 在斜坡上和在平地上差很多。
//   ApplyRootMotionConstantForce 则是"以 Strength 厘米/秒推 Duration 秒"，
//   位移恰好 = Strength × Duration，填多少走多少，碰撞安全（撞击会停下），
//   结束时还能把速度夹到 0，收招不会继续滑一段。
void UGA_BossAttackBase::BeginChargeDash(const FBossChargeParams& InCharge, float EffectiveDistance,
	UAnimMontage* DashMontage)
{
	ABossCharacterBase* Boss = GetBoss();
	ACharacter* Avatar = GetAvatarCharacter();
	if (!Boss || !Avatar)
	{
		FinishAttack();
		return;
	}

	ActiveCharge = InCharge;
	bDashActive = true;

	// ---- 1) 目标点：玩家当前位置再"往回退" StopDistance ----
	const FVector SelfLocation = Avatar->GetActorLocation();
	FVector TargetLocation = SelfLocation + Avatar->GetActorForwardVector().GetSafeNormal2D() * EffectiveDistance;

	if (const AActor* Target = Boss->GetCombatTarget())
	{
		const FVector ToTarget = Target->GetActorLocation() - SelfLocation;
		const FVector Dir2D = FVector(ToTarget.X, ToTarget.Y, 0.f).GetSafeNormal();
		if (!Dir2D.IsNearlyZero())
		{
			// 停在目标身前 —— 这个 170cm 左右的距离感是有讲究的：
			// 贴到 0 会让玩家完全被模型糊住（看不清动作、也读不出收招），
			// 留太多又会让收尾斩"砍在空气上"。
			const float Stop = FMath::Max(InCharge.StopDistance, 0.f);
			TargetLocation = FVector(Target->GetActorLocation().X, Target->GetActorLocation().Y, SelfLocation.Z)
				- Dir2D * Stop;
		}
	}

	FVector Delta = TargetLocation - SelfLocation;
	Delta.Z = 0.f;

	float Distance = Delta.Size();
	const float MaxDistance = FMath::Max(InCharge.MaxChargeDistance, 50.f);
	if (Distance > MaxDistance)
	{
		Delta *= MaxDistance / FMath::Max(Distance, KINDA_SMALL_NUMBER);
		Distance = MaxDistance;
	}

	// 已经贴脸了就不冲了：直接进收尾斩（否则会出现"往前顶一小步"的抽搐感）
	if (Distance < 20.f)
	{
		UE_LOG(LogDemoCombat, Log, TEXT("[Boss] 突进距离过短（%.0fcm）→ 直接进收尾斩"), Distance);
		ChargeTargetLocation = SelfLocation;
		OnDashFinished();
		return;
	}

	ChargeTargetLocation = SelfLocation + Delta;

	const FVector DashDir = Delta / Distance;

	// ---- 时长由"速度上限"反推，而不是写死 ----
	// 旧写法 Duration 固定 0.28s → 速度 = 距离 / 0.28，玩家站得越远冲得越像炮弹
	//（实测 961cm 时 3431cm/s），被撞到就是"被打飞 / 被顶着走"。
	// 现在：速度恒定为 ChargeSpeed，距离远只是冲得久一点；
	// 再用 Min/Max 夹住时长，近不瞬移、远不拖沓。
	const float Speed = FMath::Max(InCharge.ChargeSpeed > 0.f
		? InCharge.ChargeSpeed : InCharge.ChargeDistance / FMath::Max(InCharge.ChargeDuration, 0.05f), 50.f);
	const float MinDuration = FMath::Max(InCharge.MinChargeDuration, 0.05f);
	const float MaxDuration = FMath::Max(InCharge.MaxChargeDuration, MinDuration);
	const float Duration = FMath::Clamp(Distance / Speed, MinDuration, MaxDuration);
	const float Strength = Distance / Duration;

	// 朝向先转好，再冲：否则会是"横着平移过去"，非常出戏
	Avatar->SetActorRotation(FRotationMatrix::MakeFromX(DashDir).Rotator());

	// 冻结朝向：冲刺这段 Brain 不再拧身体，否则直线突进会被拧成弧线（收尾斩会砍偏）
	Boss->bDashFacingLocked = true;

	// 蓝图表现钩子：残影 / 破风 / 地面扬尘挂在 BP_Boss 的 OnBossChargeStart 里
	Boss->OnBossChargeStart(DashDir, Distance);

	// ---- 2) 位移任务 ----
	UAbilityTask_ApplyRootMotionConstantForce* Task = UAbilityTask_ApplyRootMotionConstantForce::ApplyRootMotionConstantForce(
		this, TEXT("BossChargeDash"), DashDir, Strength, Duration,
		/*bIsAdditive=*/false, /*StrengthOverTime=*/nullptr,
		// ClampVelocity + 0：结束时速度归零 → 收尾斩不会有残余滑步
		ERootMotionFinishVelocityMode::ClampVelocity, FVector::ZeroVector, /*ClampVelocityOnFinish=*/0.f,
		/*bEnableGravity=*/true);

	if (Task)
	{
		Task->OnFinish.AddDynamic(this, &UGA_BossAttackBase::OnDashFinished);
		Task->ReadyForActivation();
		ActiveRootMotionTask = Task;
	}
	else
	{
		UE_LOG(LogDemoCombat, Warning, TEXT("[Boss] 创建突进任务失败 → 直接进收尾斩"));
		OnDashFinished();
		return;
	}

	// ---- 3) 保险丝：根运动任务万一没回调，也不会把技能吊死 ----
	if (ActiveDashTimeoutTask)
	{
		ActiveDashTimeoutTask->EndTask();
	}
	ActiveDashTimeoutTask = UAbilityTask_WaitDelay::WaitDelay(this, Duration * 2.f + 0.5f);
	if (ActiveDashTimeoutTask)
	{
		ActiveDashTimeoutTask->OnFinish.AddDynamic(this, &UGA_BossAttackBase::OnDashTimeout);
		ActiveDashTimeoutTask->ReadyForActivation();
	}

	// ---- 4) 拖刀：突进全程开着命中判定 ----
	// 位移给代码了，所以突进蒙太奇的根位移必须关掉（RootMotionScale=0），否则会冲双倍距离。
	if (InCharge.bDamageAlongDash)
	{
		SetMeleeHitWindow(true);
	}

	if (DashMontage)
	{
		PlayMontageTask(DashMontage, GetPlayRate(), TEXT("BossChargeDash"), /*RootMotionScale=*/0.f);
	}

	UE_LOG(LogDemoCombat, Log,
		TEXT("[Boss] 突进：距离=%.0fcm 时长=%.2fs 速度=%.0fcm/s 拖刀=%s"),
		Distance, Duration, Strength, InCharge.bDamageAlongDash ? TEXT("是") : TEXT("否"));
}

void UGA_BossAttackBase::OnDashFinished()
{
	if (!bDashActive)
	{
		return;
	}
	bDashActive = false;

	// 位移结束 → 解冻朝向，Brain 的战斗追踪恢复接管（让 BOSS 重新对准玩家准备收尾斩）
	if (ABossCharacterBase* BossNow = GetBoss())
	{
		BossNow->bDashFacingLocked = false;
	}

	if (ActiveDashTimeoutTask)
	{
		ActiveDashTimeoutTask->EndTask();
		ActiveDashTimeoutTask = nullptr;
	}

	// 拖刀窗口一定关掉 —— 收尾斩会重新按自己的蒙太奇比例开一个
	SetMeleeHitWindow(false);

	UE_LOG(LogDemoCombat, Log, TEXT("[Boss] 突进结束 → 结算收尾重斩"));
	FinishChargeImpact();
}

void UGA_BossAttackBase::OnDashTimeout()
{
	if (!bDashActive)
	{
		return;
	}

	UE_LOG(LogDemoCombat, Warning,
		TEXT("[Boss] 突进任务超时未回调，按当前位置补结算（检查角色是否被卡住 / 掉出关卡）"));
	OnDashFinished();
}

void UGA_BossAttackBase::FinishChargeImpact()
{
	ABossCharacterBase* Boss = GetBoss();
	if (!Boss)
	{
		FinishAttack();
		return;
	}

	// 二阶段的伤害/范围倍率与大招倍率同一套口径
	const bool bUpgrade = IsPhase2() && PendingAttack && PendingAttack->bUpgradeInPhase2;
	const float DamageScale = bUpgrade ? PendingAttack->Phase2DamageScale : 1.f;

	const float Radius = PendingAttack ? PendingAttack->GetEffectiveImpactRadius(IsPhase2()) : ActiveCharge.ImpactRadius;
	const float ImpactDamage = (ActiveCharge.ImpactDamage > 0.f ? ActiveCharge.ImpactDamage : GetDamage()) * DamageScale;
	const float ImpactPoise = (ActiveCharge.ImpactPoiseDamage > 0.f ? ActiveCharge.ImpactPoiseDamage : GetPoiseDamage()) * DamageScale;

	// 扇形判定：只打正前方 ImpactHalfAngleDeg 半角以内的人。
	// 为什么不做整圈：突进斩的收尾应该"砍在面前"，一圈打出去会变成无脑 AOE，
	// 玩家也就没有"往侧后方绕"这个正确解法了。
	Boss->ApplyAreaImpact(
		Radius,
		/*HalfAngleDeg=*/ActiveCharge.ImpactHalfAngleDeg,
		/*ImpactDirection=*/Boss->GetActorForwardVector(),
		ImpactDamage, ImpactPoise,
		ActiveCharge.bImpactCanBeParried, ActiveCharge.bImpactCanBeBlocked,
		ActiveCharge.KnockbackStrength, ActiveCharge.KnockbackUpward,
		EDemoDamageSource::HeavyImpact,
		Boss->bDrawDebugCombat);

	// 收尾冲击波（二阶段 / 大招收尾用）：一圈向外扩散的弹幕，把玩家从脚下赶开
	if (ActiveCharge.bSpawnShockwave)
	{
		FBossBarrageParams Shockwave = ActiveCharge.Shockwave;
		Shockwave.bRingPattern = true;
		Shockwave.SpawnSocket = NAME_None;
		Shockwave.SpawnForwardOffset = 0.f;
		Shockwave.SpawnHeightOffset = FMath::Max(Shockwave.SpawnHeightOffset, 40.f);
		Shockwave.bAimAtTarget = false;

		const float SpeedScale = PendingAttack ? PendingAttack->GetEffectiveProjectileSpeedScale(IsPhase2()) : 1.f;
		const int32 Count = (bUpgrade && PendingAttack)
			? Shockwave.ProjectileCount + PendingAttack->Phase2ExtraProjectiles
			: Shockwave.ProjectileCount;

		FireBarrage(Shockwave, Count, SpeedScale, 0.f);
	}

	// 收尾重斩：位移已经走完了，所以这里的蒙太奇根位移缩放设 0，
	// 免得收招时又被动画推着滑一段（那会让刚才"精准停在目标身前"白算）。
	if (ActiveCharge.ImpactMontage)
	{
		if (PlayMontageTask(ActiveCharge.ImpactMontage, GetPlayRate(), TEXT("BossChargeImpact"), /*RootMotionScale=*/0.f))
		{
			// 命中窗口按收尾斩蒙太奇的时长比例开
			SetupMeleeHitWindow(ActiveCharge.ImpactMontage, GetPlayRate());
			return;
		}
	}

	FinishAttack();
}

// ==================================================================================
// 近战挥砍
// ==================================================================================
UGA_BossAttack::UGA_BossAttack()
{
	FGameplayTagContainer DefaultTags;
	DefaultTags.AddTag(DemoTag::Ability_Boss_Attack);
	SetAssetTags(DefaultTags);
}

void UGA_BossAttack::OnAttackStart()
{
	UAnimMontage* Montage = PendingAttack ? PendingAttack->Montage : nullptr;
	if (!Montage)
	{
		UE_LOG(LogDemoCombat, Error, TEXT("[Boss] 近战挥砍缺少蒙太奇（%s.Montage 为空）→ 先跑 Tools/SetupBossMontages.py"),
			*GetNameSafe(PendingAttack));
		FinishAttack();
		return;
	}

	if (bPinMovementWhileSwinging)
	{
		// 前冲由动画根位移提供；此时若还让 AI 走位，两者会互相打架（脚底打滑）
		PinMovement(true);
	}

	// 起手最后再对一次朝向：预警阶段 Brain 已经转过，这里兜底防止玩家绕背
	FaceTarget();

	const float Rate = GetPlayRate();
	if (!PlayMontageTask(Montage, Rate, TEXT("BossSlash"), /*RootMotionScale=*/1.f))
	{
		FinishAttack();
		return;
	}

	// 命中窗口按蒙太奇时长比例开（不依赖蒙太奇通知）
	SetupMeleeHitWindow(Montage, Rate);
}

// ==================================================================================
// 远程弹幕
// ==================================================================================
UGA_BossRangedAttack::UGA_BossRangedAttack()
{
	FGameplayTagContainer DefaultTags;
	DefaultTags.AddTag(DemoTag::Ability_Boss_Attack);
	SetAssetTags(DefaultTags);
}

void UGA_BossRangedAttack::OnAttackStart()
{
	if (!PendingAttack)
	{
		FinishAttack();
		return;
	}

	if (bPinMovementWhileCasting)
	{
		PinMovement(true);
	}
	FaceTarget();

	UAnimMontage* CastMontage = PendingAttack->Montage;
	const float Rate = GetPlayRate();

	if (!CastMontage)
	{
		// 没有施法动作也能放：立刻齐射，然后直接收招 ——
		// 千万别忘了收招，否则技能会一直挂着 State.Attacking，Brain 会卡在"出招中"。
		OnFireWindow();
		FinishAttack();
		return;
	}

	PlayMontageTask(CastMontage, Rate, TEXT("BossCast"), /*RootMotionScale=*/0.f);

	const float CastLength = CastMontage->GetPlayLength() / FMath::Max(Rate, 0.05f);
	const float FireTime = FMath::Max(
		(CastLength > 0.f ? CastLength : 0.5f) * FMath::Clamp(PendingAttack->BarrageFireRatio, 0.f, 1.f), 0.02f);

	ActiveDelayTask = UAbilityTask_WaitDelay::WaitDelay(this, FireTime);
	if (ActiveDelayTask)
	{
		ActiveDelayTask->OnFinish.AddDynamic(this, &UGA_BossRangedAttack::OnFireWindow);
		ActiveDelayTask->ReadyForActivation();
	}
	else
	{
		OnFireWindow();
	}
}

void UGA_BossRangedAttack::OnFireWindow()
{
	if (!PendingAttack)
	{
		return;
	}

	const int32 Count = PendingAttack->GetEffectiveProjectileCount(IsPhase2());
	const float SpeedScale = PendingAttack->GetEffectiveProjectileSpeedScale(IsPhase2());
	FiredCount = FireBarrage(PendingAttack->Barrage, Count, SpeedScale, 0.f);

	if (!bBackstepAfterFire)
	{
		return;
	}

	// 齐射后补一段后撤：弹幕型 BOSS 的"拉开距离"。
	// 用 root motion source 而不是 LaunchCharacter —— 它是匀速定距，距离可控（给初速度会被摩擦力吃掉一截）。
	const float BackstepDelay = FMath::Max(PendingAttack->Barrage.SpawnInterval, 0.12f);
	ActiveDelayTask = UAbilityTask_WaitDelay::WaitDelay(this, BackstepDelay);
	if (ActiveDelayTask)
	{
		ActiveDelayTask->OnFinish.AddDynamic(this, &UGA_BossRangedAttack::OnBackstepWindow);
		ActiveDelayTask->ReadyForActivation();
	}
}

void UGA_BossRangedAttack::OnBackstepWindow()
{
	ABossCharacterBase* Boss = GetBoss();
	if (!Boss || bBackstepped || BackstepDistance <= 0.f)
	{
		return;
	}
	bBackstepped = true;

	const FVector BackDir = -Boss->GetActorForwardVector().GetSafeNormal2D();
	const float Duration = 0.22f;

	UAbilityTask_ApplyRootMotionConstantForce* Task = UAbilityTask_ApplyRootMotionConstantForce::ApplyRootMotionConstantForce(
		this, TEXT("BossBackstep"), BackDir, BackstepDistance / Duration, Duration,
		/*bIsAdditive=*/false, /*StrengthOverTime=*/nullptr,
		ERootMotionFinishVelocityMode::ClampVelocity, FVector::ZeroVector, /*ClampVelocityOnFinish=*/0.f,
		/*bEnableGravity=*/true);

	if (Task)
	{
		ActiveRootMotionTask = Task;
		Task->ReadyForActivation();
	}
}

// ==================================================================================
// 突进斩
// ==================================================================================
UGA_BossChargeSlash::UGA_BossChargeSlash()
{
	FGameplayTagContainer DefaultTags;
	DefaultTags.AddTag(DemoTag::Ability_Boss_Attack);
	SetAssetTags(DefaultTags);
}

void UGA_BossChargeSlash::OnAttackStart()
{
	if (!PendingAttack)
	{
		FinishAttack();
		return;
	}

	// 蓄势阶段钉住：玩家有明确的读招 + 走位时间。
	// 这一段是唯一的"可弹反窗口" —— 真冲起来之后就不该还能被弹了。
	PinMovement(true);
	FaceTarget();

	if (WindupTime > 0.f)
	{
		ActiveDelayTask = UAbilityTask_WaitDelay::WaitDelay(this, WindupTime);
		if (ActiveDelayTask)
		{
			ActiveDelayTask->OnFinish.AddDynamic(this, &UGA_BossChargeSlash::OnWindupFinished);
			ActiveDelayTask->ReadyForActivation();
			return;
		}
	}

	OnWindupFinished();
}

void UGA_BossChargeSlash::OnWindupFinished()
{
	if (!PendingAttack)
	{
		FinishAttack();
		return;
	}

	// 冲之前必须把移速还回去：突进是根运动任务推的，
	// 而 PinMovement 把 MaxWalkSpeed 压成了 0，会抵掉一部分推力（表现是"冲不动"）。
	PinMovement(false);

	// 起手最后再对一次朝向：蓄势这段时间玩家可能已经绕到侧面了
	FaceTarget();

	const FBossChargeParams& Charge = PendingAttack->Charge;
	const float Distance = PendingAttack->GetEffectiveChargeDistance(IsPhase2());

	BeginChargeDash(Charge, Distance, PendingAttack->Montage);
}

void UGA_BossChargeSlash::OnDashFinished()
{
	// 基类会：关掉拖刀窗口 → 结算扇形收尾冲击 → 播 ImpactMontage（或直接结束）
	Super::OnDashFinished();
}

void UGA_BossChargeSlash::OnMontageCompleted()
{
	// 突进期间主蒙太奇播完（突进动作比突进位移短时会发生）→ 交给基类，
	// 基类看到 bDashActive 就会直接返回，不会提前把技能结束掉。
	if (bDashActive)
	{
		Super::OnMontageCompleted();
		return;
	}

	// 走到这里说明是【收尾斩】播完了 → 整招结束，把控制权还给 AI
	bMontageTaskActive = false;
	FinishAttack();
}

// ==================================================================================
// 大招
// ==================================================================================
UGA_BossUltimate::UGA_BossUltimate()
{
	FGameplayTagContainer DefaultTags;
	DefaultTags.AddTag(DemoTag::Ability_Boss_Attack);
	SetAssetTags(DefaultTags);
}

void UGA_BossUltimate::OnAttackStart()
{
	if (!PendingAttack)
	{
		FinishAttack();
		return;
	}

	Step = EUltStep::ChargeIn;
	ChargeElapsed = 0.f;
	RingsFired = 0;
	bUltimateChargeFinisher = false;

	if (bPinMovementWhileCharging)
	{
		PinMovement(true);
	}
	FaceTarget();

	// 蓄力期间打个标记：HUD / 玩家看到就知道"该跑位了"
	if (UAbilitySystemComponent* ASC = GetAvatarASC())
	{
		ASC->AddLooseGameplayTag(DemoTag::State_Boss_Charging, 1);
	}

	UE_LOG(LogDemoCombat, Warning, TEXT("[Boss] * 大招开始蓄力（%.1fs = 玩家的跑位窗口）"), PendingAttack->ChargeDuration);

	StartCharge();
}

void UGA_BossUltimate::EndAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	bool bReplicateEndAbility, bool bWasCancelled)
{
	Step = EUltStep::Done;

	if (UAbilitySystemComponent* ASC = GetAvatarASC())
	{
		ASC->RemoveLooseGameplayTag(DemoTag::State_Boss_Charging, 1);
	}

	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}

void UGA_BossUltimate::StartCharge()
{
	// 蓄力起手：优先用数据资产指定的；留空则退回"主蒙太奇"
	UAnimMontage* ChargeMontage = PendingAttack ? PendingAttack->ChargeMontage : nullptr;
	if (!ChargeMontage)
	{
		ChargeMontage = PendingAttack ? PendingAttack->Montage : nullptr;
	}

	if (ChargeMontage && PlayMontageTask(ChargeMontage, GetPlayRate(), TEXT("BossUltCharge"), 0.f))
	{
		return;
	}

	// 连蓄力动作都没有 → 直接进循环段
	EnterChargeLoop();
}

void UGA_BossUltimate::EnterChargeLoop()
{
	Step = EUltStep::ChargeLoop;

	if (PendingAttack && PendingAttack->ChargeLoopMontage)
	{
		if (PlayMontageTask(PendingAttack->ChargeLoopMontage, GetPlayRate(), TEXT("BossUltLoop"), 0.f))
		{
			return;
		}
	}

	// 没有循环段：蓄力时长就靠起手动作承担，直接开始丢弹幕圈
	EnterRings();
}

void UGA_BossUltimate::EnterRings()
{
	Step = EUltStep::Rings;
	RingsFired = 0;

	// 蓄力结束，把移动还回来
	PinMovement(false);

	const int32 TotalRings = PendingAttack ? FMath::Max(PendingAttack->GetEffectiveRingCount(IsPhase2()), 1) : 1;
	UE_LOG(LogDemoCombat, Warning, TEXT("[Boss] * 大招：开始 %d 圈环形弹幕"), TotalRings);

	RunRingStep();
}

void UGA_BossUltimate::RunRingStep()
{
	if (!PendingAttack)
	{
		EnterChargeOut();
		return;
	}

	const int32 TotalRings = FMath::Max(PendingAttack->GetEffectiveRingCount(IsPhase2()), 1);
	if (RingsFired >= TotalRings)
	{
		EnterChargeOut();
		return;
	}

	const bool bUpgrade = IsPhase2() && PendingAttack->bUpgradeInPhase2;
	const int32 PerRing = FMath::Max(
		PendingAttack->ProjectilesPerRing + (bUpgrade ? PendingAttack->Phase2ExtraProjectiles : 0), 1);

	// 每圈从身上向外铺开（不做枪口定位），并逐圈错开角度 —— 视觉上是"一圈圈炸开"
	FBossBarrageParams Ring = PendingAttack->Barrage;
	Ring.bRingPattern = true;
	Ring.SpawnSocket = NAME_None;
	Ring.SpawnForwardOffset = 0.f;
	Ring.SpawnHeightOffset = RingSpawnHeight;
	Ring.bAimAtTarget = false;
	Ring.ProjectileCount = PerRing;

	const float AngleOffset = PendingAttack->RingAngleStepDeg * static_cast<float>(RingsFired);
	FireBarrage(Ring, PerRing,
		PendingAttack->GetEffectiveProjectileSpeedScale(IsPhase2()), AngleOffset);

	++RingsFired;

	if (RingsFired >= TotalRings)
	{
		EnterChargeOut();
		return;
	}

	ActiveDelayTask = UAbilityTask_WaitDelay::WaitDelay(this, FMath::Max(PendingAttack->RingInterval, 0.05f));
	if (ActiveDelayTask)
	{
		ActiveDelayTask->OnFinish.AddDynamic(this, &UGA_BossUltimate::OnRingIntervalFinished);
		ActiveDelayTask->ReadyForActivation();
	}
	else
	{
		EnterChargeOut();
	}
}

void UGA_BossUltimate::OnRingIntervalFinished()
{
	RunRingStep();
}

void UGA_BossUltimate::EnterChargeOut()
{
	Step = EUltStep::ChargeOut;

	if (PendingAttack && PendingAttack->ChargeOutMontage)
	{
		if (PlayMontageTask(PendingAttack->ChargeOutMontage, GetPlayRate(), TEXT("BossUltOut"), 0.f))
		{
			return;
		}
	}

	EnterChargeFinisher();
}

void UGA_BossUltimate::EnterChargeFinisher()
{
	if (!PendingAttack || !PendingAttack->bEndWithChargeFinisher || !bAllowChargeFinisher)
	{
		FinishUltimate();
		return;
	}

	Step = EUltStep::ChargeFinisher;
	bUltimateChargeFinisher = true;

	UE_LOG(LogDemoCombat, Warning, TEXT("[Boss] * 大招收尾：突进斩"));

	// 收尾突进：收力动作已经在上一段播过了，这里直接冲 + 结算。
	// 距离取招式自己的突进参数（二阶段会自动放大），并且把冲击波打开 ——
	// 大招收尾需要一个"把玩家从远处抓回来"的收束动作，光靠弹幕圈逼不退也拉不回。
	FBossChargeParams ChargeParams = PendingAttack->Charge;
	ChargeParams.bSpawnShockwave = true;

	PinMovement(false);

	const float Distance = PendingAttack->GetEffectiveChargeDistance(IsPhase2());
	BeginChargeDash(ChargeParams, Distance, /*DashMontage=*/nullptr);
}

void UGA_BossUltimate::FinishUltimate()
{
	UE_LOG(LogDemoCombat, Warning, TEXT("[Boss] * 大招结束"));
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
}

void UGA_BossUltimate::OnMontageCompleted()
{
	// 大招的段与段之间靠"上一段播完"来推进（不用额外定时器，天然与动画同步）
	switch (Step)
	{
	case EUltStep::ChargeIn:
	{
		// 把起手动作的时长计进蓄力总时长
		if (PendingAttack && PendingAttack->ChargeMontage)
		{
			ChargeElapsed += PendingAttack->ChargeMontage->GetPlayLength() / FMath::Max(GetPlayRate(), 0.05f);
		}
		EnterChargeLoop();
		return;
	}

	case EUltStep::ChargeLoop:
	{
		// 循环蓄力：反复播循环段，直到走满 ChargeDuration（玩家的跑位窗口）
		if (PendingAttack && PendingAttack->ChargeLoopMontage)
		{
			const float LoopLength = PendingAttack->ChargeLoopMontage->GetPlayLength() / FMath::Max(GetPlayRate(), 0.05f);
			if (LoopLength > 0.02f)
			{
				ChargeElapsed += LoopLength;
			}
			else
			{
				ChargeElapsed = PendingAttack->ChargeDuration;   // 空壳循环段：别死循环
			}
		}

		if (PendingAttack && ChargeElapsed < PendingAttack->ChargeDuration)
		{
			PlayMontageTask(PendingAttack->ChargeLoopMontage, GetPlayRate(), TEXT("BossUltLoop"), 0.f);
			return;
		}

		EnterRings();
		return;
	}

	case EUltStep::ChargeOut:
		// 收力动作播完 → 看要不要突进斩收尾
		EnterChargeFinisher();
		return;

	default:
		break;
	}

	// 其余情况（例如收尾突进的蒙太奇播完）交给基类：
	// 基类会判断"突进还没结束就什么都不做"，不会提前把大招结束掉。
	Super::OnMontageCompleted();
}

void UGA_BossUltimate::OnDashFinished()
{
	if (!bUltimateChargeFinisher)
	{
		Super::OnDashFinished();
		return;
	}

	// 收尾突进结算完 → 大招就结束了。
	// 这里【不播收招动作】：一是收力段已经播过，二是替玩家省下"看完收招"的等待时间，
	// 让大招的结束干净利落（这也是"手感干脆"的一部分）。
	bUltimateChargeFinisher = false;
	bDashActive = false;

	if (ActiveDashTimeoutTask)
	{
		ActiveDashTimeoutTask->EndTask();
		ActiveDashTimeoutTask = nullptr;
	}
	SetMeleeHitWindow(false);

	ABossCharacterBase* Boss = GetBoss();
	if (Boss)
	{
		const bool bUpgrade = IsPhase2() && PendingAttack && PendingAttack->bUpgradeInPhase2;
		const float DamageScale = bUpgrade ? PendingAttack->Phase2DamageScale : 1.f;

		const float Radius = PendingAttack ? PendingAttack->GetEffectiveImpactRadius(IsPhase2()) : ActiveCharge.ImpactRadius;
		const float ImpactDamage = (ActiveCharge.ImpactDamage > 0.f ? ActiveCharge.ImpactDamage : GetDamage()) * DamageScale;
		const float ImpactPoise = (ActiveCharge.ImpactPoiseDamage > 0.f ? ActiveCharge.ImpactPoiseDamage : GetPoiseDamage()) * DamageScale;

		// 大招收尾用【整圈】冲击（半角 180 = 不过滤方向）：
		// 它是"把战场清一遍"的收束技，让玩家没法靠绕背躲掉。
		Boss->ApplyAreaImpact(Radius, /*HalfAngleDeg=*/180.f, Boss->GetActorForwardVector(),
			ImpactDamage, ImpactPoise,
			ActiveCharge.bImpactCanBeParried, ActiveCharge.bImpactCanBeBlocked,
			ActiveCharge.KnockbackStrength, ActiveCharge.KnockbackUpward,
			EDemoDamageSource::Ultimate, Boss->bDrawDebugCombat);

		// 外扩震荡波：大招的最后一下，逼玩家在收尾后立刻重新选位
		FBossBarrageParams Shockwave = ActiveCharge.Shockwave;
		Shockwave.bRingPattern = true;
		Shockwave.SpawnSocket = NAME_None;
		Shockwave.SpawnForwardOffset = 0.f;
		Shockwave.SpawnHeightOffset = FMath::Max(Shockwave.SpawnHeightOffset, 40.f);
		Shockwave.bAimAtTarget = false;

		const float SpeedScale = PendingAttack ? PendingAttack->GetEffectiveProjectileSpeedScale(IsPhase2()) : 1.f;
		const int32 Count = (bUpgrade && PendingAttack)
			? Shockwave.ProjectileCount + PendingAttack->Phase2ExtraProjectiles
			: Shockwave.ProjectileCount;

		FireBarrage(Shockwave, Count, SpeedScale, 0.f);
	}

	FinishUltimate();
}
