// 通用「能量拖尾」组件 —— 弹幕弹道 / 突进斩拖刀 / 蓄力危险光效 共用
//
// 为什么做成组件而不是写进 ADemoProjectile：
//   1) 弹幕要拖尾，突进斩也要拖尾，大招蓄力还想发光 —— 同一套东西出现三次就该抽出来；
//   2) 拖尾的实现细节（实例化网格 + 每实例颜色）换个技能完全不用改。
//
// 实现方式（重要，决定了它为什么便宜）：
//   整条尾巴是一个 UInstancedStaticMeshComponent 的 N 个实例。
//   - 每个实例的位置 = 记录下来的历史轨迹点（按「前进多少厘米」采样，不是按帧）
//   - 每个实例的大小 = 头大尾小（锥形）
//   - 每个实例的亮度/颜色 = 走「每实例自定义数据」（NumCustomDataFloats = 3，对应 RGB）
//   所以一个 40 发弹幕的大招也只多 40 个组件，而不是 40 × 12 个。
//
// ⚠ 材质约定：材质里用 PerInstanceCustomData3Vector(DataIndex = 0) 乘进自发光，
//   否则所有实例会一样亮（尾巴不会淡出）—— 只是难看，不会崩。
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DemoTrailComponent.generated.h"

class UInstancedStaticMeshComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UStaticMesh;

UCLASS(ClassGroup = (Demo), meta = (BlueprintSpawnableComponent))
class DEMO_API UDemoTrailComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UDemoTrailComponent();

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// 开关：false 立刻清掉（弹体命中 / 销毁时用）
	UFUNCTION(BlueprintCallable, Category = "Demo|Trail")
	void SetTrailEnabled(bool bEnabled);

	// 停止采样，已有残影按 FadeOutTime 淡出。
	// 突进斩冲完必须调这个：否则停下之后尾巴会被"吸"回身上堆成一坨。
	UFUNCTION(BlueprintCallable, Category = "Demo|Trail")
	void StopTrail();

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|Trail")
	bool IsTrailActive() const { return bTrailEnabled; }

	// 一键配置成"快速小弹丸"或"重型拖刀"这类预设手感的参数组
	UFUNCTION(BlueprintCallable, Category = "Demo|Trail")
	void ApplyPresetProjectile(float InHeadSize, FLinearColor InColor, float InBrightness);

	UFUNCTION(BlueprintCallable, Category = "Demo|Trail")
	void ApplyPresetDash(float InHeadSize, FLinearColor InColor, float InBrightness);

	// 手动指定颜色 / 亮度（不改其它参数）
	UFUNCTION(BlueprintCallable, Category = "Demo|Trail")
	void SetTrailColor(FLinearColor InColor, float InBrightness = -1.f);

	// ---------------- 可调参数（生成方在 BeginPlay 之前直接改属性即可）----------------

	// 残影段数。越多尾巴越长，但每帧要写的数据也越多
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|Trail")
	int32 SampleCount = 12;

	// 前进多少厘米记录一个残影点。速度越快这个值该越大（弹幕 14，突进 40）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|Trail")
	float SampleDistance = 14.f;

	// 单次位移超过这个距离视为"瞬移"（生成首帧 / 传送），直接丢掉历史，
	// 否则会从原点拉出一条横穿地图的长尾
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|Trail")
	float MaxSampleSnapDistance = 260.f;

	// 头部残影直径（cm）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|Trail")
	float HeadSize = 46.f;

	// 尾部残影相对头部的大小（0~1）→ 锥形尾巴
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|Trail")
	float TailSizeScale = 0.28f;

	// 尾部亮度相对头部（0~1）→ 尾巴淡出去
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|Trail")
	float TailBrightness = 0.06f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|Trail")
	FLinearColor TrailColor = FLinearColor(0.35f, 0.72f, 1.f, 1.f);

	// 自发光倍率。加法混合 + HDR，>1 才会吃到 Bloom，尾巴才有"发光"感
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|Trail")
	float Brightness = 8.f;

	// 拖尾材质。留空 → 按约定路径加载；再失败 → 整条尾巴不显示（不崩、不影响玩法）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|Trail")
	TObjectPtr<UMaterialInterface> TrailMaterial;

	// 残影网格。留空 → 引擎球体
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|Trail")
	TObjectPtr<UStaticMesh> TrailMesh;

	// StopTrail 之后多久完全消失（秒）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|Trail")
	float FadeOutTime = 0.16f;

protected:
	void BuildComponents();
	void UpdateTrail(float DeltaTime);
	void PushPoint(const FVector& Location);
	void RefreshInstances(float FadeAlpha);
	void HideAllInstances();

	UPROPERTY()
	TObjectPtr<UInstancedStaticMeshComponent> TrailInstances;

	// 历史轨迹点，[0] 永远是最新的
	TArray<FVector> Points;

private:
	bool bTrailEnabled = true;
	bool bFading = false;
	float FadeRemaining = 0.f;

	// 上次采样的位置：按"走了多远"而不是"过了多久"来决定要不要记点，
	// 这样 300 和 1500 速度的拖尾密度才是一致的
	FVector LastSampleLocation = FVector::ZeroVector;
	bool bHasLastSample = false;

	bool bInstancesReady = false;
};
