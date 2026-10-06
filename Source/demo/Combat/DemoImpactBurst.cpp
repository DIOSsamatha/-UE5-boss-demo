#include "Combat/DemoImpactBurst.h"

#include "Components/StaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

namespace
{
	const TCHAR* GBurstFlashMaterialPath = TEXT("/Game/Boss/VFX/MI_BossImpact.MI_BossImpact");
	const TCHAR* GBurstRingMaterialPath = TEXT("/Game/Boss/VFX/MI_BossRing.MI_BossRing");

	// 资产缺失时的兜底：至少让"有东西闪了一下、是蓝色"能被看见，
	// 而不是安静地什么都不发生（那样根本分不清是没配资产还是代码没跑）
	const TCHAR* GFallbackMaterialPath = TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");

	const TCHAR* GSphereMeshPath = TEXT("/Engine/BasicShapes/Sphere.Sphere");
	const TCHAR* GPlaneMeshPath = TEXT("/Engine/BasicShapes/Plane.Plane");

	// 引擎球体 / 平面都是 100×100（cm）
	constexpr float GBasicShapeSize = 100.f;

	UMaterialInterface* LoadMaterialWithFallback(const TCHAR* Path, FLinearColor Color)
	{
		// Color 只是留给未来做"按颜色找对应材质"的扩展位，当前两条路径都用同一套参数化材质
		(void)Color;

		if (UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, Path))
		{
			return Material;
		}
		return LoadObject<UMaterialInterface>(nullptr, GFallbackMaterialPath);
	}
}

ADemoImpactBurst::ADemoImpactBurst()
{
	PrimaryActorTick.bCanEverTick = true;

	FlashMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("FlashMesh"));
	FlashMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	FlashMesh->SetGenerateOverlapEvents(false);
	FlashMesh->SetCastShadow(false);
	FlashMesh->bReceivesDecals = false;
	FlashMesh->SetCanEverAffectNavigation(false);
	RootComponent = FlashMesh;

	RingMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("RingMesh"));
	RingMesh->SetupAttachment(FlashMesh);
	RingMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	RingMesh->SetGenerateOverlapEvents(false);
	RingMesh->SetCastShadow(false);
	RingMesh->bReceivesDecals = false;
	RingMesh->SetCanEverAffectNavigation(false);

	GlowLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("GlowLight"));
	GlowLight->SetupAttachment(FlashMesh);
	GlowLight->SetMobility(EComponentMobility::Movable);
	GlowLight->SetCastShadows(false);
	GlowLight->SetAttenuationRadius(600.f);
	// 一闪即逝的光：用移动性光源但关掉与场景光的混合，避免把周围烘焙光照弄脏
	GlowLight->SetLightingChannels(true, false, false);

	InitialLifeSpan = 0.f;   // 由 Duration 决定
}

// ------------------------------------------------------------------ 生成
ADemoImpactBurst* ADemoImpactBurst::SpawnBurst(
	UObject* WorldContextObject,
	const FVector& Location,
	const FVector& SurfaceNormal,
	FLinearColor Color,
	float Radius,
	float Duration,
	bool bSpawnLight,
	TSubclassOf<ADemoImpactBurst> BurstClass,
	bool bUseFlash,
	bool bUseRing)
{
	UWorld* World = GEngine
		? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull)
		: nullptr;
	if (!World)
	{
		return nullptr;
	}

	const TSubclassOf<ADemoImpactBurst> ClassToUse = BurstClass
		? BurstClass
		: TSubclassOf<ADemoImpactBurst>(ADemoImpactBurst::StaticClass());

	// 朝向：环贴到命中表面上。法线退化时用世界朝上兜底
	FVector Normal = SurfaceNormal.GetSafeNormal();
	if (Normal.IsNearlyZero())
	{
		Normal = FVector::UpVector;
	}

	const FTransform SpawnTM(FRotationMatrix::MakeFromZ(Normal).Rotator(), Location);

	ADemoImpactBurst* Burst = World->SpawnActorDeferred<ADemoImpactBurst>(
		ClassToUse, SpawnTM, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Burst)
	{
		return nullptr;
	}

	Burst->BurstColor = Color;
	Burst->BurstRadius = FMath::Max(Radius, 1.f);
	Burst->BurstDuration = FMath::Max(Duration, 0.02f);
	Burst->SurfaceNormal = Normal;
	Burst->bRequestLight = bSpawnLight;
	Burst->bUseFlash = bUseFlash;
	Burst->bUseRing = bUseRing;

	Burst->FinishSpawning(SpawnTM);
	return Burst;
}

void ADemoImpactBurst::BeginPlay()
{
	Super::BeginPlay();

	ApplyMaterials();

	// 一上来就先把"第一帧的样子"写对，避免第 0 帧闪烁（Tick 之前有一帧是原始缩放）
	Elapsed = 0.f;
	Tick(0.f);
}

