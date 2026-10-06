// UE 5.7: 抑制 FInheritedTagContainer 废弃 API 警告（详见 DemoGameplayEffectHelpers.cpp 注释）
#pragma warning(push)
#pragma warning(disable: 4996)

#include "Effects/DemoGameplayEffects.h"
#include "Effects/DemoGameplayEffectHelpers.h"
#include "Effects/DemoDamageExecution.h"
#include "Core/DemoAttributeSet.h"
#include "Core/DemoCombatTuning.h"
#include "Core/DemoGameplayTags.h"

// ---------------------------------------------------------------- 伤害
UDemoDamageEffect::UDemoDamageEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Instant;

	FGameplayEffectExecutionDefinition& Exec = Executions.AddDefaulted_GetRef();
	Exec.CalculationClass = UDemoDamageExecution::StaticClass();
}

// ---------------------------------------------------------------- 双异常条积累
UGE_AnomalyGainEffect::UGE_AnomalyGainEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Instant;

	// 两个通道都默认 0，实际数值由调用方用 SetSetByCallerMagnitude 塞进来：
	// 这样"哪种攻击推进哪条槽"完全由攻击手段决定，GE 本身只负责把数送到属性上。
	FGameplayModifierInfo& ImpactMod = Modifiers.AddDefaulted_GetRef();
	ImpactMod.Attribute = UDemoAttributeSet::GetAnomalyImpactAttribute();
	ImpactMod.ModifierOp = EGameplayModOp::Additive;
	FSetByCallerFloat ImpactSBC;
	ImpactSBC.DataTag = DemoTag::Data_AnomalyImpact;
	ImpactMod.ModifierMagnitude = FGameplayEffectModifierMagnitude(ImpactSBC);

	FGameplayModifierInfo& ElementMod = Modifiers.AddDefaulted_GetRef();
	ElementMod.Attribute = UDemoAttributeSet::GetAnomalyElementAttribute();
	ElementMod.ModifierOp = EGameplayModOp::Additive;
	FSetByCallerFloat ElementSBC;
	ElementSBC.DataTag = DemoTag::Data_AnomalyElement;
	ElementMod.ModifierMagnitude = FGameplayEffectModifierMagnitude(ElementSBC);
}

// ---------------------------------------------------------------- 架势条变化
UGE_PostureChangeEffect::UGE_PostureChangeEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Instant;

	FGameplayModifierInfo& Mod = Modifiers.AddDefaulted_GetRef();
	Mod.Attribute = UDemoAttributeSet::GetPostureAttribute();
	Mod.ModifierOp = EGameplayModOp::Additive;
	FSetByCallerFloat SBC;
	SBC.DataTag = DemoTag::Data_Posture;
	Mod.ModifierMagnitude = FGameplayEffectModifierMagnitude(SBC);
}

// ---------------------------------------------------------------- 硬直
UGE_StaggerEffect::UGE_StaggerEffect()
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;
	// 普通弹反 / 触发式硬直的时长，见 Core/DemoCombatTuning.h
	DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(DemoBalance::NormalParryStaggerTime));
}

// 完美弹反：同一套配置，只把时长换成"够玩家打一套连击"的长度
UGE_PerfectParryStaggerEffect::UGE_PerfectParryStaggerEffect()
{
	DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(DemoBalance::PerfectParryStaggerTime));
}

void UGE_PerfectParryStaggerEffect::CanceledAbilityOnApply(FInheritedTagContainer& OutTags)
{
	// 与基类不同：不包含 Ability_Boss_Attack。
	// 完美弹反是否中断 BOSS 招式，由 ApplyHit 按 bInterruptibleByParry 显式取消，
	// 不在 GE 里一股脑取消（否则重型招式也会被无脑打断，"部分攻击可打断"无从谈起）。
	OutTags = DemoGE::MakeTagContainer({
		DemoTag::Ability_Attack_Normal,
		DemoTag::Ability_Attack_Skill
	});
}

// 误弹重招：与完美弹反硬直同一个套路 —— 只改时长，其余配置全部继承普通硬直
UGE_MisParryStunEffect::UGE_MisParryStunEffect()
{
	DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(DemoBalance::MisParryHeavyStunTime));
}

