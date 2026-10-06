// 自定义 GameplayEffect 上下文：携带受击方向、可否弹反等战斗专属数据
#pragma once

#include "CoreMinimal.h"
#include "GameplayEffectTypes.h"
#include "Engine/NetSerialization.h"
#include "DemoCommon.h"
#include "DemoEffectContext.generated.h"

USTRUCT(BlueprintType)
struct DEMO_API FDemoGameplayEffectContext : public FGameplayEffectContext
{
	GENERATED_BODY()

public:
	FDemoGameplayEffectContext()
		: FGameplayEffectContext()
	{}

	virtual UScriptStruct* GetScriptStruct() const override
	{
		return FDemoGameplayEffectContext::StaticStruct();
	}

	virtual FGameplayEffectContext* Duplicate() const override
	{
		FDemoGameplayEffectContext* NewContext = new FDemoGameplayEffectContext();
		*NewContext = *this;
		if (GetHitResult())
		{
			NewContext->AddHitResult(*GetHitResult(), true);
		}
		return NewContext;
	}

	virtual bool NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess) override;

	// 受击方向（世界空间，已归一化），用于挑选受击动画
	UPROPERTY(BlueprintReadWrite, Category = "Demo|Combat")
	FVector_NetQuantizeNormal HitDirection = FVector_NetQuantizeNormal(ForceInit);

	// 本次攻击是否可被弹反（重击/红色招式一般不可）
	UPROPERTY(BlueprintReadWrite, Category = "Demo|Combat")
	bool bCanBeParried = false;

	// 本次攻击是否可被普通格挡挡下
	UPROPERTY(BlueprintReadWrite, Category = "Demo|Combat")
	bool bCanBeBlocked = true;

	// 是否暴击（背后攻击 / 破防状态下命中）
	UPROPERTY(BlueprintReadWrite, Category = "Demo|Combat")
	bool bIsCritical = false;

	// 附带的削韧值
	UPROPERTY(BlueprintReadWrite, Category = "Demo|Combat")
	float PoiseDamage = 0.f;

	// 本次命中被判定成了什么（普通 / 格挡 / 弹反 / 闪避 / 暴击）。
	//
	// 为什么放在上下文里而不是直接塞给受击者：
	//   伤害是"来源方 ApplyHit 判定 → 目标方 AttributeSet 执行 GE"两段式，
	//   结果是在来源方算出来的、在目标方生效的。上下文就是这两段之间唯一的信封，
	//   所以它同时也是 UI 拿到"这一下被格挡了"的唯一途径。
	UPROPERTY(BlueprintReadWrite, Category = "Demo|Combat")
	EDemoHitResultType HitResultType = EDemoHitResultType::Normal;

	// 伤害来源（近战 / 弹幕 / 落地冲击 / 大招），只用于表现层
	UPROPERTY(BlueprintReadWrite, Category = "Demo|Combat")
	EDemoDamageSource DamageSource = EDemoDamageSource::Unknown;
};

template<>
struct TStructOpsTypeTraits<FDemoGameplayEffectContext> : public TStructOpsTypeTraitsBase2<FDemoGameplayEffectContext>
{
	enum
	{
		WithNetSerializer = true,
		WithCopy = true
	};
};
