// UE 5.7 的 GameplayEffect 已组件化：GrantedTags / BlockAbilityTags / CancelAbilitiesWithTag
// 这些老字段在 5.3 起被拆到独立组件里。
//
// 重要：UE 5.7 禁止在构造函数内调用 FindOrAddComponent（内部用 NewObject 无名创建）。
//      因此所有组件配置必须延迟到 PostInitProperties() 中执行。
#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "GameplayEffect.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/BlockAbilityTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/CancelAbilityTagsGameplayEffectComponent.h"

namespace DemoGE
{
	// 便捷辅助：构建 FInheritedTagContainer（供子类 ConfigureXxx 使用）
	inline FInheritedTagContainer MakeTagContainer(const TArray<FGameplayTag>& Tags)
	{
		FInheritedTagContainer Container;
		for (const FGameplayTag& Tag : Tags)
		{
			if (Tag.IsValid())
			{
				Container.Added.AddTag(Tag);
			}
		}
		return Container;
	}
}
