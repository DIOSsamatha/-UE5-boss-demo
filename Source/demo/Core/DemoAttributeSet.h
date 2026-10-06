// 战斗属性集：血量 / 耐力 / 韧性 / 攻防 / 移速
#pragma once

#include "CoreMinimal.h"
#include "AttributeSet.h"
#include "AbilitySystemComponent.h"
#include "DemoCommon.h"
#include "DemoAttributeSet.generated.h"

DECLARE_MULTICAST_DELEGATE_TwoParams(FDemoOnAttributeChanged, float /*NewValue*/, float /*MaxValue*/);
DECLARE_MULTICAST_DELEGATE(FDemoOnPoiseBroken);
DECLARE_MULTICAST_DELEGATE(FDemoOnDeath);

// "我实际掉了多少血、这一下是什么性质" —— UI 的飘字 / 命中反馈靠它拿准确数字。
// 注意这里的 Damage 是**结算之后真正扣掉的值**（已经过攻防与状态修正、也已经过格挡减伤），
// 不是 ApplyHit 传进来的原始伤害，所以飘字跟血条变化永远是自洽的。
DECLARE_MULTICAST_DELEGATE_FourParams(FDemoOnDamageTaken,
	float /*AppliedDamage*/,
	EDemoHitResultType /*ResultType*/,
	EDemoDamageSource /*DamageSource*/,
	FVector /*HitDirection*/);

UCLASS()
class DEMO_API UDemoAttributeSet : public UAttributeSet
{
	GENERATED_BODY()

public:
	UDemoAttributeSet();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) override;
	virtual void PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data) override;

	// ---------- 生命 ----------
	UPROPERTY(BlueprintReadOnly, Category = "Demo|Vital", ReplicatedUsing = OnRep_Health)
	FGameplayAttributeData Health;
	ATTRIBUTE_ACCESSORS(UDemoAttributeSet, Health)

	UPROPERTY(BlueprintReadOnly, Category = "Demo|Vital", ReplicatedUsing = OnRep_MaxHealth)
	FGameplayAttributeData MaxHealth;
	ATTRIBUTE_ACCESSORS(UDemoAttributeSet, MaxHealth)

	// ---------- 耐力（格挡 / 闪避 / 技能消耗） ----------
	UPROPERTY(BlueprintReadOnly, Category = "Demo|Vital", ReplicatedUsing = OnRep_Stamina)
	FGameplayAttributeData Stamina;
	ATTRIBUTE_ACCESSORS(UDemoAttributeSet, Stamina)

	UPROPERTY(BlueprintReadOnly, Category = "Demo|Vital", ReplicatedUsing = OnRep_MaxStamina)
	FGameplayAttributeData MaxStamina;
	ATTRIBUTE_ACCESSORS(UDemoAttributeSet, MaxStamina)

	// ---------- 韧性（归零触发破防） ----------
	UPROPERTY(BlueprintReadOnly, Category = "Demo|Combat", ReplicatedUsing = OnRep_Poise)
	FGameplayAttributeData Poise;
	ATTRIBUTE_ACCESSORS(UDemoAttributeSet, Poise)

	UPROPERTY(BlueprintReadOnly, Category = "Demo|Combat", ReplicatedUsing = OnRep_MaxPoise)
	FGameplayAttributeData MaxPoise;
	ATTRIBUTE_ACCESSORS(UDemoAttributeSet, MaxPoise)

	// ---------- 双异常条（对标参考 Demo：Boss 血条下方两条独立积累槽） ----------
	// 为什么是两条而不是一条：
	//   一条槽只能表达"打满了"，两条才能表达"用什么手段打满"——
	//   冲击槽（橙）由近战连段/削韧类手段喂养，元素槽（青）由弹反/技能/状态效果喂养。
	//   玩家于是能读出"我这套连招在推进哪一条"，而不是盯着一根不知道怎么涨的条。
	//
	// 规则（与参考 Demo 一致）：
	//   冲击槽满 → 失衡 State_Unstable（轮廓变绿 + 受伤加成），Boss 仍能行动；
	//   元素槽满 且 已失衡 → 击破 State_Broken（失去格挡 + 虚弱窗口）。
	// 两条槽都是"满了就清零重来"，所以是节奏资源而不是一次性资源。
	UPROPERTY(BlueprintReadOnly, Category = "Demo|Combat", ReplicatedUsing = OnRep_AnomalyImpact)
	FGameplayAttributeData AnomalyImpact;
	ATTRIBUTE_ACCESSORS(UDemoAttributeSet, AnomalyImpact)

	UPROPERTY(BlueprintReadOnly, Category = "Demo|Combat", ReplicatedUsing = OnRep_MaxAnomalyImpact)
	FGameplayAttributeData MaxAnomalyImpact;
	ATTRIBUTE_ACCESSORS(UDemoAttributeSet, MaxAnomalyImpact)

	UPROPERTY(BlueprintReadOnly, Category = "Demo|Combat", ReplicatedUsing = OnRep_AnomalyElement)
	FGameplayAttributeData AnomalyElement;
	ATTRIBUTE_ACCESSORS(UDemoAttributeSet, AnomalyElement)

	UPROPERTY(BlueprintReadOnly, Category = "Demo|Combat", ReplicatedUsing = OnRep_MaxAnomalyElement)
	FGameplayAttributeData MaxAnomalyElement;
	ATTRIBUTE_ACCESSORS(UDemoAttributeSet, MaxAnomalyElement)

	// ---------- 架势（玩家的"还能格挡多久"资源）----------
	// 与韧性(Poise)职责分离，不要混用：
	//   韧性 = 被打削，归零触发【破防虚弱】，是"敌人能把你怎么样"
	//   架势 = 格挡消耗，归零触发【被破防】（硬直 + 吃部分伤害），是"你能挡多久"
	// 参考 Demo 的对应机制："架势值过低时格挡会被 Boss 破防，进入硬直且受到部分伤害"。
	// 于是"一直举盾"不再是免费策略 —— 架势条逼玩家在【挡】和【弹/闪】之间做选择，
	// 这正是把防御手段也接进反馈链的那一步。
	UPROPERTY(BlueprintReadOnly, Category = "Demo|Combat", ReplicatedUsing = OnRep_Posture)
	FGameplayAttributeData Posture;
	ATTRIBUTE_ACCESSORS(UDemoAttributeSet, Posture)

	UPROPERTY(BlueprintReadOnly, Category = "Demo|Combat", ReplicatedUsing = OnRep_MaxPosture)
	FGameplayAttributeData MaxPosture;
	ATTRIBUTE_ACCESSORS(UDemoAttributeSet, MaxPosture)

	// ---------- 攻防 ----------
	UPROPERTY(BlueprintReadOnly, Category = "Demo|Combat", ReplicatedUsing = OnRep_AttackPower)
	FGameplayAttributeData AttackPower;
	ATTRIBUTE_ACCESSORS(UDemoAttributeSet, AttackPower)

	UPROPERTY(BlueprintReadOnly, Category = "Demo|Combat", ReplicatedUsing = OnRep_DefensePower)
	FGameplayAttributeData DefensePower;
	ATTRIBUTE_ACCESSORS(UDemoAttributeSet, DefensePower)

	// ---------- 移速（供减速类状态效果使用） ----------
	UPROPERTY(BlueprintReadOnly, Category = "Demo|Movement", ReplicatedUsing = OnRep_MoveSpeed)
	FGameplayAttributeData MoveSpeed;
	ATTRIBUTE_ACCESSORS(UDemoAttributeSet, MoveSpeed)

	// ---------- 广播 ----------
	FDemoOnAttributeChanged OnHealthChanged;
	FDemoOnAttributeChanged OnStaminaChanged;
	FDemoOnAttributeChanged OnPoiseChanged;
	FDemoOnPoiseBroken OnPoiseBroken;
	FDemoOnDeath OnDeath;

	// 双异常条变化（供 HUD 的两条槽读）
	FDemoOnAttributeChanged OnAnomalyImpactChanged;
	FDemoOnAttributeChanged OnAnomalyElementChanged;

	// 冲击槽打满 → 失衡。UI 标签 + 材质变色两条通道都从这里出发（状态信号规范）。
	FDemoOnPoiseBroken OnAnomalyUnstable;
	// 元素槽在失衡状态下打满 → 击破（失去格挡 + 虚弱窗口）
	FDemoOnPoiseBroken OnAnomalyBroken;

	// 架势变化（供 HUD 的架势条读）
	FDemoOnAttributeChanged OnPostureChanged;
	// 架势被打空 → 被破防（举盾方进入硬直 + 吃部分伤害）
	FDemoOnPoiseBroken OnPostureBroken;

	// 受到伤害（含格挡 / 破防暴击等判定结果）
	FDemoOnDamageTaken OnDamageTaken;

