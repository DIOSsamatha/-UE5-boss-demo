#include "Combat/DemoTrailComponent.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "GameFramework/Actor.h"

namespace
{
	// 约定路径（由 Tools/SetupBossVFX.py 生成）。取不到就不显示拖尾，不影响任何玩法逻辑。
	const TCHAR* GTrailMaterialPath = TEXT("/Game/Boss/VFX/MI_BossGlow.MI_BossGlow");
	const TCHAR* GTrailMeshPath = TEXT("/Engine/BasicShapes/Sphere.Sphere");

	// 每实例自定义数据的通道数：0/1/2 = RGB（颜色 × 亮度）
	constexpr int32 GTrailCustomDataFloats = 3;
}

UDemoTrailComponent::UDemoTrailComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;

	// 拖尾是纯表现：不需要每帧都跑在"物理/动画"之前
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

void UDemoTrailComponent::BeginPlay()
{
	Super::BeginPlay();

	BuildComponents();

	// 首帧就把当前位置当基准点：否则第一次采样会从世界原点拉到当前位置
	if (const AActor* Owner = GetOwner())
	{
		LastSampleLocation = Owner->GetActorLocation();
		bHasLastSample = true;
	}
}

void UDemoTrailComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Points.Reset();
	Super::EndPlay(EndPlayReason);
}

// ------------------------------------------------------------------ 构建
void UDemoTrailComponent::BuildComponents()
{
	AActor* Owner = GetOwner();
	if (!Owner || !Owner->GetRootComponent() || SampleCount <= 0)
	{
		return;
	}

	UMaterialInterface* Material = TrailMaterial;
	if (!Material)
	{
		Material = LoadObject<UMaterialInterface>(nullptr, GTrailMaterialPath);
	}
	if (!Material)
	{
		// 没有材质就不建组件了：反正加法混合的拖尾没材质就是一堆灰球，不如不要。
		UE_LOG(LogTemp, Verbose,
			TEXT("[VFX] 找不到拖尾材质 %s → %s 的拖尾已省略（跑一次 Tools/SetupBossVFX.py 就有了）"),
			GTrailMaterialPath, *GetNameSafe(Owner));
		return;
	}

	UStaticMesh* Mesh = TrailMesh;
	if (!Mesh)
	{
		Mesh = LoadObject<UStaticMesh>(nullptr, GTrailMeshPath);
	}
	if (!Mesh)
	{
		return;
	}

	TrailInstances = NewObject<UInstancedStaticMeshComponent>(Owner, TEXT("DemoTrailInstances"));
	if (!TrailInstances)
	{
		return;
	}

	TrailInstances->SetupAttachment(Owner->GetRootComponent());
	TrailInstances->SetMobility(EComponentMobility::Movable);

	// 拖尾不参与任何碰撞 / 遮挡 / 阴影：它是光，不是物体
	TrailInstances->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	TrailInstances->SetCollisionProfileName(TEXT("NoCollision"));
	TrailInstances->SetGenerateOverlapEvents(false);
	TrailInstances->SetCastShadow(false);
	TrailInstances->bReceivesDecals = false;
	TrailInstances->bUseAsOccluder = false;
	TrailInstances->SetCanEverAffectNavigation(false);

	TrailInstances->SetStaticMesh(Mesh);

	// 每个实例一份 MID 没必要（颜色靠每实例数据），但亮度要能单独调 → 一个组件一个 MID
	if (UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Material, this))
	{
		MID->SetVectorParameterValue(TEXT("Color"), FLinearColor::White);
		MID->SetScalarParameterValue(TEXT("Intensity"), Brightness);
		TrailInstances->SetMaterial(0, MID);
	}
	else
	{
		TrailInstances->SetMaterial(0, Material);
	}

	// ⚠ 必须在 AddInstance 之前设好通道数：SetNumCustomDataFloats 内部会重建实例数据，
	//   已经有实例的时候调用会断言失败（引擎明确禁止"边用边改通道数"）
	TrailInstances->SetNumCustomDataFloats(GTrailCustomDataFloats);

	TrailInstances->RegisterComponent();
	Owner->AddInstanceComponent(TrailInstances);

	// 一次性铺满实例池，之后只更新变换与自定义数据（不再增删实例）
	for (int32 Index = 0; Index < SampleCount; ++Index)
	{
		TrailInstances->AddInstance(FTransform::Identity, /*bWorldSpace=*/false);
	}

	bInstancesReady = true;

	HideAllInstances();
}

