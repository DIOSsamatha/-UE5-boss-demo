#include "Core/DemoAttributeSet.h"
#include "Core/DemoGameplayTags.h"
#include "Core/DemoEffectContext.h"
#include "GameplayEffectExtension.h"
#include "Net/UnrealNetwork.h"

// ⚠⚠ 这里的数值是【构造哨兵】，不是最终平衡数值！⚠⚠
//
// 真正的战斗数值在 Core/DemoCombatTuning.h，由 ADemoCharacterBase::EnsureDefaultAttributes()
// 在 BeginPlay 最开始时按角色类型写进来（玩家 2000 血 / BOSS 10000 血，等等）。
//
// 为什么不用构造函数直接设最终值：
//   AttributeSet 的 CDO 是玩家和 BOSS 共用的，而双方数值完全不同（血量差 5 倍）。
//   在构造里写死任何一套，另一方就只能是错的。
//
// 为什么哨兵值偏偏要留成 100：
//   EnsureDefaultAttributes 用"MaxHealth 是否还等于哨兵值"来判断"有没有人配过属性"。
//   历史事故：上一版的守卫写成 `if (GetMaxHealth() > 0) return;`，
//   而构造里已经把 MaxHealth 初始化成 100 —— 守卫永远命中，
//   兜底从未执行过，于是 BOSS 血量实际是 100、玩家也是 100，双方几下就打死对方。
//   如果你把下面这个 100 改掉，必须【同步】改 EnsureDefaultAttributes 里的 UnconfiguredHealth。
UDemoAttributeSet::UDemoAttributeSet()
{
	InitMaxHealth(100.f);
	InitHealth(100.f);

	InitMaxStamina(100.f);
	InitStamina(100.f);

	InitMaxPoise(100.f);
	InitPoise(100.f);

	// 双异常条：当前值从 0 开始（不是满的），上限来自平衡表。
	// 注意这里的 100 只是"占位哨兵"，真正生效的是 EnsureDefaultAttributes 里写进去的
	// DemoBalance::AnomalyImpactMax / AnomalyElementMax。
	InitMaxAnomalyImpact(100.f);
	InitAnomalyImpact(0.f);
	InitMaxAnomalyElement(100.f);
	InitAnomalyElement(0.f);

	// 架势：满槽开局（"还能挡多久"是消耗品，不是要攒的资源）
	InitMaxPosture(100.f);
	InitPosture(100.f);

	// 攻防留 0 作为"未配置"哨兵（平衡表里两边也都是 0，伤害全写在招式基础值里）
	InitAttackPower(0.f);
	InitDefensePower(0.f);

	// 移速同样留"未配置"值：玩家侧的最终移速由 AFightPlayerCharacter 的
	// WalkSpeed / RunSpeed 接管，BOSS 侧由 BalanceMoveSpeed 写进来。
	InitMoveSpeed(600.f);
}

void UDemoAttributeSet::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME_CONDITION_NOTIFY(UDemoAttributeSet, Health, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UDemoAttributeSet, MaxHealth, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UDemoAttributeSet, Stamina, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UDemoAttributeSet, MaxStamina, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UDemoAttributeSet, Poise, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UDemoAttributeSet, MaxPoise, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UDemoAttributeSet, Posture, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UDemoAttributeSet, MaxPosture, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UDemoAttributeSet, AnomalyImpact, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UDemoAttributeSet, MaxAnomalyImpact, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UDemoAttributeSet, AnomalyElement, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UDemoAttributeSet, MaxAnomalyElement, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UDemoAttributeSet, AttackPower, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UDemoAttributeSet, DefensePower, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UDemoAttributeSet, MoveSpeed, COND_None, REPNOTIFY_Always);
}

void UDemoAttributeSet::PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue)
{
	Super::PreAttributeChange(Attribute, NewValue);

	if (Attribute == GetHealthAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.f, GetMaxHealth());
	}
	else if (Attribute == GetStaminaAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.f, GetMaxStamina());
	}
	else if (Attribute == GetPoiseAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.f, GetMaxPoise());
	}
	else if (Attribute == GetPostureAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.f, GetMaxPosture());
	}
	else if (Attribute == GetAnomalyImpactAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.f, GetMaxAnomalyImpact());
	}
	else if (Attribute == GetAnomalyElementAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.f, GetMaxAnomalyElement());
	}
	else if (Attribute == GetMoveSpeedAttribute())
	{
		NewValue = FMath::Max(NewValue, 0.f);
	}
}

