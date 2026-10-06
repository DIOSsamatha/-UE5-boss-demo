#include "Core/DemoAbilitySystemLibrary.h"
#include "Core/DemoAttributeSet.h"
#include "Core/DemoCharacterBase.h"
#include "Core/DemoCombatTuning.h"
#include "Core/DemoGameplayTags.h"
#include "Core/DemoEffectContext.h"
#include "Effects/DemoGameplayEffects.h"
#include "Boss/BossCharacterBase.h"
#include "Combat/DemoHitStopSubsystem.h"
#include "Combat/DemoImpactBurst.h"

#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

// ----------------------------------------------------------------------------------
// 调试开关（控制台里改，不用重编译）
//   demo.FriendlyFire 1  → 同阵营也能互相打（测受击表现很好用）
//   demo.DamageLog 1     → 每次命中结算都打一行日志（排查"为什么这一刀没伤害"）
// ----------------------------------------------------------------------------------
static TAutoConsoleVariable<int32> CVarDemoFriendlyFire(
	TEXT("demo.FriendlyFire"),
	0,
	TEXT("是否允许友军伤害（同阵营互相造成伤害）。0 = 关闭（默认），1 = 开启。"),
	ECVF_Cheat);

static TAutoConsoleVariable<int32> CVarDemoDamageLog(
	TEXT("demo.DamageLog"),
	0,
	TEXT("是否在日志里打印每次命中结算明细。0 = 关闭（默认），1 = 开启。"),
	ECVF_Cheat);

UAbilitySystemComponent* UDemoAbilitySystemLibrary::GetAbilitySystemComponent(const AActor* Actor)
{
	if (!Actor)
	{
		return nullptr;
	}

	if (const IAbilitySystemInterface* ASI = Cast<IAbilitySystemInterface>(Actor))
	{
		return ASI->GetAbilitySystemComponent();
	}
	return nullptr;
}

UDemoAttributeSet* UDemoAbilitySystemLibrary::GetAttributeSet(const AActor* Actor)
{
	UAbilitySystemComponent* ASC = GetAbilitySystemComponent(Actor);
	if (!ASC)
	{
		return nullptr;
	}
	return const_cast<UDemoAttributeSet*>(ASC->GetSet<UDemoAttributeSet>());
}

bool UDemoAbilitySystemLibrary::HasTag(const AActor* Actor, FGameplayTag Tag)
{
	UAbilitySystemComponent* ASC = GetAbilitySystemComponent(Actor);
	return ASC && ASC->HasMatchingGameplayTag(Tag);
}

// ==================================================================================
// 敌我关系
// ==================================================================================
bool UDemoAbilitySystemLibrary::IsHostile(const AActor* A, const AActor* B)
{
	if (!A || !B)
	{
		return false;
	}

	const ADemoCharacterBase* CharA = Cast<ADemoCharacterBase>(A);
	const ADemoCharacterBase* CharB = Cast<ADemoCharacterBase>(B);
	if (!CharA || !CharB)
	{
		// 有一方不是战斗单位 → 谈不上敌对
		return false;
	}

	// 中立（TeamId = 2）对任何人都算敌对：谁都能打它，它也能打任何人。
	// 这是 IsHostileTo 的天然行为（只要 TeamId 不同就是敌对），这里不再额外分支。
	return CharA->IsHostileTo(CharB);
}

bool UDemoAbilitySystemLibrary::IsFriendlyFireAllowed()
{
	return CVarDemoFriendlyFire.GetValueOnAnyThread() != 0;
}

void UDemoAbilitySystemLibrary::SetAllowFriendlyFire(bool bAllow)
{
	CVarDemoFriendlyFire->Set(bAllow ? 1 : 0, ECVF_SetByGameSetting);
}

bool UDemoAbilitySystemLibrary::CanDamage(const AActor* SourceActor, const AActor* TargetActor)
{
	if (!SourceActor || !TargetActor || SourceActor == TargetActor)
	{
		return false;
	}

	// 目标必须是"有 ASC 的战斗单位"——中立箱子之类的东西不该吃伤害结算
	if (!GetAbilitySystemComponent(TargetActor))
	{
		return false;
	}

	// 尸体不结算
	if (HasTag(TargetActor, DemoTag::State_Dead))
	{
		return false;
	}

	if (IsHostile(SourceActor, TargetActor))
	{
		return true;
	}

	// 同阵营：只有显式打开友伤才允许
	return IsFriendlyFireAllowed();
}

AActor* UDemoAbilitySystemLibrary::FindNearestHostile(AActor* SourceActor, float Radius, bool bRequireLineOfSight)
{
	if (!SourceActor || Radius <= 0.f)
	{
		return nullptr;
	}

	UWorld* World = SourceActor->GetWorld();
	if (!World)
	{
		return nullptr;
	}

	const FVector Origin = SourceActor->GetActorLocation();

	TArray<FOverlapResult> Overlaps;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(DemoFindNearestHostile), false, SourceActor);
	const FCollisionShape Shape = FCollisionShape::MakeSphere(Radius);

	if (!World->OverlapMultiByObjectType(Overlaps, Origin, FQuat::Identity,
		FCollisionObjectQueryParams(ECC_Pawn), Shape, Params))
	{
		return nullptr;
	}

	AActor* Best = nullptr;
	float BestDistSq = TNumericLimits<float>::Max();

	for (const FOverlapResult& Overlap : Overlaps)
	{
		AActor* Candidate = Overlap.GetActor();
		if (!Candidate || !CanDamage(SourceActor, Candidate))
		{
			continue;
		}

		if (bRequireLineOfSight)
		{
			FHitResult Blocking;
			const bool bBlocked = World->LineTraceSingleByChannel(Blocking, Origin, Candidate->GetActorLocation(),
				ECC_Visibility, Params);
			if (bBlocked && Blocking.GetActor() != Candidate)
			{
				continue;
			}
		}

		const float DistSq = FVector::DistSquared(Origin, Candidate->GetActorLocation());
		if (DistSq < BestDistSq)
		{
			BestDistSq = DistSq;
			Best = Candidate;
		}
	}

	return Best;
}

void UDemoAbilitySystemLibrary::FindHostilesInRadius(const AActor* SourceActor, const FVector& Origin, float Radius,
	TArray<AActor*>& OutActors, bool bIncludeDead)
{
	OutActors.Reset();

	if (Radius <= 0.f)
	{
		return;
	}

	UWorld* World = SourceActor ? SourceActor->GetWorld() : nullptr;
	if (!World)
	{
		return;
	}

	TArray<FOverlapResult> Overlaps;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(DemoFindHostilesInRadius), false, SourceActor);
	const FCollisionShape Shape = FCollisionShape::MakeSphere(Radius);

	if (!World->OverlapMultiByObjectType(Overlaps, Origin, FQuat::Identity,
		FCollisionObjectQueryParams(ECC_Pawn), Shape, Params))
	{
		return;
	}

	for (const FOverlapResult& Overlap : Overlaps)
	{
		AActor* Candidate = Overlap.GetActor();
		if (!Candidate || Candidate == SourceActor)
		{
			continue;
		}
		if (!IsHostile(SourceActor, Candidate))
		{
			continue;
		}
		if (!bIncludeDead && HasTag(Candidate, DemoTag::State_Dead))
		{
			continue;
		}

		OutActors.AddUnique(Candidate);
	}
}