// ------------------------------------------------------------------ 每帧
void UDemoTrailComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!bInstancesReady)
	{
		return;
	}

	UpdateTrail(DeltaTime);
}

void UDemoTrailComponent::UpdateTrail(float DeltaTime)
{
	if (bFading)
	{
		FadeRemaining -= DeltaTime;
		if (FadeRemaining <= 0.f)
		{
			// 淡完了就彻底收工，别继续每帧写实例数据
			Points.Reset();
			HideAllInstances();
			bFading = false;
			bTrailEnabled = false;
			SetComponentTickEnabled(false);
			return;
		}
	}

	const AActor* Owner = GetOwner();
	if (bTrailEnabled && Owner)
	{
		const FVector Current = Owner->GetActorLocation();

		if (!bHasLastSample)
		{
			LastSampleLocation = Current;
			bHasLastSample = true;
		}
		else
		{
			const float Moved = FVector::Dist(Current, LastSampleLocation);
			const float Step = FMath::Max(SampleDistance, 1.f);

			if (Moved > MaxSampleSnapDistance)
			{
				// 瞬移：丢掉历史。否则会从上一个位置直线拉出一条长尾（生成首帧最明显）
				Points.Reset();
				LastSampleLocation = Current;
			}
			else if (Moved >= Step)
			{
				// 一帧走了好几步就补齐中间点，保证高速下尾巴依然是连续的（不拉断成珠链）
				const int32 Steps = FMath::Clamp(FMath::FloorToInt(Moved / Step), 1, FMath::Max(SampleCount, 1));
				for (int32 s = 1; s <= Steps; ++s)
				{
					const float Alpha = static_cast<float>(s) / static_cast<float>(Steps);
					PushPoint(FMath::Lerp(LastSampleLocation, Current, Alpha));
				}
				LastSampleLocation = Current;
			}
		}
	}

	const float FadeAlpha = bFading
		? FMath::Clamp(FadeRemaining / FMath::Max(FadeOutTime, KINDA_SMALL_NUMBER), 0.f, 1.f)
		: 1.f;

	RefreshInstances(FadeAlpha);
}

void UDemoTrailComponent::PushPoint(const FVector& Location)
{
	Points.Insert(Location, 0);

	if (Points.Num() > SampleCount)
	{
		Points.SetNum(SampleCount, EAllowShrinking::No);
	}
}

void UDemoTrailComponent::RefreshInstances(float FadeAlpha)
{
	if (!TrailInstances)
	{
		return;
	}

	const int32 Available = Points.Num();
	bool bAnyVisible = false;

	for (int32 Index = 0; Index < SampleCount; ++Index)
	{
		const float T = (SampleCount > 1)
			? static_cast<float>(Index) / static_cast<float>(SampleCount - 1)
			: 0.f;

		float Size = 0.f;
		float Bright = 0.f;
		FVector Location = FVector::ZeroVector;

		if (Index < Available && Available > 0)
		{
			Location = Points[Index];
			Size = HeadSize * FMath::Lerp(1.f, TailSizeScale, T) * FadeAlpha;
			Bright = FMath::Lerp(1.f, TailBrightness, T) * FadeAlpha;
			bAnyVisible = true;
		}

		// 球体网格是 100cm 直径 → 缩放 = 目标尺寸 / 100
		const float Scale = Size / 100.f;
		const FTransform TM(FQuat::Identity, Location, FVector(Scale));

		// 批量写：这里整轮都不刷渲染状态，最后统一 MarkRenderStateDirty 一次
		//
		// ⚠ 必须是【世界空间】：Points 里存的是 Owner->GetActorLocation()（世界坐标），
		//   而本组件挂在 Owner 的 RootComponent 下 —— 用 bWorldSpace=false 会把世界坐标
		//   当成"相对父组件的偏移"，整条拖尾会被画到 Owner 位置 + 世界位置 的地方，
		//   看起来就是"凭空多出一坨球"（Owner 靠近世界原点时正好落在 BOSS 头顶附近）。
		//   （2026-09-29 修复：用户反馈的"BOSS 头上有球"就是这个）
		TrailInstances->UpdateInstanceTransform(Index, TM, /*bWorldSpace=*/true,
			/*bMarkRenderStateDirty=*/false, /*bTeleport=*/true);

		const float R = TrailColor.R * Bright;
		const float G = TrailColor.G * Bright;
		const float B = TrailColor.B * Bright;
		TrailInstances->SetCustomDataValue(Index, 0, R, false);
		TrailInstances->SetCustomDataValue(Index, 1, G, false);
		TrailInstances->SetCustomDataValue(Index, 2, B, false);
	}

	TrailInstances->MarkRenderStateDirty();

	// 整条都没了（还没起步 / 已淡完）就顺便停掉 Tick，省一点是一点
	if (!bAnyVisible && !bTrailEnabled)
	{
		SetComponentTickEnabled(false);
	}
}