void UDemoAttributeSet::PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data)
{
	Super::PostGameplayEffectExecute(Data);

	UAbilitySystemComponent* ASC = GetOwningAbilitySystemComponent();

	if (Data.EvaluatedData.Attribute == GetHealthAttribute())
	{
		SetHealth(FMath::Clamp(GetHealth(), 0.f, GetMaxHealth()));
		OnHealthChanged.Broadcast(GetHealth(), GetMaxHealth());

		// ---- 掉血（而不是回血）：把"实际扣了多少 + 这一下的性质"广播出去 ----
		// 这里是全流程唯一能拿到"结算后真实伤害"的位置：
		//   ApplyHit 只知道原始伤害，攻防减免/格挡减伤/易伤加成都是在 Execution 里算的。
		const float AppliedDelta = Data.EvaluatedData.Magnitude;
		if (AppliedDelta < 0.f)
		{
			EDemoHitResultType ResultType = EDemoHitResultType::Normal;
			EDemoDamageSource DamageSource = EDemoDamageSource::Unknown;
			FVector HitDirection = FVector::ZeroVector;

			const FGameplayEffectContext* RawContext = Data.EffectSpec.GetContext().Get();
			if (RawContext && RawContext->GetScriptStruct() == FDemoGameplayEffectContext::StaticStruct())
			{
				const FDemoGameplayEffectContext* DemoContext = static_cast<const FDemoGameplayEffectContext*>(RawContext);
				ResultType = DemoContext->HitResultType;
				DamageSource = DemoContext->DamageSource;
				HitDirection = FVector(DemoContext->HitDirection);
			}

			OnDamageTaken.Broadcast(-AppliedDelta, ResultType, DamageSource, HitDirection);
		}

		if (GetHealth() <= 0.f && ASC && !ASC->HasMatchingGameplayTag(DemoTag::State_Dead))
		{
			ASC->AddLooseGameplayTag(DemoTag::State_Dead);
			OnDeath.Broadcast();
		}
	}
	else if (Data.EvaluatedData.Attribute == GetStaminaAttribute())
	{
		SetStamina(FMath::Clamp(GetStamina(), 0.f, GetMaxStamina()));
		OnStaminaChanged.Broadcast(GetStamina(), GetMaxStamina());
	}
	else if (Data.EvaluatedData.Attribute == GetPoiseAttribute())
	{
		SetPoise(FMath::Clamp(GetPoise(), 0.f, GetMaxPoise()));
		OnPoiseChanged.Broadcast(GetPoise(), GetMaxPoise());

		if (GetPoise() <= 0.f && ASC && !ASC->HasMatchingGameplayTag(DemoTag::State_Broken))
		{
			ASC->AddLooseGameplayTag(DemoTag::State_Broken);
			OnPoiseBroken.Broadcast();
		}
	}
	else if (Data.EvaluatedData.Attribute == GetPostureAttribute())
	{
		SetPosture(FMath::Clamp(GetPosture(), 0.f, GetMaxPosture()));
		OnPostureChanged.Broadcast(GetPosture(), GetMaxPosture());

		// 架势打空 → 被破防。和韧性清零一样只发一次（已有 State_Broken 就不再发），
		// 免得连续挨打时每次都触发一次硬直，变成永控。
		if (GetPosture() <= 0.f && ASC && !ASC->HasMatchingGameplayTag(DemoTag::State_Broken))
		{
			OnPostureBroken.Broadcast();
		}
	}
	else if (Data.EvaluatedData.Attribute == GetAnomalyImpactAttribute())
	{
		SetAnomalyImpact(FMath::Clamp(GetAnomalyImpact(), 0.f, GetMaxAnomalyImpact()));
		OnAnomalyImpactChanged.Broadcast(GetAnomalyImpact(), GetMaxAnomalyImpact());

		// 冲击槽打满 → 失衡。刻意【不在这里清零】：条停在满格才是"再压两刀就击破"的信号，
		// 清零由 BOSS 侧在失衡结束/被击破时统一做（ABossCharacterBase::ExitUnstable）。
		if (ASC && GetAnomalyImpact() >= GetMaxAnomalyImpact()
			&& !ASC->HasMatchingGameplayTag(DemoTag::State_Unstable))
		{
			ASC->AddLooseGameplayTag(DemoTag::State_Unstable);
			OnAnomalyUnstable.Broadcast();
		}
	}
	else if (Data.EvaluatedData.Attribute == GetAnomalyElementAttribute())
	{
		SetAnomalyElement(FMath::Clamp(GetAnomalyElement(), 0.f, GetMaxAnomalyElement()));
		OnAnomalyElementChanged.Broadcast(GetAnomalyElement(), GetMaxAnomalyElement());

		// 元素槽打满。参考 Demo 的规则是"乏力状态下橙条再被打满 → 击破"，
		// 这里的"乏力"对应我们已经挂上的 State_Unstable（失衡）态：
		//   只满元素槽          → 什么都不发生（避免玩家靠单一手段无脑击破）
		//   元素槽满 + 已失衡   → 击破：失去格挡 + 虚弱窗口（BOSS 侧接表现）
		if (ASC && GetAnomalyElement() >= GetMaxAnomalyElement()
			&& ASC->HasMatchingGameplayTag(DemoTag::State_Unstable)
			&& !ASC->HasMatchingGameplayTag(DemoTag::State_Broken))
		{
			ASC->AddLooseGameplayTag(DemoTag::State_Broken);
			OnAnomalyBroken.Broadcast();
		}
	}
}