EDemoHitDirection UDemoAbilitySystemLibrary::GetHitDirection(const AActor* TargetActor, const FVector& HitFromLocation)
{
	if (!TargetActor)
	{
		return EDemoHitDirection::Front;
	}

	const FVector Forward = TargetActor->GetActorForwardVector().GetSafeNormal2D();
	const FVector ToAttacker = (HitFromLocation - TargetActor->GetActorLocation()).GetSafeNormal2D();

	if (ToAttacker.IsNearlyZero())
	{
		return EDemoHitDirection::Front;
	}

	const float Dot = FVector::DotProduct(Forward, ToAttacker);
	const float AngleDeg = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Dot, -1.f, 1.f)));

	if (AngleDeg <= 45.f)
	{
		return EDemoHitDirection::Front;
	}
	if (AngleDeg >= 135.f)
	{
		return EDemoHitDirection::Back;
	}

	const float CrossZ = FVector::CrossProduct(Forward, ToAttacker).Z;
	return CrossZ > 0.f ? EDemoHitDirection::Right : EDemoHitDirection::Left;
}

EDemoHitResultType UDemoAbilitySystemLibrary::ApplyHit(AActor* SourceActor, AActor* TargetActor, float BaseDamage,
	float PoiseDamage, bool bCanBeParried, bool bCanBeBlocked, const FVector& HitFromLocation, bool bTriggerHitStop,
	EDemoDamageSource DamageSource)
{
	if (!SourceActor || !TargetActor)
	{
		return EDemoHitResultType::Dodged;
	}

	// 自己打自己：直接忽略（挥空 / 弹幕刚出生时偶尔会撞到自己）
	if (SourceActor == TargetActor)
	{
		return EDemoHitResultType::Dodged;
	}

	UAbilitySystemComponent* SourceASC = GetAbilitySystemComponent(SourceActor);
	UAbilitySystemComponent* TargetASC = GetAbilitySystemComponent(TargetActor);

	if (!SourceASC || !TargetASC)
	{
		return EDemoHitResultType::Dodged;
	}

	// ---------- 0) 敌我过滤（放在最前面）----------
	// 必须早于无敌帧/弹反判定：否则"打友军"也会触发完美闪避的子弹时间，表现很怪。
	if (!CanDamage(SourceActor, TargetActor))
	{
		if (CVarDemoDamageLog.GetValueOnAnyThread() != 0)
		{
			UE_LOG(LogDemoCombat, Log, TEXT("[Hit] 跳过 %s → %s：非敌对（同阵营且未开 demo.FriendlyFire）"),
				*SourceActor->GetName(), *TargetActor->GetName());
		}
		return EDemoHitResultType::Dodged;
	}

	// 目标已死亡，不再结算
	if (TargetASC->HasMatchingGameplayTag(DemoTag::State_Dead))
	{
		return EDemoHitResultType::Dodged;
	}

	const EDemoHitDirection HitDirection = GetHitDirection(TargetActor, HitFromLocation);

	// ================= 1) 闪避：无敌帧 =================
	//
	// 两档判定（"绝区零手感"的关键）：
	//   完美闪避 = 无敌帧 + 还在开场的 0.22s 判定窗里 → 子弹时间 + 白闪 + 清冷却 + 回耐力
	//   普通闪避 = 只吃到无敌帧（闪避后半段才被打到）  → 只免伤，给一条轻提示
	//
	// 为什么要分两档：只有"卡着出招那一瞬间按下 Shift"才有慢动作奖励，
	// 玩家才会去读招而不是无脑连按闪避。
	if (TargetASC->HasMatchingGameplayTag(DemoTag::State_Invincible))
	{
		const bool bPerfectDodge = TargetASC->HasMatchingGameplayTag(DemoTag::State_PerfectDodgeWindow);

		FGameplayEventData Payload;
		Payload.Instigator = SourceActor;
		Payload.Target = TargetActor;

		if (bPerfectDodge)
		{
			// 子弹时间：玩家的正确操作直接把世界放慢 ——
			// 既是纯感官奖励，也真的给了"看清敌人后续连招"的反应窗口。
			if (UDemoHitStopSubsystem* HitStop = SourceActor->GetWorld()->GetSubsystem<UDemoHitStopSubsystem>())
			{
				HitStop->RequestSlowMotion(
					DemoBalance::PerfectDodgeSlowMoTime,
					DemoBalance::PerfectDodgeSlowMoScale);
			}

			// 回耐力：奖励"用对操作"而不是"挨打"。
			// 用完美闪避专用的那条（+20），比完美弹反的 +25 少 —— 两者风险不同，回报要拉开档次。
			ApplyEffectToSelf(TargetActor, UGE_PerfectDodgeRewardEffect::StaticClass());

			// 回架势：完美闪避是架势条最主要的"还债"手段。
			// 没有这条返还，架势只出不进 → 玩家挨几下就永远挡不动了，
			// 反馈链会把玩家推向"反正也挡不住，不如一直躲"的单一解。
			ApplyPostureChange(TargetActor, TargetActor, DemoBalance::PerfectDodgePostureRefund);

			// 派生窗：挂上 State_DodgePerfect，让"窗内按攻击"能取消闪避后摇直接接招（"闪A"）。
			// 只有完美闪避才给 —— 普通闪避没有，避免无脑连按闪避无限派生。
			ApplyEffectToSelf(TargetActor, UGE_DodgePerfectFollowupEffect::StaticClass());

			UAbilitySystemBlueprintLibrary::SendGameplayEventToActor(
				TargetActor, DemoTag::Event_Dodge_Perfect, Payload);

			if (CVarDemoDamageLog.GetValueOnAnyThread() != 0)
			{
				UE_LOG(LogDemoCombat, Log,
					TEXT("[Hit] %s → %s：★完美闪避★（子弹时间 %.2fs / 时缓 %.2f）"),
					*SourceActor->GetName(), *TargetActor->GetName(),
					DemoBalance::PerfectDodgeSlowMoTime, DemoBalance::PerfectDodgeSlowMoScale);
			}
		}
		else
		{
			UAbilitySystemBlueprintLibrary::SendGameplayEventToActor(
				TargetActor, DemoTag::Event_Dodge_Normal, Payload);

			if (CVarDemoDamageLog.GetValueOnAnyThread() != 0)
			{
				UE_LOG(LogDemoCombat, Log, TEXT("[Hit] %s → %s：无敌帧内闪避（非完美窗）"),
					*SourceActor->GetName(), *TargetActor->GetName());
			}
		}

		return EDemoHitResultType::Dodged;
	}

	// ================= 2) 弹反 =================
	//   完美弹反 = 判定窗前 0.12s 内弹到 → 强顿帧 + 慢动作 + 攻击方 1.3s 大硬直 + 额外削韧 + 回耐力
	//   普通弹反 = 窗口后半段弹到       → 顿帧 + 攻击方 0.6s 硬直
	if (bCanBeParried && TargetASC->HasMatchingGameplayTag(DemoTag::State_Parrying))
	{
		const bool bPerfectParry = TargetASC->HasMatchingGameplayTag(DemoTag::State_PerfectParryWindow);

		FGameplayEventData Payload;
		Payload.Instigator = SourceActor;
		Payload.Target = TargetActor;
		Payload.EventMagnitude = BaseDamage;

		// 事件：完美弹反会【同时】发 Perfect 和 Success 两个。
		//   为什么：UGA_PlayerParry 只监听 Success（= "弹到了"，用来切成反击动作），
		//   不想为一个纯表现层的档位去改技能的事件订阅；
		//   而 HUD / 音效 / 特效监听 Perfect，做更夸张的强化表现。
		//   两个事件语义独立：Success 管"有没有弹到"，Perfect 管"弹得准不准"。
		UAbilitySystemBlueprintLibrary::SendGameplayEventToActor(TargetActor, DemoTag::Event_Parry_Success, Payload);
		UAbilitySystemBlueprintLibrary::SendGameplayEventToActor(SourceActor, DemoTag::Event_Parry_Success, Payload);

		if (bPerfectParry)
		{
			UAbilitySystemBlueprintLibrary::SendGameplayEventToActor(TargetActor, DemoTag::Event_Parry_Perfect, Payload);
			UAbilitySystemBlueprintLibrary::SendGameplayEventToActor(SourceActor, DemoTag::Event_Parry_Perfect, Payload);
		}

		// 弹反者获得短暂无敌，方便接反击
		ApplyEffectToSelf(TargetActor, UGE_InvincibleEffect::StaticClass());

		// 被弹反者（BOSS）当前招式是否可被弹反【打断动画】：
		// 重型招式（突进斩/大招）bInterruptibleByParry=false → 弹反只免伤+奖励，不打断、不冻结，
		// 玩家必须改走闪避；轻招（近战/弹幕）才会被弹反打断。这就是"完美格挡只打断部分攻击"。
		const bool bAttackInterruptible = [&]() -> bool
		{
			if (ABossCharacterBase* Boss = Cast<ABossCharacterBase>(SourceActor))
			{
				return Boss->IsCurrentAttackInterruptible();
			}
			return true;  // 非 BOSS 攻击者（理论只有 BOSS）回退为"可打断"，保持原行为
		}();

		// 被弹反者吃硬直 / 奖励
		if (bPerfectParry)
		{
			if (bAttackInterruptible)
			{
				// 打断动画 + 长硬直：完美弹反的硬直 GE 已不再自动取消 BOSS 出招，
				// 这里按"可打断"显式取消当前招式，落实"只打断部分攻击"。
				ApplyPerfectParryStagger(SourceActor);
				if (UAbilitySystemComponent* BossASC = GetAbilitySystemComponent(SourceActor))
				{
					FGameplayTagContainer BossAttackTags;
					BossAttackTags.AddTag(DemoTag::Ability_Boss_Attack);
					BossASC->CancelAbilities(&BossAttackTags);
				}
			}
			// 不可打断：完美弹反仍免伤 + 奖励，但不冻结、不打断重型招式（玩家必须改走闪避）

			// 额外削韧：让"主动去弹"成为更快把 BOSS 打进破防的手段
			ApplyPoiseDamageOnly(TargetActor, SourceActor, DemoBalance::PerfectParryBonusPoiseDamage);
			ApplyEffectToSelf(TargetActor, UGE_PerfectRewardEffect::StaticClass());

			// 稳心：完美弹反同样回架势（挡回去的那一刻心是稳的）。
			// 量给得比完美闪避少（18 vs 25）：闪避窗口 0.22s、弹反窗口 0.22s 相近，
			// 但弹反已经拿到了"额外削韧 + 攻击方硬直 + 推进元素槽"三重产出，
			// 架势返还这一项必须让位，否则弹反会全面优于闪避。
			ApplyPostureChange(TargetActor, TargetActor, DemoBalance::PerfectParryPostureRefund);
		}
		else
		{
			// 普通弹反：同样按"可打断"门控，与完美弹反保持一致（重型招式不被任何弹反打断）。
			// 可打断时基类硬直 GE 仍会自动取消 BOSS 招式；不可打断则只免伤、不硬直、不打断。
			if (bAttackInterruptible)
			{
				ApplyStagger(SourceActor, false);
			}
			else
			{
				// 误弹重招：拿【普通】弹反去接不可打断的重型招式（突进斩 / 大招）→ 要付代价。
				//
				// 为什么必须罚：完美弹反重型招式已经是"免伤 + 奖励但不打断"（玩家不亏），
				// 如果普通弹反同样白嫖，玩家就没有任何理由去分辨招式轻重，
				// "读招"这个核心玩法会直接退化成"见招就弹"。
				// 硬直 0.4s + 扣体力，让"这招弹不得、得闪"变成身体记忆。
				ApplyEffectToSelf(TargetActor, UGE_MisParryStunEffect::StaticClass());
				ApplyStaminaCost(TargetActor, DemoBalance::MisParryHeavyStaminaCost);
			}
		}

		// ---- 弹反 → 元素槽（青条）----
		// 反馈链：弹反不再只是"免伤"，它同时是推进异常条最快的单次手段。
		// 完美弹反给得更多（30 vs 22），让"敢在窗口前段弹"这件事有额外产出，
		// 于是"读招"和"输出"被强制耦合 —— 只砍不弹推不动第二条槽。
		ApplyAnomalyGain(TargetActor, SourceActor, /*ImpactGain=*/0.f,
			bPerfectParry ? DemoBalance::PerfectParryAnomalyElement : DemoBalance::ParryAnomalyElement);

		if (bTriggerHitStop)
		{
			if (UDemoHitStopSubsystem* HitStop = SourceActor->GetWorld()->GetSubsystem<UDemoHitStopSubsystem>())
			{
				if (bPerfectParry)
				{
					// 两段式：先"冻住"（撞击感），再"放慢"（看清自己弹开了什么 + 有时间接反击）。
					// 不能用两次独立调用 —— 那个子系统的恢复时刻是共用的，后一次会覆盖前一次，
					// 结果是只剩一段普通时缓，层次全没了。
					HitStop->RequestHitStopThenSlowMotion(
						DemoBalance::PerfectParryHitStop,
						DemoBalance::PerfectParryHitStopScale,
						DemoBalance::PerfectParrySlowMoTime,
						DemoBalance::PerfectParrySlowMoScale);
				}
				else
				{
					HitStop->RequestHitStop(
						DemoBalance::NormalParryHitStop,
						DemoBalance::NormalParryHitStopScale);
				}
			}
		}

		// ---- 火花 + 镜头冲击：只狼式弹刀"叮"的那一下 ----
		//
		// 位置取两人【之间】、弹刀者的胸口高度：
		// 贴着任何一方都会看着像打在空气上，所以要落到"两把刀相交"的那一点附近。
		FVector ParryPoint = TargetActor->GetActorLocation();
		ParryPoint.Z += 30.f;

		FVector ToAttacker = SourceActor->GetActorLocation() - TargetActor->GetActorLocation();
		const float DistanceToAttacker = ToAttacker.Size2D();
		const FVector ToAttackerDir = ToAttacker.GetSafeNormal2D();
		if (!ToAttackerDir.IsNearlyZero())
		{
			ParryPoint += ToAttackerDir * FMath::Min(DistanceToAttacker * 0.5f, 90.f);
		}

		// 法线朝攻击者 → 扩散环是【竖着】拍在两人之间的（正面朝外），
		// 而不是像命中地面那样平铺开 —— 这才是"金属相击"该有的形状。
		//
		// ⚠ 2026-10-06：扩散环已关掉（bUseRing=false）。玩家三次反馈"角色被攻击后稳心
		//   会出现一块灰色的板"。算过尺寸：环是一张 100cm 的 Plane，最终边长 =
		//   Radius × RingEndScale(2.6) × 2 = 70 × 2.6 × 2 = **364cm**，
		//   而生成点就在玩家眼前 1~2m（两人之间），HDR 强度还到 7.7 ——
		//   一张 3.6m 的加法混合平面糊在镜头前，读出来就是"一块板"，不是冲击环。
		//   弹反的读点现在交给：点光一闪 + 顿帧 + 镜头冲击 + "弹 反 !/完 美 弹 反 !"飘字。
		ADemoImpactBurst::SpawnBurst(
			TargetActor,
			ParryPoint,
			ToAttackerDir.IsNearlyZero() ? FVector::ForwardVector : ToAttackerDir,
			// 偏白的高热色；完美弹刀再亮一档
			bPerfectParry ? FLinearColor(1.f, 0.95f, 0.72f, 1.f) : FLinearColor(1.f, 0.78f, 0.42f, 1.f),
		DemoBalance::ParrySparkRadius * (bPerfectParry ? 1.f : 0.8f),
		DemoBalance::ParrySparkDuration,
		/*bSpawnLight=*/true,
		/*BurstClass=*/nullptr,
		// ⚠ 不要中心那个球（引擎基础球体网格）：弹刀时它看起来就是"凭空冒出个小球"，
		//    和刀刃相击读不到一起。只留点光，"叮"的那一下照样看得见。
		/*bUseFlash=*/false,
		/*bUseRing=*/false);

		// 角色侧反馈：玩家角色在这里做镜头冲击，BOSS 不需要（基类空实现）
		if (ADemoCharacterBase* Parrier = Cast<ADemoCharacterBase>(TargetActor))
		{
			Parrier->PlayParryFeedback(ParryPoint, bPerfectParry);
		}

		if (CVarDemoDamageLog.GetValueOnAnyThread() != 0)
		{
			UE_LOG(LogDemoCombat, Log, TEXT("[Hit] %s → %s：★%s★（攻击方进入 %.2fs 硬直）"),
				*SourceActor->GetName(), *TargetActor->GetName(),
				bPerfectParry ? TEXT("完美弹反") : TEXT("弹反"),
				bPerfectParry ? DemoBalance::PerfectParryStaggerTime : DemoBalance::NormalParryStaggerTime);
		}

		return EDemoHitResultType::Parried;
	}

	// ---------- 3) 格挡：掉少量血 + 扣耐力 ----------
	// 两个来源：
	//   ① 玩家：按住格挡键 → 身上挂着 State.Blocking 标签，这里一查就有；
	//   ② BOSS：概率触发的"主动举盾" —— 没有可查的持续状态，只能反问目标本人。
	//      ⚠⚠ 必须在这里问（伤害结算【之前】）！挪到受击回调里就只能补播个动画，
	//      血照掉、硬直照吃，玩家读到的是"它在挨打"而不是"它挡住了"。
	// ① 按住格挡（玩家）：身上挂着 State.Blocking 标签，查一下就知道。
	const bool bHeldGuardBlock = bCanBeBlocked && TargetASC->HasMatchingGameplayTag(DemoTag::State_Blocking);
	bool bBlocked = bHeldGuardBlock;
	if (!bBlocked)
	{
		if (ADemoCharacterBase* TargetChar = Cast<ADemoCharacterBase>(TargetActor))
		{
			bBlocked = TargetChar->TryGuardIncomingHit(SourceActor, BaseDamage, bCanBeBlocked);
		}
	}
	float FinalBaseDamage = BaseDamage * (bBlocked ? 0.25f : 1.f);

	// 破防状态下受击视为暴击
	const bool bCritical = TargetASC->HasMatchingGameplayTag(DemoTag::State_Broken) && !bBlocked;

	// ---------- 4) 构造上下文并施加伤害 GE ----------
	FGameplayEffectContextHandle ContextHandle = SourceASC->MakeEffectContext();
	ContextHandle.AddSourceObject(SourceActor);

	FHitResult Hit;
	Hit.ImpactPoint = TargetActor->GetActorLocation();
	Hit.Location = Hit.ImpactPoint;
	ContextHandle.AddHitResult(Hit, true);

	if (FDemoGameplayEffectContext* DemoContext = static_cast<FDemoGameplayEffectContext*>(ContextHandle.Get()))
	{
		DemoContext->HitDirection = FVector_NetQuantizeNormal(HitDirection == EDemoHitDirection::Front ? FVector::ForwardVector : FVector::UpVector);
		DemoContext->bCanBeParried = bCanBeParried;
		DemoContext->bCanBeBlocked = bCanBeBlocked;
		DemoContext->bIsCritical = bCritical;
		DemoContext->PoiseDamage = PoiseDamage;

		// 这两项是给 UI 用的：受击方 AttributeSet 结算时会读出来广播
		DemoContext->HitResultType = bBlocked
			? EDemoHitResultType::Blocked
			: (bCritical ? EDemoHitResultType::Critical : EDemoHitResultType::Normal);
		DemoContext->DamageSource = DamageSource;
	}

	FGameplayEffectSpecHandle SpecHandle = SourceASC->MakeOutgoingSpec(UDemoDamageEffect::StaticClass(), 1.f, ContextHandle);
	if (SpecHandle.IsValid())
	{
		SpecHandle.Data->SetSetByCallerMagnitude(DemoTag::Data_Damage, FinalBaseDamage);
		SpecHandle.Data->SetSetByCallerMagnitude(DemoTag::Data_PoiseDamage, bBlocked ? PoiseDamage * 0.5f : PoiseDamage);
		SourceASC->ApplyGameplayEffectSpecToTarget(*SpecHandle.Data.Get(), TargetASC);
	}

	// ---------- 4.5) 玩家受击僵直 ----------
	// 只有【真的打在玩家身上】才给僵直：
	//   · 格挡不掉僵直 —— 格挡的意义就是"挡住但不被中断"；
	//   · 完美闪避 / 弹反在前面已经 return 掉，根本走不到这里。
	// BOSS 侧有自己的一套（BossCharacterBase::OnHitReact，带"防被连击锁死"的守卫），这里只管玩家。
	if (!bBlocked && TargetActor
		&& TargetActor->IsA(ADemoCharacterBase::StaticClass())
		&& !TargetActor->IsA(ABossCharacterBase::StaticClass()))
	{
		ApplyPlayerHitstun(TargetActor);
	}

	// ---- 双异常条积累：反馈链的起点 ----
	// 只有【真的打在身上】才积累（被格挡 / 被弹反都不算）。
	// 这条约束是"BOSS 默认格挡"能成立的前提：站桩输出推不动异常条，
	// 玩家必须靠击破窗口或后摇惩罚窗口把伤害打进去 —— BOSS 于是从沙袋变成博弈对象。
	if (!bBlocked)
	{
		float ImpactGain = 0.f;
		float ElementGain = 0.f;

		if (DamageSource == EDemoDamageSource::Melee)
		{
			// 用削韧量区分"普攻连段"和"技能"：普攻 14 / 技能 50（见 DemoCombatTuning）。
			// 比新增一个枚举参数轻，而且和现有伤害表是同一套数据源，不会两边改漏。
			const bool bSkillLike = PoiseDamage >= DemoBalance::PlayerSkillPoiseDamage * 0.8f;

			if (bSkillLike)
			{
				ImpactGain = DemoBalance::SkillAnomalyImpact;
				ElementGain = DemoBalance::SkillAnomalyElement;
			}
			else
			{
				// 连段：以"第 2 段"为基准值，逐段递增的精确版本需要连段技能把段号传进来
				ImpactGain = DemoBalance::ComboAnomalyImpact[1];
				ElementGain = DemoBalance::ComboAnomalyElement[1];
			}
		}

		ApplyAnomalyGain(SourceActor, TargetActor, ImpactGain, ElementGain);
	}

	if (bBlocked)
	{
		ApplyEffectToSelf(TargetActor, UGE_BlockImpactCostEffect::StaticClass());

		// ---- 格挡扣架势 ----
		// 挡不是免费的：这一刀把架势削掉一截，削空了就要吃"被破防"的代价。
		// 带伤害 scaling（基础 8 + 伤害 ×0.25）才能教会玩家"重招别硬挡"。
		//
		// ⚠⚠ 只对【按住格挡】生效（bHeldGuardBlock）：
		//   架势条的设计语义是"玩家的还能格挡多久"（DemoAttributeSet.h），
		//   数值（PostureMax 460 / 每次 8+0.25×伤害）也是按玩家侧调的。
		//   BOSS 的概率格挡如果也吃这条，它会以每秒十几点的速度掉架势，
		//   而 BOSS 没有任何"回架势"的来源 → 打一会儿就常驻破防，
		//   表现会变成"它一挡就崩"，比不会格挡还糟。
		//   所以 BOSS 格挡只吃"伤害 ×0.25 + 表现"，不扣架势。
		if (bHeldGuardBlock)
		{
			const float PostureCost = -(DemoBalance::BlockPostureCostBase
				+ BaseDamage * DemoBalance::BlockPostureCostPerDamage);
			ApplyPostureChange(SourceActor, TargetActor, PostureCost);
		}

		// 通知"挡住的那个人"：BOSS 会靠它累计被压制次数并反制（P1-4）。
		if (ADemoCharacterBase* Blocker = Cast<ADemoCharacterBase>(TargetActor))
		{
			Blocker->OnBlockedIncomingHit(SourceActor);
		}

		// ---- 架势被这一下打空 → 被破防：补上"部分伤害"的差额 ----
		// 格挡本身只吃 25% 伤害（上面 FinalBaseDamage 已经按 0.25 结算过），
		// 破防那一下抬到 60% —— 差额在这里补一发纯伤害，语义干净（不再重复削韧）。
		if (TargetASC->HasMatchingGameplayTag(DemoTag::State_Broken))
		{
			const float Extra = BaseDamage * (DemoBalance::GuardBreakDamageScale - 0.25f);
			if (Extra > 0.f)
			{
				ApplyDamageOnly(SourceActor, TargetActor, Extra);
			}

			if (CVarDemoDamageLog.GetValueOnAnyThread() != 0)
			{
				UE_LOG(LogDemoCombat, Warning,
					TEXT("[Hit] %s 架势被打破 → 被破防！补伤 %.1f（格挡 25%% → 破防 %.0f%%）"),
					*TargetActor->GetName(), Extra, DemoBalance::GuardBreakDamageScale * 100.f);
			}
		}

		if (bTriggerHitStop)
		{
			if (UDemoHitStopSubsystem* HitStop = SourceActor->GetWorld()->GetSubsystem<UDemoHitStopSubsystem>())
			{
				HitStop->RequestHitStop(0.06f, 0.05f);
			}
		}
	}
	else if (bTriggerHitStop)
	{
		if (UDemoHitStopSubsystem* HitStop = SourceActor->GetWorld()->GetSubsystem<UDemoHitStopSubsystem>())
		{
			// 普通命中 0.09s@0.15（明显定帧）；暴击/易伤 0.13s@0.10（更重）。
			// 见 Core/DemoCombatTuning.h 的 NormalHitHitStop / CriticalHitHitStop。
			HitStop->RequestHitStop(
				bCritical ? DemoBalance::CriticalHitHitStop : DemoBalance::NormalHitHitStop,
				bCritical ? DemoBalance::CriticalHitHitStopScale : DemoBalance::NormalHitHitStopScale);
		}
	}

	// ---- 拼刀判定：双方同一窗口内都在挥刀 ----
	// 判定基于【PerformMeleeTrace 打的时间戳】而不是"处于出招状态"：
	//   出招有前摇/后摇，只看状态会把"我刚起手、对方已经收招"也判成拼刀，判定会很水。
	// 这里放在伤害结算之后：拼刀抵消的是"这一下打出来的硬直"，
	//   所以必须在 hitstun GE 已经生效之后再去摘它，顺序反了就抵消了个寂寞。
	{
		ADemoCharacterBase* SourceChar = Cast<ADemoCharacterBase>(SourceActor);
		ADemoCharacterBase* TargetChar = Cast<ADemoCharacterBase>(TargetActor);

		const bool bBothSwinging = SourceChar && TargetChar
			&& SourceChar->HasRecentSwing(DemoBalance::ClashWindow)
			&& TargetChar->HasRecentSwing(DemoBalance::ClashWindow);

		// bCanBeParried 限定"刀剑类近战"：弹幕 / 大招不该跟玩家的刀"拼"在一起
		if (bBothSwinging && bCanBeParried)
		{
			ResolveClash(SourceActor, TargetActor);
		}
	}

	// 命中事件：供 HUD、特效、音效监听。
	// 带上 ContextHandle → 受击方就能从里面读出"这一下是近战还是弹幕、有没有被格挡"。
	{
		FGameplayEventData Payload;
		Payload.Instigator = SourceActor;
		Payload.Target = TargetActor;
		Payload.EventMagnitude = FinalBaseDamage;
		Payload.ContextHandle = ContextHandle;
		UAbilitySystemBlueprintLibrary::SendGameplayEventToActor(TargetActor, DemoTag::Event_Hit_Dealt, Payload);
	}

	const EDemoHitResultType Result = bBlocked
		? EDemoHitResultType::Blocked
		: (bCritical ? EDemoHitResultType::Critical : EDemoHitResultType::Normal);

	if (CVarDemoDamageLog.GetValueOnAnyThread() != 0)
	{
		UE_LOG(LogDemoCombat, Log, TEXT("[Hit] %s → %s 伤害=%.1f 削韧=%.1f 来源=%s 结果=%s"),
			*SourceActor->GetName(), *TargetActor->GetName(), FinalBaseDamage,
			bBlocked ? PoiseDamage * 0.5f : PoiseDamage,
			DemoDamageSourceToText(DamageSource), DemoHitResultToText(Result));
	}

	return Result;
}