void ADemoImpactBurst::ApplyMaterials()
{
	// 中心球要不要出现 —— 弹反 / 拼刀关掉（调用方传 bUseFlash=false），
	// 只留扩散环 + 点光。球是 RootComponent，但 SetVisibility 默认不向子组件传播，
	// 所以这里关的是【且仅是】那个球。
	FlashMesh->SetVisibility(bUseFlash);

	// ⚠ 下面这些（环 / 点光）跟球无关，【不能】因为关了球就提前 return 掉，
	//    否则弹反会变成"什么都没有"。这里只是不去加载球体网格和它的材质。
	if (bUseFlash)
	{
		if (UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, GSphereMeshPath))
		{
			FlashMesh->SetStaticMesh(Sphere);
		}

		UMaterialInterface* FlashBase = LoadMaterialWithFallback(GBurstFlashMaterialPath, BurstColor);
		if (FlashBase)
		{
			if (UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(FlashBase, this))
			{
				MID->SetVectorParameterValue(TEXT("Color"), BurstColor);
				MID->SetScalarParameterValue(TEXT("Intensity"), PeakIntensity);
				FlashMesh->SetMaterial(0, MID);
			}
		}
	}

	if (UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, GPlaneMeshPath))
	{
		RingMesh->SetStaticMesh(Plane);
	}

	RingMesh->SetVisibility(bUseRing);
	if (bUseRing)
	{
		UMaterialInterface* RingBase = LoadMaterialWithFallback(GBurstRingMaterialPath, BurstColor);
		if (RingBase)
		{
			if (UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(RingBase, this))
			{
				MID->SetVectorParameterValue(TEXT("Color"), BurstColor);
				MID->SetScalarParameterValue(TEXT("Intensity"), PeakIntensity * 0.55f);
				MID->SetScalarParameterValue(TEXT("Opacity"), 1.f);
				RingMesh->SetMaterial(0, MID);
			}
		}
	}

	// 环抬高一点贴地：完全同高会和地面 Z-Fighting（表现为一圈闪烁的锯齿）
	RingMesh->SetRelativeLocation(FVector(0.f, 0.f, RingGroundOffset));

	if (GlowLight)
	{
		GlowLight->SetVisibility(bRequestLight);
		GlowLight->SetLightColor(BurstColor);
		GlowLight->SetAttenuationRadius(BurstRadius * LightRadiusScale);
	}
}

// ------------------------------------------------------------------ 每帧
void ADemoImpactBurst::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (bFinished)
	{
		return;
	}

	Elapsed += DeltaTime;

	const float Duration = FMath::Max(BurstDuration, KINDA_SMALL_NUMBER);
	const float T = FMath::Clamp(Elapsed / Duration, 0.f, 1.f);

	// ---- 中心闪光：前 1/3 冲到最大、之后迅速收，亮度一路衰减 ----
	// 用 pow 而不是线性：线性的闪光看起来像"慢慢变大的一坨"，没有"炸"的感觉
	const float FlashPulse = FMath::Clamp(T / 0.33f, 0.f, 1.f);
	const float FlashScaleFactor = FMath::Lerp(FlashStartScale, FlashEndScale, FMath::Pow(FlashPulse, 0.5f));
	if (bUseFlash)
	{
		const float FlashScale = BurstRadius * FlashScaleFactor * 2.f / GBasicShapeSize;
		FlashMesh->SetRelativeScale3D(FVector(FlashScale));

		if (UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(FlashMesh->GetMaterial(0)))
		{
			// (1-T)^2.2：起步几乎不降、末尾迅速归零 —— 闪光该有的"啪"一下
			MID->SetScalarParameterValue(TEXT("Intensity"), PeakIntensity * FMath::Pow(1.f - T, 2.2f));
		}
	}

	// ---- 扩散环：快速铺开 + 越往外越淡 ----
	if (bUseRing)
	{
		const float RingScaleFactor = FMath::Lerp(0.15f, RingEndScale, FMath::Pow(T, 0.55f));
		const float RingScale = BurstRadius * RingScaleFactor * 2.f / GBasicShapeSize;
		RingMesh->SetRelativeScale3D(FVector(RingScale));

		if (UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(RingMesh->GetMaterial(0)))
		{
			MID->SetScalarParameterValue(TEXT("Intensity"), PeakIntensity * 0.55f * FMath::Pow(1.f - T, 1.6f));
		}
	}

	// ---- 点光：跟着一起收 ----
	if (GlowLight && GlowLight->IsVisible())
	{
		GlowLight->SetIntensity(LightPeakIntensity * FMath::Pow(1.f - T, 2.f));
	}

	if (T >= 1.f)
	{
		bFinished = true;
		Destroy();
	}
}
