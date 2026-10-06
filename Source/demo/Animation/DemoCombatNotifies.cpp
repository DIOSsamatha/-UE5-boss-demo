#include "Animation/DemoCombatNotifies.h"
#include "Core/DemoCharacterBase.h"
#include "Core/DemoGameplayTags.h"
#include "Combat/DemoHitStopSubsystem.h"

#include "AbilitySystemComponent.h"

static ADemoCharacterBase* GetDemoCharacter(USkeletalMeshComponent* MeshComp)
{
	return MeshComp ? Cast<ADemoCharacterBase>(MeshComp->GetOwner()) : nullptr;
}

// ---------------------------------------------------------------- 近战判定
void UANS_MeleeHitWindow::NotifyBegin(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float TotalDuration,
	const FAnimNotifyEventReference& EventReference)
{
	if (ADemoCharacterBase* Character = GetDemoCharacter(MeshComp))
	{
		Character->SetMeleeHitWindowEnabled(true);
	}
}

void UANS_MeleeHitWindow::NotifyEnd(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
	const FAnimNotifyEventReference& EventReference)
{
	if (ADemoCharacterBase* Character = GetDemoCharacter(MeshComp))
	{
		Character->SetMeleeHitWindowEnabled(false);
	}
}

// ---------------------------------------------------------------- 连段窗口
void UANS_ComboWindow::NotifyBegin(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float TotalDuration,
	const FAnimNotifyEventReference& EventReference)
{
	if (ADemoCharacterBase* Character = GetDemoCharacter(MeshComp))
	{
		Character->SetComboWindowEnabled(true);
	}
}

void UANS_ComboWindow::NotifyEnd(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
	const FAnimNotifyEventReference& EventReference)
{
	if (ADemoCharacterBase* Character = GetDemoCharacter(MeshComp))
	{
		Character->SetComboWindowEnabled(false);
	}
}

// ---------------------------------------------------------------- 无敌帧
void UANS_InvincibleWindow::NotifyBegin(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float TotalDuration,
	const FAnimNotifyEventReference& EventReference)
{
	if (ADemoCharacterBase* Character = GetDemoCharacter(MeshComp))
	{
		if (UAbilitySystemComponent* ASC = Character->GetAbilitySystemComponent())
		{
			ASC->AddLooseGameplayTag(DemoTag::State_Invincible, 1);
		}
	}
}

void UANS_InvincibleWindow::NotifyEnd(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
	const FAnimNotifyEventReference& EventReference)
{
	if (ADemoCharacterBase* Character = GetDemoCharacter(MeshComp))
	{
		if (UAbilitySystemComponent* ASC = Character->GetAbilitySystemComponent())
		{
			ASC->RemoveLooseGameplayTag(DemoTag::State_Invincible, 1);
		}
	}
}

// ---------------------------------------------------------------- 弹反窗口
void UANS_ParryWindow::NotifyBegin(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float TotalDuration,
	const FAnimNotifyEventReference& EventReference)
{
	if (ADemoCharacterBase* Character = GetDemoCharacter(MeshComp))
	{
		if (UAbilitySystemComponent* ASC = Character->GetAbilitySystemComponent())
		{
			ASC->AddLooseGameplayTag(DemoTag::State_Parrying, 1);
		}
	}
}

void UANS_ParryWindow::NotifyEnd(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
	const FAnimNotifyEventReference& EventReference)
{
	if (ADemoCharacterBase* Character = GetDemoCharacter(MeshComp))
	{
		if (UAbilitySystemComponent* ASC = Character->GetAbilitySystemComponent())
		{
			ASC->RemoveLooseGameplayTag(DemoTag::State_Parrying, 1);
		}
	}
}

// ---------------------------------------------------------------- 顿帧
void UAN_HitStop::Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
	const FAnimNotifyEventReference& EventReference)
{
	if (!MeshComp)
	{
		return;
	}

	if (UWorld* World = MeshComp->GetWorld())
	{
		if (UDemoHitStopSubsystem* HitStop = World->GetSubsystem<UDemoHitStopSubsystem>())
		{
			HitStop->RequestHitStop(Duration, TimeDilation);
		}
	}
}
