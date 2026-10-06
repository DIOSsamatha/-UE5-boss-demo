#include "Player/LockOnComponent.h"
#include "Core/DemoCharacterBase.h"
#include "Core/DemoCommon.h"
#include "Core/DemoGameplayTags.h"

#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"

namespace
{
	// 锁定候选只在"角色周围这么多厘米"里搜。
	// 用球体重叠而不是 GetAllActorsOfClass：这张图以后放了小怪，
	// 全表扫描会随怪物数量线性变慢，而重叠查询有空间索引。
	constexpr float CandidateSearchPadding = 200.f;
}

ULockOnComponent::ULockOnComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
}

// ------------------------------------------------------------------ 目标选取
TArray<AActor*> ULockOnComponent::CollectCandidates() const
{
	TArray<AActor*> Candidates;

	AActor* Owner = GetOwner();
	UWorld* World = GetWorld();
	if (!Owner || !World)
	{
		return Candidates;
	}

	// 三元会产生类型歧义，先收敛成 TSubclassOf<AActor>
	const TSubclassOf<AActor> FilterClass = TargetClassFilter
		? TargetClassFilter
		: TSubclassOf<AActor>(ADemoCharacterBase::StaticClass());

	TArray<FOverlapResult> Overlaps;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(LockOnCandidates), /*bTraceComplex=*/false, Owner);

	const float SearchRadius = FMath::Max(MaxLockDistance, LockBreakDistance) + CandidateSearchPadding;

	World->OverlapMultiByObjectType(
		Overlaps,
		Owner->GetActorLocation(),
		FQuat::Identity,
		FCollisionObjectQueryParams(ECC_Pawn),
		FCollisionShape::MakeSphere(SearchRadius),
		Params);

	const ADemoCharacterBase* OwnerCharacter = Cast<ADemoCharacterBase>(Owner);

	for (const FOverlapResult& Overlap : Overlaps)
	{
		AActor* Actor = Overlap.GetActor();
		if (!Actor || Actor == Owner)
		{
			continue;
		}

		if (!Actor->IsA(FilterClass))
		{
			continue;
		}

		const ADemoCharacterBase* Candidate = Cast<ADemoCharacterBase>(Actor);

		// 死了的、或者根本没血量的（比如场景里的装饰 Pawn）一律不算
		if (!Candidate || Candidate->IsDead())
		{
			continue;
		}

		// 只锁敌对阵营 —— 靠 TeamId，不靠 Tag，免得配错
		if (OwnerCharacter && !OwnerCharacter->IsHostileTo(Actor))
		{
			continue;
		}

		Candidates.AddUnique(Actor);
	}

	return Candidates;
}

AActor* ULockOnComponent::PickBestCandidate(const TArray<AActor*>& Candidates, bool bRelativeToLocked) const
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return nullptr;
	}

	const FVector OwnerLocation = Owner->GetActorLocation();

	// 基准方向：优先用镜头朝向（玩家"看哪儿"就锁哪儿），拿不到再退回角色朝向
	FVector ViewForward = Owner->GetActorForwardVector();
	if (const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
	{
		ViewForward = PC->GetControlRotation().Vector();
	}

	// 切目标时用的是"当前目标方向"而不是镜头方向：镜头正被锁定逻辑压着，用它当基准会转不过来
	FVector RefForward = ViewForward;
	if (bRelativeToLocked && LockedTarget)
	{
		RefForward = (LockedTarget->GetActorLocation() - OwnerLocation).GetSafeNormal();
	}

	const float AngleLimit = bRelativeToLocked ? SwitchTargetMaxAngle : MaxLockAngle;
	const float DistanceLimit = bRelativeToLocked ? LockBreakDistance : MaxLockDistance;

	AActor* Best = nullptr;
	float BestScore = TNumericLimits<float>::Max();

	for (AActor* Candidate : Candidates)
	{
		if (!Candidate || Candidate == LockedTarget)
		{
			continue;
		}

		const FVector ToCandidate = Candidate->GetActorLocation() - OwnerLocation;
		const float Distance = ToCandidate.Size();

		if (Distance > DistanceLimit || Distance < MinLockDistance)
		{
			continue;
		}

		const float Angle = FMath::RadiansToDegrees(
			FMath::Acos(FMath::Clamp(FVector::DotProduct(RefForward, ToCandidate.GetSafeNormal()), -1.f, 1.f)));

		if (Angle > AngleLimit)
		{
			continue;
		}

		// 距离为主、角度为辅：近的优先，但正对着的比"同样近但偏在角落"的更优先。
		// 角度权重要跟着距离量级走（都是几百厘米），8 这个系数是手感值，不用改。
		const float Score = Distance + Angle * 8.f;
		if (Score < BestScore)
		{
			BestScore = Score;
			Best = Candidate;
		}
	}

	return Best;
}