// ---------------------------------------------------------------- 完美闪避派生窗
UGE_DodgePerfectFollowupEffect::UGE_DodgePerfectFollowupEffect()
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;
	DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(DemoBalance::PerfectDodgeFollowupWindow));
}

void UGE_DodgePerfectFollowupEffect::ConfigureGrantedTags(FInheritedTagContainer& OutTags)
{
	OutTags = DemoGE::MakeTagContainer({ DemoTag::State_DodgePerfect });
}

void UGE_StaggerEffect::ConfigureGrantedTags(FInheritedTagContainer& OutTags)
{
	OutTags = DemoGE::MakeTagContainer({ DemoTag::State_Stagger });
}

void UGE_StaggerEffect::ConfigureBlockedAbilities(FInheritedTagContainer& OutTags)
{
	OutTags = DemoGE::MakeTagContainer({
		DemoTag::Ability_Attack_Normal,
		DemoTag::Ability_Attack_Skill,
		DemoTag::Ability_Movement_Dodge,
		DemoTag::Ability_Defense_Parry,
		DemoTag::Ability_Defense_Block,
		DemoTag::Ability_Boss_Attack
	});
}

void UGE_StaggerEffect::CanceledAbilityOnApply(FInheritedTagContainer& OutTags)
{
	OutTags = DemoGE::MakeTagContainer({
		DemoTag::Ability_Attack_Normal,
		DemoTag::Ability_Attack_Skill,
		DemoTag::Ability_Boss_Attack
	});
}

// ---------------------------------------------------------------- 破防
UGE_BreakEffect::UGE_BreakEffect()
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;
	DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(2.5f));
}

void UGE_BreakEffect::ConfigureGrantedTags(FInheritedTagContainer& OutTags)
{
	OutTags = DemoGE::MakeTagContainer({ DemoTag::State_Stagger, DemoTag::Debuff_Vulnerable });
}

void UGE_BreakEffect::ConfigureBlockedAbilities(FInheritedTagContainer& OutTags)
{
	OutTags = DemoGE::MakeTagContainer({
		DemoTag::Ability_Attack_Normal,
		DemoTag::Ability_Attack_Skill,
		DemoTag::Ability_Movement_Dodge,
		DemoTag::Ability_Defense_Parry,
		DemoTag::Ability_Defense_Block,
		DemoTag::Ability_Boss_Attack
	});
}

void UGE_BreakEffect::CanceledAbilityOnApply(FInheritedTagContainer& OutTags)
{
	OutTags = DemoGE::MakeTagContainer({
		DemoTag::Ability_Attack_Normal,
		DemoTag::Ability_Attack_Skill,
		DemoTag::Ability_Boss_Attack
	});
}

// ---------------------------------------------------------------- BOSS 普通受击短硬直
UGE_BossHitstunEffect::UGE_BossHitstunEffect()
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;
	// 短硬直时长见 Core/DemoCombatTuning.h 的 BossHitstunTime
	DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(DemoBalance::BossHitstunTime));
}

void UGE_BossHitstunEffect::ConfigureGrantedTags(FInheritedTagContainer& OutTags)
{
	// 复用 State_Stagger：Brain 的全局判断会自动让 BOSS 进入 Stagger 状态并冻结
	OutTags = DemoGE::MakeTagContainer({ DemoTag::State_Stagger });
}

void UGE_BossHitstunEffect::ConfigureBlockedAbilities(FInheritedTagContainer& OutTags)
{
	// 只封 BOSS 自己的出招；不碰玩家技能，免得把玩家连段也掐了
	OutTags = DemoGE::MakeTagContainer({
		DemoTag::Ability_Boss_Attack
	});
}

void UGE_BossHitstunEffect::CanceledAbilityOnApply(FInheritedTagContainer& OutTags)
{
	// 命中瞬间如果在起手/出招，立刻中断当前招式（但 OnDamagedReaction 已守卫：
	// 只有 BOSS 处于"可被攻击空档"时才施加，committed 的招式不会被普通命中打断）
	OutTags = DemoGE::MakeTagContainer({
		DemoTag::Ability_Boss_Attack
	});
}

