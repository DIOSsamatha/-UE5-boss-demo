// 让 GAS 使用我们自定义的 FDemoGameplayEffectContext
// 需要在 DefaultGame.ini 里配置 AbilitySystemGlobalsClassName=/Script/demo.DemoAbilitySystemGlobals
#pragma once

#include "CoreMinimal.h"
#include "AbilitySystemGlobals.h"
#include "DemoAbilitySystemGlobals.generated.h"

UCLASS()
class DEMO_API UDemoAbilitySystemGlobals : public UAbilitySystemGlobals
{
	GENERATED_BODY()

public:
	virtual FGameplayEffectContext* AllocGameplayEffectContext() const override;
};
