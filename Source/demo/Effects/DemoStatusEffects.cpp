// UE 5.7: 抑制 FInheritedTagContainer 废弃 API 警告（详见 DemoGameplayEffectHelpers.cpp 注释）
#pragma warning(push)
#pragma warning(disable: 4996)

#include "Effects/DemoStatusEffects.h"
#include "Effects/DemoGameplayEffectHelpers.h"
#include "Core/DemoAttributeSet.h"
#include "Core/DemoGameplayTags.h"

// ---------------------------------------------------------------- 燃烧
UGE_BurnEffect::UGE_BurnEffect()
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;
	DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(5.f));
	Period = FScalableFloat(0.5f);
	bExecutePeriodicEffectOnApplication = false;

	FGameplayModifierInfo& Mod = Modifiers.AddDefaulted_GetRef();
	Mod.Attribute = UDemoAttributeSet::GetHealthAttribute();
	Mod.ModifierOp = EGameplayModOp::Additive;
	Mod.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(-3.f));
}

void UGE_BurnEffect::ConfigureGrantedTags(FInheritedTagContainer& OutTags)
{
	OutTags = DemoGE::MakeTagContainer({ DemoTag::Debuff_Burn });
}

// ---------------------------------------------------------------- 中毒
UGE_PoisonEffect::UGE_PoisonEffect()
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;
	DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(8.f));
	Period = FScalableFloat(0.5f);
	bExecutePeriodicEffectOnApplication = false;

	FGameplayModifierInfo& HealthMod = Modifiers.AddDefaulted_GetRef();
	HealthMod.Attribute = UDemoAttributeSet::GetHealthAttribute();
	HealthMod.ModifierOp = EGameplayModOp::Additive;
	HealthMod.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(-2.f));

	FGameplayModifierInfo& PoiseMod = Modifiers.AddDefaulted_GetRef();
	PoiseMod.Attribute = UDemoAttributeSet::GetPoiseAttribute();
	PoiseMod.ModifierOp = EGameplayModOp::Additive;
	PoiseMod.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(-1.f));
}

void UGE_PoisonEffect::ConfigureGrantedTags(FInheritedTagContainer& OutTags)
{
	OutTags = DemoGE::MakeTagContainer({ DemoTag::Debuff_Poison });
}

// ---------------------------------------------------------------- 流血
UGE_BleedEffect::UGE_BleedEffect()
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;
	DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(6.f));
}

void UGE_BleedEffect::ConfigureGrantedTags(FInheritedTagContainer& OutTags)
{
	OutTags = DemoGE::MakeTagContainer({ DemoTag::Debuff_Bleed });
}

// ---------------------------------------------------------------- 易伤
UGE_VulnerableEffect::UGE_VulnerableEffect()
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;
	DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(5.f));
}

void UGE_VulnerableEffect::ConfigureGrantedTags(FInheritedTagContainer& OutTags)
{
	OutTags = DemoGE::MakeTagContainer({ DemoTag::Debuff_Vulnerable });
}

// ---------------------------------------------------------------- 减速
UGE_SlowEffect::UGE_SlowEffect()
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;
	DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(4.f));

	FGameplayModifierInfo& Mod = Modifiers.AddDefaulted_GetRef();
	Mod.Attribute = UDemoAttributeSet::GetMoveSpeedAttribute();
	Mod.ModifierOp = EGameplayModOp::Multiplicitive;
	Mod.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(0.5f));
}

void UGE_SlowEffect::ConfigureGrantedTags(FInheritedTagContainer& OutTags)
{
	OutTags = DemoGE::MakeTagContainer({ DemoTag::Debuff_Slow });
}

// ---------------------------------------------------------------- 攻击提升
UGE_AttackUpEffect::UGE_AttackUpEffect()
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;
	DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(8.f));

	FGameplayModifierInfo& Mod = Modifiers.AddDefaulted_GetRef();
	Mod.Attribute = UDemoAttributeSet::GetAttackPowerAttribute();
	Mod.ModifierOp = EGameplayModOp::Additive;
	Mod.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(15.f));
}

void UGE_AttackUpEffect::ConfigureGrantedTags(FInheritedTagContainer& OutTags)
{
	OutTags = DemoGE::MakeTagContainer({ DemoTag::Buff_AttackUp });
}

// ---------------------------------------------------------------- 防御提升
UGE_DefenseUpEffect::UGE_DefenseUpEffect()
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;
	DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(8.f));

	FGameplayModifierInfo& Mod = Modifiers.AddDefaulted_GetRef();
	Mod.Attribute = UDemoAttributeSet::GetDefensePowerAttribute();
	Mod.ModifierOp = EGameplayModOp::Additive;
	Mod.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(20.f));
}

void UGE_DefenseUpEffect::ConfigureGrantedTags(FInheritedTagContainer& OutTags)
{
	OutTags = DemoGE::MakeTagContainer({ DemoTag::Buff_DefenseUp });
}

#pragma warning(pop)