// ---------------------------------------------------------------- 玩家受击僵直
UGE_PlayerHitstunEffect::UGE_PlayerHitstunEffect()
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;
	// 时长见 Core/DemoCombatTuning.h 的 PlayerHitstunTime
	DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(DemoBalance::PlayerHitstunTime));
}

void UGE_PlayerHitstunEffect::ConfigureGrantedTags(FInheritedTagContainer& OutTags)
{
	OutTags = DemoGE::MakeTagContainer({ DemoTag::State_Hitstun });
}

void UGE_PlayerHitstunEffect::ConfigureBlockedAbilities(FInheritedTagContainer& OutTags)
{
	// ⚠ 只封进攻手段。闪避 / 弹反 / 格挡一律不动 —— 玩家被打了仍然能翻滚脱身，
	//   这是"不被打成连击锁死"的底线（见头文件说明）。
	OutTags = DemoGE::MakeTagContainer({
		DemoTag::Ability_Attack_Normal,
		DemoTag::Ability_Attack_Skill
	});
}

void UGE_PlayerHitstunEffect::CanceledAbilityOnApply(FInheritedTagContainer& OutTags)
{
	// 命中瞬间中断正在挥的普攻/技能 —— "这一下被打断了"的体感就来自这里
	OutTags = DemoGE::MakeTagContainer({
		DemoTag::Ability_Attack_Normal,
		DemoTag::Ability_Attack_Skill
	});
}

// ---------------------------------------------------------------- 无敌帧
// 时长覆盖整个闪避动作（DemoCombatTuning.h 的 DodgeInvincibleDuration）。
//
// ⚠ 这条是"闪了却照样挨打"的根治点：
//   以前闪避完全不发无敌帧，State.Invincible 只能靠闪避蒙太奇上的动画通知给，
//   而闪避蒙太奇是脚本批量生成的、里面根本没有通知
//   → 闪避期间玩家没有任何免伤，纯位移，手感上等于"没有闪避"。
//   现在由 UGA_PlayerDodge 显式 ApplyEffectToSelf，不再依赖动画资产。
UGE_InvincibleEffect::UGE_InvincibleEffect()
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;
	DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(DemoBalance::DodgeInvincibleDuration));
}

void UGE_InvincibleEffect::ConfigureGrantedTags(FInheritedTagContainer& OutTags)
{
	OutTags = DemoGE::MakeTagContainer({ DemoTag::State_Invincible });
}

// ---------------------------------------------------------------- 降级无敌帧（低体力闪避）
UGE_InvincibleWeakEffect::UGE_InvincibleWeakEffect()
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;
	DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(DemoBalance::DodgeWeakInvincibleTime));
}

void UGE_InvincibleWeakEffect::ConfigureGrantedTags(FInheritedTagContainer& OutTags)
{
	// 标签与正常无敌帧完全一致：ApplyHit 那边只认 State.Invincible，
	// 不需要知道这次是"满状态闪"还是"没力气地扑一下"。
	OutTags = DemoGE::MakeTagContainer({ DemoTag::State_Invincible });
}

// ---------------------------------------------------------------- 完美闪避判定窗
UGE_PerfectDodgeWindowEffect::UGE_PerfectDodgeWindowEffect()
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;
	DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(DemoBalance::PerfectDodgeWindow));
}

void UGE_PerfectDodgeWindowEffect::ConfigureGrantedTags(FInheritedTagContainer& OutTags)
{
	OutTags = DemoGE::MakeTagContainer({ DemoTag::State_PerfectDodgeWindow });
}

// ---------------------------------------------------------------- 完美弹反判定窗
UGE_PerfectParryWindowEffect::UGE_PerfectParryWindowEffect()
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;
	DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(DemoBalance::PerfectParryWindow));
}

void UGE_PerfectParryWindowEffect::ConfigureGrantedTags(FInheritedTagContainer& OutTags)
{
	OutTags = DemoGE::MakeTagContainer({ DemoTag::State_PerfectParryWindow });
}

