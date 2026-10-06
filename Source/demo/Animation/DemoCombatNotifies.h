// 战斗用动画通知 —— 你在 Montage 时间轴上拖这几个通知，就能把动画和战斗逻辑接起来
// MeleeHitWindow   : 判定开启区间（挥剑真正能砍到人的那段）
// ComboWindow      : 连段输入缓冲区间（这段内再按攻击键会接下一段）
// InvincibleWindow : 无敌帧（闪避用）
// ParryWindow      : 弹反窗口（防守起手的那几帧）
// HitStop          : 顿帧（放在刀砍中的那一帧）
#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "DemoCombatNotifies.generated.h"

// ---------------------------------------------------------------- 近战判定窗口
UCLASS()
class DEMO_API UANS_MeleeHitWindow : public UAnimNotifyState
{
	GENERATED_BODY()

public:
	virtual void NotifyBegin(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float TotalDuration,
		const FAnimNotifyEventReference& EventReference) override;
	virtual void NotifyEnd(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
		const FAnimNotifyEventReference& EventReference) override;
	virtual FString GetNotifyName_Implementation() const override { return TEXT("MeleeHit"); }
};

// ---------------------------------------------------------------- 连段窗口
UCLASS()
class DEMO_API UANS_ComboWindow : public UAnimNotifyState
{
	GENERATED_BODY()

public:
	virtual void NotifyBegin(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float TotalDuration,
		const FAnimNotifyEventReference& EventReference) override;
	virtual void NotifyEnd(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
		const FAnimNotifyEventReference& EventReference) override;
	virtual FString GetNotifyName_Implementation() const override { return TEXT("Combo"); }
};

// ---------------------------------------------------------------- 无敌帧
UCLASS()
class DEMO_API UANS_InvincibleWindow : public UAnimNotifyState
{
	GENERATED_BODY()

public:
	virtual void NotifyBegin(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float TotalDuration,
		const FAnimNotifyEventReference& EventReference) override;
	virtual void NotifyEnd(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
		const FAnimNotifyEventReference& EventReference) override;
	virtual FString GetNotifyName_Implementation() const override { return TEXT("Invincible"); }
};

// ---------------------------------------------------------------- 弹反窗口
UCLASS()
class DEMO_API UANS_ParryWindow : public UAnimNotifyState
{
	GENERATED_BODY()

public:
	virtual void NotifyBegin(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float TotalDuration,
		const FAnimNotifyEventReference& EventReference) override;
	virtual void NotifyEnd(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
		const FAnimNotifyEventReference& EventReference) override;
	virtual FString GetNotifyName_Implementation() const override { return TEXT("Parry"); }
};

// ---------------------------------------------------------------- 顿帧
UCLASS()
class DEMO_API UAN_HitStop : public UAnimNotify
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo")
	float Duration = 0.08f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo")
	float TimeDilation = 0.02f;

	virtual void Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
		const FAnimNotifyEventReference& EventReference) override;
	virtual FString GetNotifyName_Implementation() const override { return TEXT("HitStop"); }
};
