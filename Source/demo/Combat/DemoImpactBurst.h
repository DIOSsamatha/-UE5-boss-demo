// 命中爆点 —— 弹幕打中东西 / 重招砸地时的"闪一下就散"的表现
//
// 为什么用代码做而不是 Niagara：
//   1) 这个项目完全没有 Niagara 资产，用 C++ 做能保证"零配置就有特效"；
//   2) 只有三层（中心闪光 + 扩散环 + 点光），状态机简单到不值得开一个 Niagara 系统。
//
// 三层各管一件事：
//   Flash   —— 中心爆闪，0.1s 内缩到最小最亮，然后迅速消失（"炸"的那一下）
//   Ring    —— 贴地扩散环（对齐命中法线），把冲击的"范围"读出来
//   Light   —— 点光一闪，让周围环境真的被照亮（没有它，特效看起来像贴图贴上去的）
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DemoImpactBurst.generated.h"

class UStaticMeshComponent;
class UPointLightComponent;
class UMaterialInterface;

UCLASS()
class DEMO_API ADemoImpactBurst : public AActor
{
	GENERATED_BODY()

public:
	ADemoImpactBurst();

	// 统一生成入口。BurstClass 为空时用原生 ADemoImpactBurst。
	UFUNCTION(BlueprintCallable, Category = "Demo|VFX", meta = (WorldContext = "WorldContextObject"))
	static ADemoImpactBurst* SpawnBurst(
		UObject* WorldContextObject,
		const FVector& Location,
		const FVector& SurfaceNormal,
		FLinearColor Color,
		float Radius,
		float Duration,
		bool bSpawnLight = true,
		TSubclassOf<ADemoImpactBurst> BurstClass = nullptr,
		// 是否显示中心那个球体爆闪（引擎基础球体网格）。
		// 弹反 / 拼刀传 false：那两处要的是"金属相击的一圈冲击 + 一瞬的照亮"，
		// 中心摆一个会缩放的球会读成"凭空冒出个小球"，反而脏。
		bool bUseFlash = true,
		// 是否显示扩散环（那张 100cm 的 Plane，会被放到几米大）。
		// ⚠ 2026-10-06：弹反 / 拼刀改传 false —— 玩家反馈"被攻击后稳心会出现一块灰色的板"。
		//   算过尺寸：环的最终边长 = Radius × RingEndScale(2.6) × 2，弹反 Radius=70
		//   → 364cm 的平面，而生成点就在玩家眼前 1~2m（两人之间），HDR 强度又到 7.7
		//   → 一张大尺寸加法混合的平面糊在镜头前，就是那块"板"。
		//   弹反的读点交给点光 + 顿帧 + 镜头冲击 + 飘字，不再往脸上糊平面。
		bool bUseRing = true);

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

	// ---- 可调参数（蓝图子类里改，或生成前改）----

	// 起始闪光球相对 Radius 的倍数
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|VFX|Burst")
	float FlashStartScale = 0.35f;

	// 闪光球最大放大到 Radius 的多少倍
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|VFX|Burst")
	float FlashEndScale = 1.15f;

	// 扩散环最终半径相对 Radius 的倍数（环要飞得比闪光远，才有"冲击波"感）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|VFX|Burst")
	float RingEndScale = 2.6f;

	// 自发光峰值（HDR，越大越亮；>1 才会吃到 Bloom）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|VFX|Burst")
	float PeakIntensity = 14.f;

	// 点光峰值强度
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|VFX|Burst")
	float LightPeakIntensity = 4200.f;

	// 点光衰减半径相对 Radius 的倍数
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|VFX|Burst")
	float LightRadiusScale = 4.f;

	// 环贴地时抬高一点，避免和地面 Z-Fighting
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|VFX|Burst")
	float RingGroundOffset = 4.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|VFX|Burst")
	FLinearColor BurstColor = FLinearColor(0.45f, 0.75f, 1.f, 1.f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|VFX|Burst")
	float BurstRadius = 90.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|VFX|Burst")
	float BurstDuration = 0.32f;

	// 是否生成扩散环。纯空爆（打在空中）可以关掉
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|VFX|Burst")
	bool bUseRing = true;

	// 是否显示中心闪光【球】。关掉后只剩扩散环 + 点光 —— 弹反 / 拼刀走这条。
	// 注意：球是 RootComponent，隐藏它【不会】连带隐藏挂在它下面的环和光
	// （SetVisibility 默认不向子组件传播），所以关掉球不会把整个特效弄没。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|VFX|Burst")
	bool bUseFlash = true;

	// 命中表面法线：环会贴到这个平面上
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|VFX|Burst")
	FVector SurfaceNormal = FVector::UpVector;

protected:
	void ApplyMaterials();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Demo|VFX|Burst")
	TObjectPtr<UStaticMeshComponent> FlashMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Demo|VFX|Burst")
	TObjectPtr<UStaticMeshComponent> RingMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Demo|VFX|Burst")
	TObjectPtr<UPointLightComponent> GlowLight;

private:
	// 生成时请求开灯（BeginPlay 里读，因为组件在构造时还拿不到最终参数）
	bool bRequestLight = true;

	float Elapsed = 0.f;
	bool bFinished = false;
};