// ---------------------------------------------------------------- 完美判定奖励（回耐力）
UGE_PerfectRewardEffect::UGE_PerfectRewardEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Instant;

	FGameplayModifierInfo& Mod = Modifiers.AddDefaulted_GetRef();
	Mod.Attribute = UDemoAttributeSet::GetStaminaAttribute();
	Mod.ModifierOp = EGameplayModOp::Additive;
	Mod.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(DemoBalance::PerfectParryStaminaRefund));
}

// ---------------------------------------------------------------- 完美闪避专用返还（+20 < 弹反的 +25）
UGE_PerfectDodgeRewardEffect::UGE_PerfectDodgeRewardEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Instant;

	FGameplayModifierInfo& Mod = Modifiers.AddDefaulted_GetRef();
	Mod.Attribute = UDemoAttributeSet::GetStaminaAttribute();
	Mod.ModifierOp = EGameplayModOp::Additive;
	Mod.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(DemoBalance::PerfectDodgeStaminaRefund));
}

// ---------------------------------------------------------------- 弹刀判定窗
UGE_ParryWindowEffect::UGE_ParryWindowEffect()
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;
	// 这才是真正的弹刀判定窗时长：State.Parrying 存在时间 = 能弹到 Boss 攻击的时间。
	// ⚠ 必须与 UGA_PlayerParry::WindowDuration 保持一致（都取自 DemoCombatTuning.h），
	//   改这里就要同步改那边；否则会出现"技能还在但已经弹不动了"。
	DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(DemoBalance::ParryWindowDuration));
}

void UGE_ParryWindowEffect::ConfigureGrantedTags(FInheritedTagContainer& OutTags)
{
	OutTags = DemoGE::MakeTagContainer({ DemoTag::State_Parrying });
}

// ---------------------------------------------------------------- 格挡状态
UGE_BlockingEffect::UGE_BlockingEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Infinite;
}

void UGE_BlockingEffect::ConfigureGrantedTags(FInheritedTagContainer& OutTags)
{
	OutTags = DemoGE::MakeTagContainer({ DemoTag::State_Blocking });
}

// ---------------------------------------------------------------- 韧性恢复
UGE_PoiseRecoveryEffect::UGE_PoiseRecoveryEffect()
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;
	DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(2.f));
	Period = FScalableFloat(0.2f);
	bExecutePeriodicEffectOnApplication = false;

	FGameplayModifierInfo& Mod = Modifiers.AddDefaulted_GetRef();
	Mod.Attribute = UDemoAttributeSet::GetPoiseAttribute();
	Mod.ModifierOp = EGameplayModOp::Additive;
	Mod.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(15.f));
}

// ---------------------------------------------------------------- 耐力恢复
UGE_StaminaRegenEffect::UGE_StaminaRegenEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Infinite;
	Period = FScalableFloat(0.1f);
	bExecutePeriodicEffectOnApplication = false;

	FGameplayModifierInfo& Mod = Modifiers.AddDefaulted_GetRef();
	Mod.Attribute = UDemoAttributeSet::GetStaminaAttribute();
	Mod.ModifierOp = EGameplayModOp::Additive;
	Mod.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(3.f));
}

// ---------------------------------------------------------------- 闪避消耗
UGE_DodgeCostEffect::UGE_DodgeCostEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Instant;

	FGameplayModifierInfo& Mod = Modifiers.AddDefaulted_GetRef();
	Mod.Attribute = UDemoAttributeSet::GetStaminaAttribute();
	Mod.ModifierOp = EGameplayModOp::Additive;
	Mod.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(-20.f));
}

// ---------------------------------------------------------------- 格挡受击消耗
UGE_BlockImpactCostEffect::UGE_BlockImpactCostEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Instant;

	FGameplayModifierInfo& Mod = Modifiers.AddDefaulted_GetRef();
	Mod.Attribute = UDemoAttributeSet::GetStaminaAttribute();
	Mod.ModifierOp = EGameplayModOp::Additive;
	Mod.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(-12.f));
}

