#include "Core/DemoAbilitySystemGlobals.h"
#include "Core/DemoEffectContext.h"

FGameplayEffectContext* UDemoAbilitySystemGlobals::AllocGameplayEffectContext() const
{
	return new FDemoGameplayEffectContext();
}