AActor* ULockOnComponent::FindBestTarget() const
{
	return PickBestCandidate(CollectCandidates(), /*bRelativeToLocked=*/false);
}

// ------------------------------------------------------------------ 状态切换
void ULockOnComponent::SetLockedTarget(AActor* NewTarget)
{
	if (LockedTarget == NewTarget)
	{
		return;
	}

	LockedTarget = NewTarget;
	LockedTime = 0.f;

	if (LockedTarget)
	{
		EnterExternalRotationMode();
	}
	else
	{
		ExitExternalRotationMode();
	}

	OnLockTargetChanged.Broadcast(NewTarget);
}

bool ULockOnComponent::EnableLockOn()
{
	AActor* Best = FindBestTarget();
	if (!Best)
	{
		UE_LOG(LogDemoCombat, Log, TEXT("[LockOn] 视野内没有可锁定的敌人（距离<=%.0f 角度<=%.0f）"),
			MaxLockDistance, MaxLockAngle);
		return false;
	}

	SetLockedTarget(Best);
	return true;
}

void ULockOnComponent::DisableLockOn()
{
	SetLockedTarget(nullptr);
}

void ULockOnComponent::ToggleLockOn()
{
	if (IsLockedOn())
	{
		DisableLockOn();
	}
	else
	{
		EnableLockOn();
	}
}

void ULockOnComponent::SwitchTarget(bool bToRight)
{
	if (!IsLockedOn())
	{
		EnableLockOn();
		return;
	}

	TArray<AActor*> Candidates = CollectCandidates();
	if (Candidates.Num() <= 1)
	{
		return;
	}

	AActor* Owner = GetOwner();
	const FVector OwnerLocation = Owner ? Owner->GetActorLocation() : FVector::ZeroVector;
	const FVector CurrentToTarget = (LockedTarget->GetActorLocation() - OwnerLocation).GetSafeNormal2D();

	AActor* NextTarget = nullptr;
	float BestAngleDiff = TNumericLimits<float>::Max();

	for (AActor* Candidate : Candidates)
	{
		if (!Candidate || Candidate == LockedTarget)
		{
			continue;
		}

		const FVector ToCandidate = (Candidate->GetActorLocation() - OwnerLocation).GetSafeNormal2D();

		// 带符号夹角：Z 分量的符号就是左右
		const float SignedAngle = FMath::RadiansToDegrees(
			FMath::Atan2(FVector::CrossProduct(CurrentToTarget, ToCandidate).Z,
				FVector::DotProduct(CurrentToTarget, ToCandidate)));

		// 逆时针为正；要"往右换"就挑负角里最接近的
		if (bToRight ? SignedAngle >= 0.f : SignedAngle <= 0.f)
		{
			continue;
		}

		const float AbsAngle = FMath::Abs(SignedAngle);
		if (AbsAngle < BestAngleDiff)
		{
			BestAngleDiff = AbsAngle;
			NextTarget = Candidate;
		}
	}

	if (NextTarget)
	{
		SetLockedTarget(NextTarget);
	}
}

// ------------------------------------------------------------------ 朝向模式
void ULockOnComponent::EnterExternalRotationMode()
{
	if (!bDriveOwnerRotation || bRotationModeOverridden)
	{
		return;
	}

	ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	UCharacterMovementComponent* MoveComp = OwnerCharacter ? OwnerCharacter->GetCharacterMovement() : nullptr;
	if (!MoveComp)
	{
		return;
	}

	// 先存下来再改。⚠ 一定要"存下来"：
	// 本项目玩家用的是横移(strafe)模式（bOrientRotationToMovement=false），
	// 早期版本解锁时直接把它写回 true，等于把 strafe 关掉了 —— 表现就是
	// "解除锁定后按 A/D 角色会转着圈跑"。存/还原就没这个问题。
	bSavedOrientRotationToMovement = MoveComp->bOrientRotationToMovement;
	bSavedUseControllerDesiredRotation = MoveComp->bUseControllerDesiredRotation;
	bRotationModeOverridden = true;

	// 两个都关掉 = 移动组件完全不管朝向，交给下面的 Tick 自己插值。
	// 为什么不直接留着 bUseControllerDesiredRotation：锁定期间玩家用鼠标微调镜头时，
	// 控制朝向会跟着偏移，角色就会跟着一起扭 —— 而我们要的是"镜头可以晃、身体钉住目标"。
	MoveComp->bOrientRotationToMovement = false;
	MoveComp->bUseControllerDesiredRotation = false;
}

