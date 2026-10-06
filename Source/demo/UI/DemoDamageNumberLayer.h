// 伤害飘字层
//
// 一个铺满全屏的透明画布：谁挨打了就把数字"贴"在那个世界坐标上往上飘。
// 用屏幕投影（ProjectWorldLocationToScreen）而不是世界空间 Widget：
//   · 同一层里可以混着不同角色的飘字，不用给每个角色挂组件
//   · 数字大小/颜色可以随伤害类型变化（破防暴击更大、被格挡是灰蓝）
//
// 用法：把它当 HUD 根画布的子控件、锚点拉满全屏（UCombatHUDWidget 里已经这么做了）。
#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Core/DemoCommon.h"
#include "DemoDamageNumberLayer.generated.h"

class UCanvasPanel;
class UTextBlock;

UCLASS()
class DEMO_API UDemoDamageNumberLayer : public UUserWidget
{
	GENERATED_BODY()

public:
	// 同时存在的飘字上限（超了就复用最老的，不会无限堆控件）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|UI")
	int32 MaxActiveNumbers = 24;

	// 一条飘字存活多久（秒）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|UI")
	float NumberLifetime = 1.05f;

	// 上飘距离（像素）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|UI")
	float RiseDistance = 80.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|UI")
	int32 FontSize = 20;

	// 水平随机散开范围（像素）—— 连续命中时数字不会完全叠在一起
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|UI")
	float LateralSpread = 26.f;

	// 生成一条飘字。bIsIncoming = true 表示"伤害打在本地玩家身上"（用红色系）
	UFUNCTION(BlueprintCallable, Category = "Demo|UI")
	void SpawnDamageNumber(const FVector& WorldLocation, float Damage, EDemoHitResultType ResultType,
		EDemoDamageSource DamageSource, bool bIsIncoming);

	UFUNCTION(BlueprintCallable, Category = "Demo|UI")
	void ClearAllNumbers();

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	void EnsureDefaultTree();
	UTextBlock* AcquireTextBlock();

	UPROPERTY(Transient) TObjectPtr<UCanvasPanel> RootCanvas;

private:
	// 一条正在飘的数字
	struct FDamageNumber
	{
		TWeakObjectPtr<UTextBlock> Text;
		FVector WorldLocation = FVector::ZeroVector;
		FVector2D Lateral = FVector2D::ZeroVector;
		FLinearColor BaseColor = FLinearColor::White;
		float Elapsed = 0.f;
	};

	// 控件池：飘完不销毁、下次复用，避免每次命中都 NewObject
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> TextPool;

	TArray<FDamageNumber> ActiveNumbers;
	int32 NextPoolIndex = 0;
	bool bTreeReady = false;
};
