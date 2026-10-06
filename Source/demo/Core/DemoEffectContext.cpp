#include "Core/DemoEffectContext.h"

bool FDemoGameplayEffectContext::NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess)
{
	if (!FGameplayEffectContext::NetSerialize(Ar, Map, bOutSuccess))
	{
		return false;
	}

	Ar << HitDirection;
	Ar << PoiseDamage;

	uint8 Flags = 0;
	if (Ar.IsSaving())
	{
		Flags |= bCanBeParried ? 1u : 0u;
		Flags |= bCanBeBlocked ? 2u : 0u;
		Flags |= bIsCritical ? 4u : 0u;
	}
	Ar << Flags;

	if (Ar.IsLoading())
	{
		bCanBeParried = (Flags & 1u) != 0;
		bCanBeBlocked = (Flags & 2u) != 0;
		bIsCritical = (Flags & 4u) != 0;
	}

	// 结果类型 + 伤害来源各占 4 bit，塞进同一个字节（枚举值都很小）
	uint8 ResultAndSource = 0;
	if (Ar.IsSaving())
	{
		ResultAndSource = static_cast<uint8>(static_cast<uint8>(HitResultType) & 0x0Fu);
		ResultAndSource |= static_cast<uint8>((static_cast<uint8>(DamageSource) & 0x0Fu) << 4);
	}
	Ar << ResultAndSource;

	if (Ar.IsLoading())
	{
		HitResultType = static_cast<EDemoHitResultType>(ResultAndSource & 0x0Fu);
		DamageSource = static_cast<EDemoDamageSource>((ResultAndSource >> 4) & 0x0Fu);
	}

	bOutSuccess = true;
	return true;
}
