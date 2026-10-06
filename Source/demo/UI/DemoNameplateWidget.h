// 敌人头顶的浮动血条（nameplate）
//
// 由 ADemoCharacterBase 上的 UWidgetComponent 承载：挂点、尺寸、显示与否都在角色上配，
// 这里只负责"画一条能跟着血量变的条"。
//
// 和 UDemoStatusBarWidget 的分工：那个是给 HUD 用的宽条（带数值文字、延迟掉血），
// 这个是贴在角色头上的窄条（只有名字 + 血量，读起来要一眼扫到）。
#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "DemoNameplateWidget.generated.h"

class ADemoCharacterBase;
class UBorder;
class UOverlay;
class UProgressBar;
class USizeBox;
class UTextBlock;
class UVerticalBox;

UCLASS()
class DEMO_API UDemoNameplateWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	// ---------- 外观 ----------
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|UI")
	FVector2D PanelSize = FVector2D(190.f, 34.f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|UI")
	float BarHeight = 8.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|UI")
	FLinearColor FillColor = FLinearColor(0.80f, 0.16f, 0.13f, 1.f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|UI")
	FLinearColor BackColor = FLinearColor(0.f, 0.f, 0.f, 0.72f);

	// 二阶段（狂化）时血条换成这个颜色
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|UI")
	FLinearColor Phase2FillColor = FLinearColor(0.95f, 0.55f, 0.08f, 1.f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|UI")
	int32 NameFontSize = 12;

	// 满血时自动隐藏（小怪用；BOSS 想一直看到就设 false）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|UI")
	bool bHideWhenFullHealth = false;

	// 从 HUD 隐藏（例如 BOSS 死亡结算时统一关掉）
	UFUNCTION(BlueprintCallable, Category = "Demo|UI")
	void BindToCharacter(ADemoCharacterBase* InCharacter);

	UFUNCTION(BlueprintCallable, Category = "Demo|UI")
	void SetNameText(const FText& InName);

	// 直接给数值：内部自动算比例 + 处理"满血隐藏"
	UFUNCTION(BlueprintCallable, Category = "Demo|UI")
	void SetHealthValue(float NewHealth, float MaxHealth);

	UFUNCTION(BlueprintCallable, Category = "Demo|UI")
	void SetPhaseHighlight(bool bPhase2);

	UFUNCTION(BlueprintCallable, Category = "Demo|UI")
	void SetHiddenByRule(bool bHidden);

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	void EnsureDefaultTree();
	void ApplyVisibility();

	UPROPERTY(Transient) TObjectPtr<USizeBox> RootSizeBox;
	UPROPERTY(Transient) TObjectPtr<UVerticalBox> RootBox;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> NameText;
	UPROPERTY(Transient) TObjectPtr<UBorder> BackBorder;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> GhostBar;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> FillBar;

	UPROPERTY(BlueprintReadOnly, Category = "Demo|UI")
	TObjectPtr<ADemoCharacterBase> BoundCharacter;

private:
	float TargetPercent = 1.f;
	float GhostPercent = 1.f;
	bool bTreeReady = false;
	bool bHiddenByRule = false;
};
