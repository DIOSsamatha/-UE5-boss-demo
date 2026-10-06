#include "Effects/DemoDamageExecution.h"
#include "Core/DemoAttributeSet.h"
#include "Core/DemoGameplayTags.h"
#include "Core/DemoCombatTuning.h"

struct FDemoDamageStatics
{
	DECLARE_ATTRIBUTE_CAPTUREDEF(AttackPower);
	DECLARE_ATTRIBUTE_CAPTUREDEF(DefensePower);
	DECLARE_ATTRIBUTE_CAPTUREDEF(Health);
	DECLARE_ATTRIBUTE_CAPTUREDEF(Poise);

	FDemoDamageStatics()
	{
		DEFINE_ATTRIBUTE_CAPTUREDEF(UDemoAttributeSet, AttackPower, Source, false);
		DEFINE_ATTRIBUTE_CAPTUREDEF(UDemoAttributeSet, DefensePower, Target, false);
		DEFINE_ATTRIBUTE_CAPTUREDEF(UDemoAttributeSet, Health, Target, false);
		DEFINE_ATTRIBUTE_CAPTUREDEF(UDemoAttributeSet, Poise, Target, false);
	}
};

static const FDemoDamageStatics& DamageStatics()
{
	static FDemoDamageStatics Statics;
	return Statics;
}

UDemoDamageExecution::UDemoDamageExecution()
{
	RelevantAttributesToCapture.Add(DamageStatics().AttackPowerDef);
	RelevantAttributesToCapture.Add(DamageStatics().DefensePowerDef);
	RelevantAttributesToCapture.Add(DamageStatics().HealthDef);
	RelevantAttributesToCapture.Add(DamageStatics().PoiseDef);
}

void UDemoDamageExecution::Execute_Implementation(const FGameplayEffectCustomExecutionParameters& ExecutionParams,
	FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const
{
	const FGameplayEffectSpec& Spec = ExecutionParams.GetOwningSpec();

	FAggregatorEvaluateParameters EvalParams;
	EvalParams.SourceTags = Spec.CapturedSourceTags.GetAggregatedTags();
	EvalParams.TargetTags = Spec.CapturedTargetTags.GetAggregatedTags();

	float AttackPower = 0.f;
	ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(DamageStatics().AttackPowerDef, EvalParams, AttackPower);

	float DefensePower = 0.f;
	ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(DamageStatics().DefensePowerDef, EvalParams, DefensePower);

	float BaseDamage = Spec.GetSetByCallerMagnitude(DemoTag::Data_Damage, false, 0.f);
	float PoiseDamage = Spec.GetSetByCallerMagnitude(DemoTag::Data_PoiseDamage, false, 0.f);

	// 1) 攻击力折算
	float FinalDamage = BaseDamage + AttackPower;

	// 2) 防御减伤：D / (D + 100) 的收益递减曲线
	const float Reduction = DefensePower / (DefensePower + 100.f);
	FinalDamage *= (1.f - Reduction);

	// 3) 状态效果修正
	const FGameplayTagContainer* TargetTags = EvalParams.TargetTags;
	if (TargetTags)
	{
		if (TargetTags->HasTag(DemoTag::Debuff_Vulnerable))
		{
			FinalDamage *= 1.35f;
		}
		if (TargetTags->HasTag(DemoTag::Debuff_Bleed))
		{
			FinalDamage *= 1.15f;
		}
		if (TargetTags->HasTag(DemoTag::State_Broken))
		{
			FinalDamage *= 1.5f;
		}
		// 失衡受伤加成：这是"把异常条打满"这件事的【唯一直观收益】。
		// 没有它，玩家只会觉得"多了个绿圈但打起来一样费劲"，反馈链就断在最后一环。
		if (TargetTags->HasTag(DemoTag::State_Unstable))
		{
			FinalDamage *= DemoBalance::UnstableDamageBonus;
		}
		if (TargetTags->HasTag(DemoTag::Buff_DefenseUp))
		{
			FinalDamage *= 0.7f;
		}
	}

	if (TargetTags && TargetTags->HasTag(DemoTag::Buff_AttackUp))
	{
		FinalDamage *= 1.25f;
	}

	// 保底 1 点：避免"明明打中了却掉 0 血"的观感问题。
	//
	// ⚠ 但只在【本来就想造成伤害】时才保底：
	//   完美弹反的额外削韧会走同一个 Execution，把 BaseDamage 传 0、只带 PoiseDamage。
	//   以前这里无条件 Max(...,1)，于是"纯削韧"也会顺带扣 1 点血 —— 数值不大，
	//   但它会让"这一下只削韧不掉血"的语义失效，日志和飘字都会对不上。
	const bool bIntendedDamage = (BaseDamage > 0.f) || (AttackPower > 0.f);
	if (bIntendedDamage)
	{
		FinalDamage = FMath::Max(FinalDamage, 1.f);
	}
	else
	{
		FinalDamage = 0.f;
	}

	if (FinalDamage > 0.f)
	{
		OutExecutionOutput.AddOutputModifier(
			FGameplayModifierEvaluatedData(DamageStatics().HealthProperty, EGameplayModOp::Additive, -FinalDamage));
	}

	if (PoiseDamage > 0.f)
	{
		OutExecutionOutput.AddOutputModifier(
			FGameplayModifierEvaluatedData(DamageStatics().PoiseProperty, EGameplayModOp::Additive, -PoiseDamage));
	}
}