void UDemoAttributeSet::OnRep_Posture(const FGameplayAttributeData& OldValue) const
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UDemoAttributeSet, Posture, OldValue);
}

void UDemoAttributeSet::OnRep_MaxPosture(const FGameplayAttributeData& OldValue) const
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UDemoAttributeSet, MaxPosture, OldValue);
}

void UDemoAttributeSet::OnRep_AnomalyImpact(const FGameplayAttributeData& OldValue) const
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UDemoAttributeSet, AnomalyImpact, OldValue);
}

void UDemoAttributeSet::OnRep_MaxAnomalyImpact(const FGameplayAttributeData& OldValue) const
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UDemoAttributeSet, MaxAnomalyImpact, OldValue);
}

void UDemoAttributeSet::OnRep_AnomalyElement(const FGameplayAttributeData& OldValue) const
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UDemoAttributeSet, AnomalyElement, OldValue);
}

void UDemoAttributeSet::OnRep_MaxAnomalyElement(const FGameplayAttributeData& OldValue) const
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UDemoAttributeSet, MaxAnomalyElement, OldValue);
}

void UDemoAttributeSet::OnRep_Health(const FGameplayAttributeData& OldValue) const
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UDemoAttributeSet, Health, OldValue);
}

void UDemoAttributeSet::OnRep_MaxHealth(const FGameplayAttributeData& OldValue) const
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UDemoAttributeSet, MaxHealth, OldValue);
}

void UDemoAttributeSet::OnRep_Stamina(const FGameplayAttributeData& OldValue) const
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UDemoAttributeSet, Stamina, OldValue);
}

void UDemoAttributeSet::OnRep_MaxStamina(const FGameplayAttributeData& OldValue) const
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UDemoAttributeSet, MaxStamina, OldValue);
}

void UDemoAttributeSet::OnRep_Poise(const FGameplayAttributeData& OldValue) const
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UDemoAttributeSet, Poise, OldValue);
}

void UDemoAttributeSet::OnRep_MaxPoise(const FGameplayAttributeData& OldValue) const
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UDemoAttributeSet, MaxPoise, OldValue);
}

void UDemoAttributeSet::OnRep_AttackPower(const FGameplayAttributeData& OldValue) const
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UDemoAttributeSet, AttackPower, OldValue);
}

void UDemoAttributeSet::OnRep_DefensePower(const FGameplayAttributeData& OldValue) const
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UDemoAttributeSet, DefensePower, OldValue);
}

void UDemoAttributeSet::OnRep_MoveSpeed(const FGameplayAttributeData& OldValue) const
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UDemoAttributeSet, MoveSpeed, OldValue);
}
