#include "Boss/BossAIController.h"
#include "Boss/BossCharacterBase.h"
#include "Boss/BossBrainComponent.h"

ABossAIController::ABossAIController()
{
	// 让控制器朝向由移动决定，出招时 Brain 会手动转向玩家
	bSetControlRotationFromPawnOrientation = false;
}

void ABossAIController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);

	if (ABossCharacterBase* Boss = Cast<ABossCharacterBase>(InPawn))
	{
		if (UBossBrainComponent* Brain = Boss->GetBrain())
		{
			// 由大脑驱动，这里只保证寻路可用
			Brain->SetIsReplicated(false);
		}
	}
}