FActiveGameplayEffectHandle UDemoAbilitySystemLibrary::ApplyEffectToSelf(AActor* TargetActor, TSubclassOf<UGameplayEffect> EffectClass)
{
	UAbilitySystemComponent* ASC = GetAbilitySystemComponent(TargetActor);
	if (!ASC || !EffectClass)
	{
		return FActiveGameplayEffectHandle();
	}

	const FGameplayEffectContextHandle ContextHandle = ASC->MakeEffectContext();
	const FGameplayEffectSpecHandle SpecHandle = ASC->MakeOutgoingSpec(EffectClass, 1.f, ContextHandle);
	if (!SpecHandle.IsValid())
	{
		return FActiveGameplayEffectHandle();
	}

	return ASC->ApplyGameplayEffectSpecToSelf(*SpecHandle.Data.Get());
}

FActiveGameplayEffectHandle UDemoAbilitySystemLibrary::ApplyEffectToTarget(AActor* SourceActor, AActor* TargetActor, TSubclassOf<UGameplayEffect> EffectClass)
{
	UAbilitySystemComponent* SourceASC = GetAbilitySystemComponent(SourceActor);
	UAbilitySystemComponent* TargetASC = GetAbilitySystemComponent(TargetActor);
	if (!SourceASC || !TargetASC || !EffectClass)
	{
		return FActiveGameplayEffectHandle();
	}

	const FGameplayEffectContextHandle ContextHandle = SourceASC->MakeEffectContext();
	const FGameplayEffectSpecHandle SpecHandle = SourceASC->MakeOutgoingSpec(EffectClass, 1.f, ContextHandle);
	if (!SpecHandle.IsValid())
	{
		return FActiveGameplayEffectHandle();
	}

	return SourceASC->ApplyGameplayEffectSpecToTarget(*SpecHandle.Data.Get(), TargetASC);
}