protected:
	UFUNCTION() void OnRep_Health(const FGameplayAttributeData& OldValue) const;
	UFUNCTION() void OnRep_MaxHealth(const FGameplayAttributeData& OldValue) const;
	UFUNCTION() void OnRep_Stamina(const FGameplayAttributeData& OldValue) const;
	UFUNCTION() void OnRep_MaxStamina(const FGameplayAttributeData& OldValue) const;
	UFUNCTION() void OnRep_Poise(const FGameplayAttributeData& OldValue) const;
	UFUNCTION() void OnRep_MaxPoise(const FGameplayAttributeData& OldValue) const;
	UFUNCTION() void OnRep_Posture(const FGameplayAttributeData& OldValue) const;
	UFUNCTION() void OnRep_MaxPosture(const FGameplayAttributeData& OldValue) const;
	UFUNCTION() void OnRep_AnomalyImpact(const FGameplayAttributeData& OldValue) const;
	UFUNCTION() void OnRep_MaxAnomalyImpact(const FGameplayAttributeData& OldValue) const;
	UFUNCTION() void OnRep_AnomalyElement(const FGameplayAttributeData& OldValue) const;
	UFUNCTION() void OnRep_MaxAnomalyElement(const FGameplayAttributeData& OldValue) const;
	UFUNCTION() void OnRep_AttackPower(const FGameplayAttributeData& OldValue) const;
	UFUNCTION() void OnRep_DefensePower(const FGameplayAttributeData& OldValue) const;
	UFUNCTION() void OnRep_MoveSpeed(const FGameplayAttributeData& OldValue) const;
};
