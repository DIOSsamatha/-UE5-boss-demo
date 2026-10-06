// UE 5.7: FInheritedTagContainer::GetGameplayTagContainer() 已被标记为废弃 (C4996)，
// 但引擎内部的 SetAndApply* 组件 API 仍在调用它，且新 API (SetAssetTags) 仅限构造函数使用，
// 与 UE 5.7 "禁止构造函数内 FindOrAddComponent" 的规则冲突。
// 此处抑制该废弃警告是合理的变通方案。
#pragma warning(push)
#pragma warning(disable: 4996)

#include "Effects/DemoGameplayEffectHelpers.h"
#include "Effects/DemoGameplayEffects.h"

UDemoGameplayEffectBase::UDemoGameplayEffectBase()
{
}

void UDemoGameplayEffectBase::PostInitProperties()
{
	Super::PostInitProperties();
	ApplyComponentConfig();
}

void UDemoGameplayEffectBase::ApplyComponentConfig()
{
	// GrantedTags
	{
		FInheritedTagContainer Tags;
		ConfigureGrantedTags(Tags);
		FindOrAddComponent<UTargetTagsGameplayEffectComponent>().SetAndApplyTargetTagChanges(Tags);
	}

	// BlockedAbilities
	{
		FInheritedTagContainer Tags;
		ConfigureBlockedAbilities(Tags);
		FindOrAddComponent<UBlockAbilityTagsGameplayEffectComponent>().SetAndApplyBlockedAbilityTagChanges(Tags);
	}

	// CanceledAbilities
	{
		FInheritedTagContainer Tags;
		CanceledAbilityOnApply(Tags);
		FindOrAddComponent<UCancelAbilityTagsGameplayEffectComponent>()
			.SetAndApplyCanceledAbilityTagChanges(Tags, FInheritedTagContainer());
	}
}

#pragma warning(pop)