void UDemoAbilitySystemLibrary::RemoveActiveEffect(AActor* TargetActor, FActiveGameplayEffectHandle Handle)
{
	UAbilitySystemComponent* ASC = GetAbilitySystemComponent(TargetActor);
	if (ASC && Handle.IsValid())
	{
		ASC->RemoveActiveGameplayEffect(Handle);
	}
}

void UDemoAbilitySystemLibrary::ApplyStagger(AActor* TargetActor, bool bIsBreak)
{
	if (!TargetActor)
	{
		return;
	}

	ApplyEffectToSelf(TargetActor,
		bIsBreak ? UGE_BreakEffect::StaticClass() : UGE_StaggerEffect::StaticClass());
}

void UDemoAbilitySystemLibrary::ApplyBossHitstun(AActor* TargetActor, float DurationScale)
{
	if (!TargetActor)
	{
		return;
	}

	// 与弹反/破防共用 State.Stagger，但时长远短（BossHitstunTime）。
	// "封 BOSS 出招 + 中断当前招式"由 GE 的 Tag 配置自动完成，Brain 负责冻结与恢复。
	// DurationScale：轻击传 <1 让硬直更短（只是"踉跄"而非整段僵直），重击传 1.0。
	const float Scale = FMath::Clamp(DurationScale, 0.15f, 3.f);
	UAbilitySystemComponent* ASC = GetAbilitySystemComponent(TargetActor);
	if (!ASC)
	{
		return;
	}
	FGameplayEffectContextHandle Ctx = ASC->MakeEffectContext();
	Ctx.AddSourceObject(TargetActor);
	FGameplayEffectSpecHandle Spec = ASC->MakeOutgoingSpec(UGE_BossHitstunEffect::StaticClass(), 1.f, Ctx);
	if (Spec.IsValid())
	{
		// 覆盖 GE 里写死的 BossHitstunTime：轻击用更短时长，重击用原值。
		// bLockDuration=true 锁住这个值，避免被 GE 定义重新求值覆盖。
		Spec.Data->SetDuration(DemoBalance::BossHitstunTime * Scale, /*bLockDuration=*/true);
		ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
	}
}