void UDemoTrailComponent::HideAllInstances()
{
	if (!TrailInstances)
	{
		return;
	}

	// 缩放为 0 的实例不可见；这里同样按世界空间写（和 RefreshInstances 保持一致）
	const FTransform Hidden(FQuat::Identity, FVector::ZeroVector, FVector::ZeroVector);
	for (int32 Index = 0; Index < SampleCount; ++Index)
	{
		TrailInstances->UpdateInstanceTransform(Index, Hidden, true, false, true);
		TrailInstances->SetCustomDataValue(Index, 0, 0.f, false);
		TrailInstances->SetCustomDataValue(Index, 1, 0.f, false);
		TrailInstances->SetCustomDataValue(Index, 2, 0.f, false);
	}
	TrailInstances->MarkRenderStateDirty();
}

// ------------------------------------------------------------------ 对外接口
void UDemoTrailComponent::SetTrailEnabled(bool bEnabled)
{
	if (bTrailEnabled == bEnabled)
	{
		return;
	}

	bTrailEnabled = bEnabled;

	if (bEnabled)
	{
		bFading = false;
		FadeRemaining = 0.f;
		bHasLastSample = false;
		SetComponentTickEnabled(true);
	}
	else
	{
		// 关掉 = 立刻清干净（弹体命中时用，不需要淡出）
		Points.Reset();
		HideAllInstances();
		bFading = false;
	}
}

void UDemoTrailComponent::StopTrail()
{
	if (!bTrailEnabled || bFading)
	{
		return;
	}

	if (FadeOutTime <= 0.f)
	{
		SetTrailEnabled(false);
		return;
	}

	bFading = true;
	FadeRemaining = FadeOutTime;
	SetComponentTickEnabled(true);
}

void UDemoTrailComponent::SetTrailColor(FLinearColor InColor, float InBrightness)
{
	TrailColor = InColor;

	if (InBrightness >= 0.f)
	{
		Brightness = InBrightness;
	}

	if (TrailInstances)
	{
		if (UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(TrailInstances->GetMaterial(0)))
		{
			MID->SetScalarParameterValue(TEXT("Intensity"), Brightness);
		}
	}
}

void UDemoTrailComponent::ApplyPresetProjectile(float InHeadSize, FLinearColor InColor, float InBrightness)
{
	// 弹丸：采样密、尾巴短而亮 —— 读起来是"高速掠过的光弹"
	SampleCount = 12;
	SampleDistance = 14.f;
	MaxSampleSnapDistance = 260.f;
	HeadSize = InHeadSize;
	TailSizeScale = 0.22f;
	TailBrightness = 0.05f;
	TrailColor = InColor;
	Brightness = InBrightness;
}

void UDemoTrailComponent::ApplyPresetDash(float InHeadSize, FLinearColor InColor, float InBrightness)
{
	// 突进：速度是弹丸的两三倍，采样要稀一点，尾巴长而粗 —— 读起来是"拖刀"
	SampleCount = 18;
	SampleDistance = 40.f;
	MaxSampleSnapDistance = 600.f;
	HeadSize = InHeadSize;
	TailSizeScale = 0.45f;
	TailBrightness = 0.10f;
	TrailColor = InColor;
	Brightness = InBrightness;
}