// ---------------------------------------------------------------- 通用体力消耗
UGE_StaminaCostEffect::UGE_StaminaCostEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Instant;

	// 扣多少由调用方用 SetByCaller 决定（一个 GE 覆盖连段每段不同价 / 技能 / 空弹）。
	// 注意：这里仍是 Additive，取负的操作放在 UDemoAbilitySystemLibrary::ApplyStaminaCost 里，
	// 这样调用方一律传"正数 = 扣多少"，读代码时不会被符号绕晕。
	FGameplayModifierInfo& Mod = Modifiers.AddDefaulted_GetRef();
	Mod.Attribute = UDemoAttributeSet::GetStaminaAttribute();
	Mod.ModifierOp = EGameplayModOp::Additive;
	FSetByCallerFloat SBC;
	SBC.DataTag = DemoTag::Data_StaminaCost;
	Mod.ModifierMagnitude = FGameplayEffectModifierMagnitude(SBC);
}

// ---------------------------------------------------------------- 体力耗尽
UGE_StaminaExhaustEffect::UGE_StaminaExhaustEffect()
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;
	DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(DemoBalance::StaminaExhaustedTime));

	Period = FScalableFloat(0.1f);
	bExecutePeriodicEffectOnApplication = false;

	// 每 0.1s 扣 1.5，与恢复 GE 的 +3/0.1s 抵消 → 净恢复 +15/s（正常是 +30/s）。
	// 用"叠加一条负的周期效果"而不是改恢复 GE，是为了不动那条全局 Infinite 恢复，
	// 耗尽结束这个 GE 一消失，恢复速率自然回到 30/s，不需要额外还原逻辑。
	FGameplayModifierInfo& Mod = Modifiers.AddDefaulted_GetRef();
	Mod.Attribute = UDemoAttributeSet::GetStaminaAttribute();
	Mod.ModifierOp = EGameplayModOp::Additive;
	Mod.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(DemoBalance::StaminaExhaustedRegenPerTick));
}

void UGE_StaminaExhaustEffect::ConfigureGrantedTags(FInheritedTagContainer& OutTags)
{
	OutTags = DemoGE::MakeTagContainer({ DemoTag::State_StaminaExhausted });
}

// ---------------------------------------------------------------- 冷却
UGE_SkillCooldownEffect::UGE_SkillCooldownEffect()
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;
	DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(5.f));
}

void UGE_SkillCooldownEffect::ConfigureGrantedTags(FInheritedTagContainer& OutTags)
{
	OutTags = DemoGE::MakeTagContainer({ DemoTag::Cooldown_Attack_Skill });
}

// ---------------------------------------------------------------- 闪避冷却
UGE_DodgeCooldownEffect::UGE_DodgeCooldownEffect()
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;
	DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(0.5f));
}

void UGE_DodgeCooldownEffect::ConfigureGrantedTags(FInheritedTagContainer& OutTags)
{
	OutTags = DemoGE::MakeTagContainer({ DemoTag::Cooldown_Dodge });
}

// ================================================================ 道具：回血"酒"
UGE_HealEffect::UGE_HealEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Instant;

	// 加多少由调用方用 SetSetByCallerMagnitude(Data_Heal, 数值) 决定。
	// 数值 = 最大生命 × HealPercent（见 UGA_PlayerUseHeal），这里只把通道数送到 Health。
	FGameplayModifierInfo& Mod = Modifiers.AddDefaulted_GetRef();
	Mod.Attribute = UDemoAttributeSet::GetHealthAttribute();
	Mod.ModifierOp = EGameplayModOp::Additive;
	FSetByCallerFloat SBC;
	SBC.DataTag = DemoTag::Data_Heal;
	Mod.ModifierMagnitude = FGameplayEffectModifierMagnitude(SBC);
}

UGE_HealCooldownEffect::UGE_HealCooldownEffect()
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;
	DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(DemoBalance::HealCooldown));
}

void UGE_HealCooldownEffect::ConfigureGrantedTags(FInheritedTagContainer& OutTags)
{
	OutTags = DemoGE::MakeTagContainer({ DemoTag::Cooldown_UseItem_Heal });
}

#pragma warning(pop)