void UDemoAbilitySystemLibrary::ApplyPlayerHitstun(AActor* TargetActor)
{
	if (!TargetActor)
	{
		return;
	}

	// 玩家受击僵直：中断并短暂封禁普攻/技能，但保留闪避/弹反/格挡（见 GE 的 Tag 配置）。
	// 用独立标签 State_Hitstun，不复用破防的 State_Stagger —— 玩家里被打一下远没到破防。
	ApplyEffectToSelf(TargetActor, UGE_PlayerHitstunEffect::StaticClass());
}

void UDemoAbilitySystemLibrary::ApplyPerfectParryStagger(AActor* TargetActor)
{
	if (!TargetActor)
	{
		return;
	}

	// 与 UGE_StaggerEffect 同一套"封技能 + 中断出招"配置，只是时长更长（1.3s）。
	// 这段时间够玩家接一套 3~4 段连击 —— 就是完美弹反"高风险高回报"里的回报。
	ApplyEffectToSelf(TargetActor, UGE_PerfectParryStaggerEffect::StaticClass());
}

void UDemoAbilitySystemLibrary::ApplyPoiseDamageOnly(AActor* SourceActor, AActor* TargetActor, float PoiseDamage)
{
	if (!TargetActor || PoiseDamage <= 0.f)
	{
		return;
	}

	UAbilitySystemComponent* TargetASC = GetAbilitySystemComponent(TargetActor);
	if (!TargetASC || TargetASC->HasMatchingGameplayTag(DemoTag::State_Dead))
	{
		return;
	}

	// SourceActor 只用来建 EffectContext（谁削的韧），允许为空
	UAbilitySystemComponent* ContextASC = GetAbilitySystemComponent(SourceActor);
	if (!ContextASC)
	{
		ContextASC = TargetASC;
	}

	FGameplayEffectContextHandle ContextHandle = ContextASC->MakeEffectContext();
	ContextHandle.AddSourceObject(SourceActor ? SourceActor : TargetActor);

	FGameplayEffectSpecHandle SpecHandle =
		ContextASC->MakeOutgoingSpec(UDemoDamageEffect::StaticClass(), 1.f, ContextHandle);
	if (!SpecHandle.IsValid())
	{
		return;
	}

	// 伤害给 0 —— UDemoDamageExecution 里"BaseDamage=0 且 AttackPower=0 时不保底 1 点"，
	// 所以这一发只削韧、完全不掉血。
	SpecHandle.Data->SetSetByCallerMagnitude(DemoTag::Data_Damage, 0.f);
	SpecHandle.Data->SetSetByCallerMagnitude(DemoTag::Data_PoiseDamage, PoiseDamage);
	ContextASC->ApplyGameplayEffectSpecToTarget(*SpecHandle.Data.Get(), TargetASC);

	if (CVarDemoDamageLog.GetValueOnAnyThread() != 0)
	{
		UE_LOG(LogDemoCombat, Log, TEXT("[Hit] 额外削韧：%s → %s 削韧=%.1f（不掉血）"),
			*GetNameSafe(SourceActor), *TargetActor->GetName(), PoiseDamage);
	}
}

