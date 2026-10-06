// 可复用状态条（血条 / 耐力条 / 韧性条都用它）
//
// 为什么用代码搭控件树，而不是让你在 UMG 里拖：
//   这个项目里"血条"要有 5 处（玩家血/耐力/韧性、BOSS 血/韧性、敌人头顶血条），
//   每处都在 UMG 里手拖一遍既慢又容易配错；而且 WBP_CombatHUD / WBP_BossBar 目前是空的。
//   所以这里在 C++ 里把控件树建出来 —— 已有的 WBP 继承了这个类，打开就能看到条，
//   想改外观仍然可以在子类里覆写 OnStatusBarUpdated / 直接改 UMG 布局。
//
// 视觉结构（三层叠在一起，从下到上）：
//   ┌─────────────── 深色底框 ───────────────┐
//   │ ████████████░░░░  ← 延迟层（白金色）    │  掉血后先停一下再往前追 = "看得见掉了多少"
//   │ ████████████████  ← 当前值层（主题色）  │
//   │            320 / 1600                   │  数值文字（居中，带描边阴影）
//   └─────────────────────────────────────────┘
#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "DemoStatusBarWidget.generated.h"

class UBorder;
class UOverlay;
class UProgressBar;
class USizeBox;
class UTextBlock;

UCLASS()
class DEMO_API UDemoStatusBarWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	// ---------- 外观（建树时读取；蓝图子类里可以直接改默认值）----------
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|UI")
	float BarHeight = 16.f;

	// 当前值层颜色
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|UI")
	FLinearColor FillColor = FLinearColor(0.78f, 0.14f, 0.12f, 1.f);

	// 延迟层颜色（刚掉下去的那一截）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|UI")
	FLinearColor GhostColor = FLinearColor(0.95f, 0.86f, 0.55f, 0.9f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|UI")
	FLinearColor BackColor = FLinearColor(0.02f, 0.02f, 0.03f, 0.78f);

	// 延迟层追赶速度（比例/秒）。<= 0 表示不显示延迟层
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|UI")
	float GhostCatchUpSpeed = 0.45f;

	// 掉血后先停多久再追赶（秒）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|UI")
	float GhostHoldTime = 0.30f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|UI")
	bool bShowValueText = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|UI")
	int32 ValueFontSize = 12;

	// 数值文字自动加 "/最大值"（血条用）。耐力/韧性条一般也开着
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|UI")
	bool bAppendMaxValue = true;

	// ---------- 运行时接口 ----------
	// 设置比例（0~1）。bInstant = true 时延迟层也立刻归位（读档 / 重置用）
	UFUNCTION(BlueprintCallable, Category = "Demo|UI")
	void SetPercent(float InPercent, bool bInstant = false);

	UFUNCTION(BlueprintCallable, Category = "Demo|UI")
	void SetValueAndMax(float InValue, float InMax, bool bInstant = false);

	// 单纯改文字（不比值的时候用）
	UFUNCTION(BlueprintCallable, Category = "Demo|UI")
	void SetLabel(const FText& InLabel);

	UFUNCTION(BlueprintCallable, Category = "Demo|UI")
	void SetBarColors(FLinearColor InFillColor, FLinearColor InGhostColor);

	UFUNCTION(BlueprintCallable, Category = "Demo|UI")
	void SetShowValueText(bool bShow);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|UI")
	float GetPercent() const { return TargetPercent; }

	// 蓝图子类覆写：每帧的最终视觉刷新（想加渐变 / 抖动 / 分段刻度就覆写它）
	UFUNCTION(BlueprintImplementableEvent, Category = "Demo|UI")
	void OnStatusBarUpdated(float NewPercent, float NewGhostPercent);

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativePreConstruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	// 在 C++ 里把控件树建出来（只在控件树为空时执行一次）
	void EnsureDefaultTree();
	void RefreshVisuals();

	UPROPERTY(Transient) TObjectPtr<USizeBox> RootSizeBox;
	UPROPERTY(Transient) TObjectPtr<UBorder> BackBorder;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> GhostBar;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> FillBar;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ValueText;
	UPROPERTY(Transient) TObjectPtr<UOverlay> RootOverlay;

private:
	float TargetPercent = 1.f;
	float GhostPercent = 1.f;
	float GhostHoldRemaining = 0.f;
	bool bTreeReady = false;

	// 有变化才刷新 Slate（HUD 是每帧轮询数值的，不加这个开关会每帧重建一次布局）
	bool bVisualDirty = true;

	// 数值文字：优先用 SetValueAndMax 给的 "当前 / 上限"，其次用 SetLabel 自定义文字
	bool bHasValuePair = false;
	float LastValue = 0.f;
	float LastMax = 0.f;

	FText CachedLabel = FText::GetEmpty();
};
