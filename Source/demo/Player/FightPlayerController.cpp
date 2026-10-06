#include "Player/FightPlayerController.h"

AFightPlayerController::AFightPlayerController()
{
	bStartWithMouseCursor = false;
}

void AFightPlayerController::BeginPlay()
{
	Super::BeginPlay();

	FInputModeGameOnly InputMode;
	SetInputMode(InputMode);
	SetShowMouseCursor(bStartWithMouseCursor);
}