void UDemoAbilitySystemLibrary::ResolveClash(AActor* A, AActor* B)
{
	if (!A || !B)
	{
		return;
	}

	ADemoCharacterBase* CA = Cast<ADemoCharacterBase>(A);
	ADemoCharacterBase* CB = Cast<ADemoCharacterBase>(B);
	if (!CA || !CB)
	{
		return;
	}

	// 内置冷却：连段里每刀都稳心的话硬直系统就废了
	if (CA->HasRecentClash(DemoBalance::ClashCooldown) || CB->HasRecentClash(DemoBalance::ClashCooldown))
	{
		return;
	}
	CA->MarkClash();
	CB->MarkClash();

	// ---- 稳心：抵消硬直 ----
	// ⚠ 必须移除【授予 State_Stagger 的那个 GE】，不能只摘 loose tag：
	//    硬直是 GE 授予的 tag，只摘 tag 会在下一次 GE 刷新时被重新写回，
	//    表现为"拼刀了但还是僵在原地"。
	FGameplayTagContainer StaggerTags;
	StaggerTags.AddTag(DemoTag::State_Stagger);

	for (AActor* Side : { A, B })
	{
		if (UAbilitySystemComponent* ASC = GetAbilitySystemComponent(Side))
		{
			ASC->RemoveActiveEffectsWithGrantedTags(StaggerTags);
		}

		// 稳心回架势（BOSS 不格挡，这条对它是空转，但语义两边统一）
		ApplyPostureChange(Side, Side, DemoBalance::ClashPostureRefund);
	}

	// ---- 表现：金属相击的火花 + 顿帧，位置取两刀相交处 ----
	const FVector Mid = (A->GetActorLocation() + B->GetActorLocation()) * 0.5f;
	FVector Normal = (B->GetActorLocation() - A->GetActorLocation()).GetSafeNormal2D();
	if (Normal.IsNearlyZero())
	{
		Normal = FVector::ForwardVector;
	}

	if (UWorld* World = A->GetWorld())
	{
		// 同弹反：拼刀也是金属相击，中心球一样是"凭空冒出来的小球"，关掉；
		// 扩散环同样关掉（3.6m 的平面糊在两人之间 = 玩家说的"灰板"）。
		ADemoImpactBurst::SpawnBurst(World, Mid + FVector(0.f, 0.f, 30.f), Normal,
			FLinearColor(1.f, 0.92f, 0.70f, 1.f),
			DemoBalance::ParrySparkRadius * 0.9f, 0.28f, /*bSpawnLight=*/true,
			/*BurstClass=*/nullptr, /*bUseFlash=*/false, /*bUseRing=*/false);

		if (UDemoHitStopSubsystem* HitStop = World->GetSubsystem<UDemoHitStopSubsystem>())
		{
			HitStop->RequestHitStop(DemoBalance::ClashHitStop, DemoBalance::ClashHitStopScale);
		}
	}

	// 事件：给 UI / 音效接（蓝图也能响应）
	FGameplayEventData Payload;
	Payload.Instigator = A;
	Payload.Target = B;
	UAbilitySystemBlueprintLibrary::SendGameplayEventToActor(A, DemoTag::Event_Clash, Payload);
	UAbilitySystemBlueprintLibrary::SendGameplayEventToActor(B, DemoTag::Event_Clash, Payload);

	if (CVarDemoDamageLog.GetValueOnAnyThread() != 0)
	{
		UE_LOG(LogDemoCombat, Warning,
			TEXT("[拼刀] %s ⇄ %s 稳心：抵消硬直 + 架势 %+.1f（冷却 %.1fs）"),
			*A->GetName(), *B->GetName(), DemoBalance::ClashPostureRefund, DemoBalance::ClashCooldown);
	}
}

