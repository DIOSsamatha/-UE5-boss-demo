// 玩家控制器：目前只负责输入模式与鼠标显示
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "FightPlayerController.generated.h"

UCLASS()
class DEMO_API AFightPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	AFightPlayerController();

protected:
	virtual void BeginPlay() override;

	// 注意：不能叫 bShowMouseCursor，APlayerController 里已有同名成员
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Input")
	bool bStartWithMouseCursor = false;
};
