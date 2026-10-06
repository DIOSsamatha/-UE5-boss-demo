#include "Abilities/DemoGameplayAbility.h"
#include "Core/DemoAttributeSet.h"
#include "Core/DemoAbilitySystemLibrary.h"
#include "GameFramework/Character.h"

UDemoGameplayAbility::UDemoGameplayAbility()
{
	// demo 阶段用本地预测即可，Instanced 保证每个实例有独立状态（连段需要）
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	ReplicationPolicy = EGameplayAbilityReplicationPolicy::ReplicateNo;
}

void UDemoGameplayAbility::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	Super::ActivateAbility(Handle, ActorInfo, ActivationInfo, TriggerEventData);

	for (const TSubclassOf<UGameplayEffect>& EffectClass : EffectsToApplyOnActivate)
	{
		if (EffectClass)
		{
			ApplyEffectToSelf(EffectClass);
		}
	}
}

ACharacter* UDemoGameplayAbility::GetAvatarCharacter() const
{
	if (!CurrentActorInfo)
	{
		return nullptr;
	}
	return Cast<ACharacter>(CurrentActorInfo->AvatarActor.Get());
}

UAbilitySystemComponent* UDemoGameplayAbility::GetAvatarASC() const
{
	if (!CurrentActorInfo)
	{
		return nullptr;
	}
	return CurrentActorInfo->AbilitySystemComponent.Get();
}

UDemoAttributeSet* UDemoGameplayAbility::GetAvatarAttributeSet() const
{
	UAbilitySystemComponent* ASC = GetAvatarASC();
	if (!ASC)
	{
		return nullptr;
	}
	return const_cast<UDemoAttributeSet*>(ASC->GetSet<UDemoAttributeSet>());
}

FActiveGameplayEffectHandle UDemoGameplayAbility::ApplyEffectToSelf(TSubclassOf<UGameplayEffect> EffectClass)
{
	AActor* Avatar = CurrentActorInfo ? CurrentActorInfo->AvatarActor.Get() : nullptr;
	return UDemoAbilitySystemLibrary::ApplyEffectToSelf(Avatar, EffectClass);
}
