// 所有技能的基类：封装常用取用接口，统一在激活时施加自带 GE
#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "DemoGameplayAbility.generated.h"

class UDemoAttributeSet;
class UGameplayEffect;

UCLASS()
class DEMO_API UDemoGameplayAbility : public UGameplayAbility
{
	GENERATED_BODY()

public:
	UDemoGameplayAbility();

	// 技能激活时自动施加给自己的 GE（例如格挡状态、无敌帧、消耗）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Ability")
	TArray<TSubclassOf<UGameplayEffect>> EffectsToApplyOnActivate;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|Ability")
	ACharacter* GetAvatarCharacter() const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|Ability")
	UAbilitySystemComponent* GetAvatarASC() const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|Ability")
	UDemoAttributeSet* GetAvatarAttributeSet() const;

	UFUNCTION(BlueprintCallable, Category = "Demo|Ability")
	FActiveGameplayEffectHandle ApplyEffectToSelf(TSubclassOf<UGameplayEffect> EffectClass);

	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;
};