void UDemoAbilitySystemLibrary::ApplyPostureChange(AActor* SourceActor, AActor* TargetActor, float DeltaPosture)
{
	if (!TargetActor || FMath::IsNearlyZero(DeltaPosture))
	{
		return;
	}

	UAbilitySystemComponent* TargetASC = GetAbilitySystemComponent(TargetActor);
	if (!TargetASC || TargetASC->HasMatchingGameplayTag(DemoTag::State_Dead))
	{
		return;
	}

	UAbilitySystemComponent* ContextASC = GetAbilitySystemComponent(SourceActor);
	if (!ContextASC)
	{
		ContextASC = TargetASC;
	}

	FGameplayEffectContextHandle ContextHandle = ContextASC->MakeEffectContext();
	ContextHandle.AddSourceObject(SourceActor ? SourceActor : TargetActor);

	FGameplayEffectSpecHandle SpecHandle =
		ContextASC->MakeOutgoingSpec(UGE_PostureChangeEffect::StaticClass(), 1.f, ContextHandle);
	if (!SpecHandle.IsValid())
	{
		return;
	}

	SpecHandle.Data->SetSetByCallerMagnitude(DemoTag::Data_Posture, DeltaPosture);
	ContextASC->ApplyGameplayEffectSpecToTarget(*SpecHandle.Data.Get(), TargetASC);

	if (CVarDemoDamageLog.GetValueOnAnyThread() != 0)
	{
		UE_LOG(LogDemoCombat, Log, TEXT("[架势] %s：%+.1f（当前 %.1f / %.1f）"),
			*TargetActor->GetName(), DeltaPosture,
			TargetASC->GetNumericAttribute(UDemoAttributeSet::GetPostureAttribute()),
			TargetASC->GetNumericAttribute(UDemoAttributeSet::GetMaxPostureAttribute()));
	}
}