void ULockOnComponent::ExitExternalRotationMode()
{
	if (!bRotationModeOverridden)
	{
		return;
	}
	bRotationModeOverridden = false;

	ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	UCharacterMovementComponent* MoveComp = OwnerCharacter ? OwnerCharacter->GetCharacterMovement() : nullptr;
	if (!MoveComp)
	{
		return;
	}

	MoveComp->bOrientRotationToMovement = bSavedOrientRotationToMovement;
	MoveComp->bUseControllerDesiredRotation = bSavedUseControllerDesiredRotation;
}

// ------------------------------------------------------------------ Tick
FVector ULockOnComponent::GetFocusLocation() const
{
	if (!LockedTarget)
	{
		return FVector::ZeroVector;
	}

	// ---- 焦点只用【简单碰撞体】= 角色的胶囊，不用 Actor 包围盒 ----
	//
	// 为什么必须这么改：包围盒会把武器 / 披风 / 挂件一起算进去，
	// 而大型 BOSS 的武器经常高过头顶 —— 焦点于是被抬到领口以上，
	// 屏幕上看起来就是"准星锁在头顶血条上"，而不是锁在人身上。
	// 胶囊体才是"身体"，从它算出来的焦点才会落在躯干上。
	FVector Origin = LockedTarget->GetActorLocation();
	float HalfHeight = 0.f;
	float Radius = 0.f;
	LockedTarget->GetSimpleCollisionCylinder(HalfHeight, Radius);

	if (HalfHeight <= KINDA_SMALL_NUMBER)
	{
		// 不是胶囊类目标（箱子 / 纯 Actor）：退回包围盒，
		// 但只用它的下半部分，同样是为了别瞄到顶上去
		FVector BoxExtent = FVector::ZeroVector;
		LockedTarget->GetActorBounds(/*bOnlyCollidingComponents=*/true, Origin, BoxExtent);
		HalfHeight = BoxExtent.Z * 0.5f;
	}

	// 上抬量 = 胶囊半高 × 比例 + 绝对偏移。
	// 默认 0.15：BOSS 胶囊半高 130 → 中心(130) + 19.5 ≈ 150cm，落点上胸，
	// 和它 295cm 处的头顶血条拉开了足够距离。
	const float Lift = HalfHeight * FocusHeightRatio + FocusHeightOffset;

	return FVector(Origin.X, Origin.Y, Origin.Z + Lift);
}

void ULockOnComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	AActor* Owner = GetOwner();
	if (!Owner || !LockedTarget)
	{
		return;
	}

	LockedTime += DeltaTime;

	// ---- 失效判定 ----
	const ADemoCharacterBase* TargetCharacter = Cast<ADemoCharacterBase>(LockedTarget);
	const float Distance = FVector::Dist(Owner->GetActorLocation(), LockedTarget->GetActorLocation());

	const bool bTargetGone = !IsValid(LockedTarget) || (TargetCharacter && TargetCharacter->IsDead());
	const bool bTooFar = Distance > LockBreakDistance;

	if (bTargetGone || bTooFar)
	{
		UE_LOG(LogDemoCombat, Log, TEXT("[LockOn] 自动解除锁定：%s（距离=%.0f）"),
			bTargetGone ? TEXT("目标已死亡/失效") : TEXT("超出脱锁距离"), Distance);
		DisableLockOn();
		return;
	}

	if (bBreakOnLineOfSightBlocked)
	{
		FHitResult Hit;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(LockOnLOS), /*bTraceComplex=*/false, Owner);
		Params.AddIgnoredActor(LockedTarget);

		const bool bBlocked = GetWorld()->LineTraceSingleByChannel(
			Hit, Owner->GetActorLocation(), GetFocusLocation(), ECC_Visibility, Params);

		if (bBlocked)
		{
			DisableLockOn();
			return;
		}
	}

	// ---- 身体朝向目标 ----
	if (!bDriveOwnerRotation || !bRotationModeOverridden)
	{
		return;
	}

	ACharacter* OwnerCharacter = Cast<ACharacter>(Owner);
	if (!OwnerCharacter)
	{
		return;
	}

	// ⚠ 出招期间【弱跟随】BOSS，而不是完全不管，也不完全锁定（2026-10-02）
	//
	// 原实现：出招时直接 return，完全不拧身体 → 连续攻击时朝向只由招式自己决定，
	//   玩家一边绕圈一边连打，身体只会越来越歪 → 后续攻击全部砍到空气。
	//
	// 为什么不能简单地"出招时也全力转向"：招式自带根运动（位移+朝向），
	//   全力每帧拧会和根运动互相打架 —— 根位移把胶囊推开一点 → 角度变了 → 再拧回来 →
	//   弹簧臂挂在这个会甩动的身体上 → 镜头被甩得疯狂乱摇。
	//
	// 折中做法（动作游戏通行做法）：出招期间只做【很弱】的纠偏 ——
	//   ① 转向速度只有常态的 1/AttackTurnSpeedScale，不会和根运动抢方向盘；
	//   ② 偏差小于 AttackTurnAngleThreshold 时【完全不动】（不制造任何抖动）；
	//   ③ 只有偏差大到"攻击明显打空"了才慢慢拉回来。
	// 这样既能保证连打不飘，又不会让镜头乱摇。
	const ADemoCharacterBase* OwnerChar = Cast<ADemoCharacterBase>(Owner);
	UAbilitySystemComponent* OwnerASC = OwnerChar ? OwnerChar->GetAbilitySystemComponent() : nullptr;
	const bool bOwnerAttacking = OwnerASC && OwnerASC->HasMatchingGameplayTag(DemoTag::State_Attacking);

	const FVector ToTarget = (LockedTarget->GetActorLocation() - Owner->GetActorLocation()).GetSafeNormal2D();
	if (ToTarget.IsNearlyZero())
	{
		return;
	}

	const FRotator TargetRot(0.f, FRotationMatrix::MakeFromX(ToTarget).Rotator().Yaw, 0.f);

	// ---- Turn In Place 死区 ----
	// 以前这里【无条件】每帧把角色拧向目标：夹角再小也拧，于是 BOSS 或玩家稍有位移，
	// 角色就持续微调 —— 视觉上就是身体在原地"抖"，横移时尤其明显。
	// 现在夹角小于阈值就完全不动：横移时身体保持当前朝向（这本来就更自然），
	// 只有偏差大到看得出来（>35°）才平滑转过去，观感即"原地转身"。
	const float AngleToTarget = FMath::Abs(
		FRotator::NormalizeAxis(TargetRot.Yaw - Owner->GetActorRotation().Yaw));

	if (bOwnerAttacking)
	{
		// 出招中：偏差小 → 绝对不碰（防抖/防镜头乱摇）；偏差大到打空了 → 弱跟随拉回来
		if (AngleToTarget < AttackTurnAngleThreshold)
		{
			return;
		}
		const float AttackRate = TurnSpeed / FMath::Max(AttackTurnSpeedScale, 1.f);
		const FRotator NewRot = FMath::RInterpTo(Owner->GetActorRotation(), TargetRot, DeltaTime, AttackRate);
		OwnerCharacter->SetActorRotation(FRotator(0.f, NewRot.Yaw, 0.f));
		return;
	}

	if (AngleToTarget < TurnInPlaceAngleThreshold)
	{
		return;
	}

	const FRotator NewRot = FMath::RInterpTo(Owner->GetActorRotation(), TargetRot, DeltaTime, TurnSpeed);
	OwnerCharacter->SetActorRotation(FRotator(0.f, NewRot.Yaw, 0.f));
}

void ULockOnComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 组件先于角色销毁时也得把朝向模式还回去，否则热重载 / 重生成会把角色留在"外部驱动"状态
	ExitExternalRotationMode();
	LockedTarget = nullptr;

	Super::EndPlay(EndPlayReason);
}
