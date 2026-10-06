// BOSS 的 AI 控制器：负责寻路跟随，决策由 BossBrainComponent 负责
#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "BossAIController.generated.h"

UCLASS()
class DEMO_API ABossAIController : public AAIController
{
	GENERATED_BODY()

public:
	ABossAIController();

protected:
	virtual void OnPossess(APawn* InPawn) override;
};