void UDemoAbilitySystemLibrary::ApplyStaminaCost(AActor* TargetActor, float Cost)
{
	if (!TargetActor || Cost <= 0.f)
	{
		return;
	}

	UAbilitySystemComponent* ASC = GetAbilitySystemComponent(TargetActor);
	if (!ASC)
	{
		return;
	}

	const FGameplayEffectContextHandle ContextHandle = ASC->MakeEffectContext();
	const FGameplayEffectSpecHandle SpecHandle =
		ASC->MakeOutgoingSpec(UGE_StaminaCostEffect::StaticClass(), 1.f, ContextHandle);
	if (!SpecHandle.IsValid())
	{
		return;
	}

	// 调用方传正数（= 扣多少），GE 的 ModifierOp 是 Additive，所以这里取负
	SpecHandle.Data->SetSetByCallerMagnitude(DemoTag::Data_StaminaCost, -Cost);
	ASC->ApplyGameplayEffectSpecToSelf(*SpecHandle.Data.Get());

	const float NowStamina = ASC->GetNumericAttribute(UDemoAttributeSet::GetStaminaAttribute());

	// 见底检测：这一下把体力扣光了 → 挂耗尽软惩罚（2.5s，恢复减半）。
	// 加"当前没在耗尽中"这个条件，避免空体力时每一下消耗都重挂一次、把 2.5s 计时无限刷新。
	if (NowStamina <= KINDA_SMALL_NUMBER
		&& !ASC->HasMatchingGameplayTag(DemoTag::State_StaminaExhausted))
	{
		ApplyEffectToSelf(TargetActor, UGE_StaminaExhaustEffect::StaticClass());

		if (CVarDemoDamageLog.GetValueOnAnyThread() != 0)
		{
			UE_LOG(LogDemoCombat, Log,
				TEXT("[体力] %s 体力耗尽 → 软惩罚 %.1fs（恢复减半、普攻打折，但可自由走位）"),
				*TargetActor->GetName(), DemoBalance::StaminaExhaustedTime);
		}
	}

	if (CVarDemoDamageLog.GetValueOnAnyThread() != 0)
	{
		UE_LOG(LogDemoCombat, Log, TEXT("[体力] %s：-%.1f（当前 %.1f / %.1f）"),
			*TargetActor->GetName(), Cost, NowStamina,
			ASC->GetNumericAttribute(UDemoAttributeSet::GetMaxStaminaAttribute()));
	}
}

void UDemoAbilitySystemLibrary::ApplyDamageOnly(AActor* SourceActor, AActor* TargetActor, float Damage)
{
	if (!TargetActor || Damage <= 0.f)
	{
		return;
	}

	UAbilitySystemComponent* TargetASC = GetAbilitySystemComponent(TargetActor);
	if (!TargetASC || TargetASC->HasMatchingGameplayTag(DemoTag::State_Dead))
	{
		return;
	}

	UAbilitySystemComponent* ContextASC = GetAbilitySystemComponent(SourceActor);
	if (!ContextASC)
	{
		ContextASC = TargetASC;
	}

	FGameplayEffectContextHandle ContextHandle = ContextASC->MakeEffectContext();
	ContextHandle.AddSourceObject(SourceActor ? SourceActor : TargetActor);

	FGameplayEffectSpecHandle SpecHandle =
		ContextASC->MakeOutgoingSpec(UDemoDamageEffect::StaticClass(), 1.f, ContextHandle);
	if (!SpecHandle.IsValid())
	{
		return;
	}

	SpecHandle.Data->SetSetByCallerMagnitude(DemoTag::Data_Damage, Damage);
	SpecHandle.Data->SetSetByCallerMagnitude(DemoTag::Data_PoiseDamage, 0.f);
	ContextASC->ApplyGameplayEffectSpecToTarget(*SpecHandle.Data.Get(), TargetASC);
}

void UDemoAbilitySystemLibrary::ApplyAnomalyGain(AActor* SourceActor, AActor* TargetActor, float ImpactGain, float ElementGain)
{
	if (!TargetActor || (ImpactGain <= 0.f && ElementGain <= 0.f))
	{
		return;
	}

	UAbilitySystemComponent* TargetASC = GetAbilitySystemComponent(TargetActor);
	if (!TargetASC || TargetASC->HasMatchingGameplayTag(DemoTag::State_Dead))
	{
		return;
	}

	// 只有配了异常条的角色（BOSS）才吃这套资源 —— 玩家的对应资源是架势条。
	// 少了这道闸，BOSS 每打玩家一下都在给玩家攒"失衡"，反馈链的方向就反了。
	if (const ADemoCharacterBase* TargetChar = Cast<ADemoCharacterBase>(TargetActor))
	{
		if (!TargetChar->HasAnomalyBars())
		{
			return;
		}
	}

	// 已经击破了就不再积累：虚弱窗口是明确的"收割时间"，
	// 再继续攒只会让下一轮失衡来得莫名其妙（玩家读不出节奏）。
	if (TargetASC->HasMatchingGameplayTag(DemoTag::State_Broken))
	{
		return;
	}

	UAbilitySystemComponent* ContextASC = GetAbilitySystemComponent(SourceActor);
	if (!ContextASC)
	{
		ContextASC = TargetASC;
	}

	FGameplayEffectContextHandle ContextHandle = ContextASC->MakeEffectContext();
	ContextHandle.AddSourceObject(SourceActor ? SourceActor : TargetActor);

	FGameplayEffectSpecHandle SpecHandle =
		ContextASC->MakeOutgoingSpec(UGE_AnomalyGainEffect::StaticClass(), 1.f, ContextHandle);
	if (!SpecHandle.IsValid())
	{
		return;
	}

	SpecHandle.Data->SetSetByCallerMagnitude(DemoTag::Data_AnomalyImpact, ImpactGain);
	SpecHandle.Data->SetSetByCallerMagnitude(DemoTag::Data_AnomalyElement, ElementGain);
	ContextASC->ApplyGameplayEffectSpecToTarget(*SpecHandle.Data.Get(), TargetASC);

	if (CVarDemoDamageLog.GetValueOnAnyThread() != 0)
	{
		UE_LOG(LogDemoCombat, Log, TEXT("[异常] %s → %s 冲击+%.1f 元素+%.1f"),
			*GetNameSafe(SourceActor), *TargetActor->GetName(), ImpactGain, ElementGain);
	}
}
