// 状态效果 GE 集合 —— 后续扩展新效果时，复制任意一个类的写法即可
// 每个效果只需做两件事：1) ConfigureGrantedTags 挂上自己的状态 Tag  2) 在构造函数配 Modifier
//
// 所有状态效果继承 UDemoGameplayEffectBase（避免 UE 5.7 构造函数内 FindOrAddComponent 崩溃）
#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "Effects/DemoGameplayEffects.h"
#include "DemoStatusEffects.generated.h"

// 燃烧：周期性掉血
UCLASS(BlueprintType)
class DEMO_API UGE_BurnEffect : public UDemoGameplayEffectBase
{
	GENERATED_BODY()
public:
	UGE_BurnEffect();
	virtual void ConfigureGrantedTags(FInheritedTagContainer& OutTags) override;
};

// 中毒：周期性掉血 + 削韧
UCLASS(BlueprintType)
class DEMO_API UGE_PoisonEffect : public UDemoGameplayEffectBase
{
	GENERATED_BODY()
public:
	UGE_PoisonEffect();
	virtual void ConfigureGrantedTags(FInheritedTagContainer& OutTags) override;
};

// 流血：持续期间受到的伤害额外提升（判定在 UDemoDamageExecution 里）
UCLASS(BlueprintType)
class DEMO_API UGE_BleedEffect : public UDemoGameplayEffectBase
{
	GENERATED_BODY()
public:
	UGE_BleedEffect();
	virtual void ConfigureGrantedTags(FInheritedTagContainer& OutTags) override;
};

// 易伤：受到伤害提升 35%
UCLASS(BlueprintType)
class DEMO_API UGE_VulnerableEffect : public UDemoGameplayEffectBase
{
	GENERATED_BODY()
public:
	UGE_VulnerableEffect();
	virtual void ConfigureGrantedTags(FInheritedTagContainer& OutTags) override;
};

// 减速：移速 x0.5
UCLASS(BlueprintType)
class DEMO_API UGE_SlowEffect : public UDemoGameplayEffectBase
{
	GENERATED_BODY()
public:
	UGE_SlowEffect();
	virtual void ConfigureGrantedTags(FInheritedTagContainer& OutTags) override;
};

// 攻击提升：攻击力 +15
UCLASS(BlueprintType)
class DEMO_API UGE_AttackUpEffect : public UDemoGameplayEffectBase
{
	GENERATED_BODY()
public:
	UGE_AttackUpEffect();
	virtual void ConfigureGrantedTags(FInheritedTagContainer& OutTags) override;
};

// 防御提升：防御力 +20
UCLASS(BlueprintType)
class DEMO_API UGE_DefenseUpEffect : public UDemoGameplayEffectBase
{
	GENERATED_BODY()
public:
	UGE_DefenseUpEffect();
	virtual void ConfigureGrantedTags(FInheritedTagContainer& OutTags) override;
};
