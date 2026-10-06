#include "Combat/DemoProjectile.h"
#include "Combat/DemoTrailComponent.h"
#include "Combat/DemoImpactBurst.h"
#include "Core/DemoCharacterBase.h"
#include "Core/DemoAbilitySystemLibrary.h"

#include "Components/SphereComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Kismet/GameplayStatics.h"

namespace
{
	const TCHAR* GProjectileCoreMaterialPath = TEXT("/Game/Boss/VFX/MI_BossGlow.MI_BossGlow");
	// 资产没跑脚本时的兜底：引擎基础形状材质是实心的，但至少"看得见、有颜色"
	const TCHAR* GFallbackCoreMaterialPath = TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");
	const TCHAR* GSphereMeshPath = TEXT("/Engine/BasicShapes/Sphere.Sphere");
}

ADemoProjectile::ADemoProjectile()
{
	PrimaryActorTick.bCanEverTick = false;

	// 球体当根：碰撞与飞行都以它为准
	CollisionSphere = CreateDefaultSubobject<USphereComponent>(TEXT("CollisionSphere"));
	CollisionSphere->InitSphereRadius(26.f);
	CollisionSphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	CollisionSphere->SetCollisionObjectType(ECC_WorldDynamic);
	CollisionSphere->SetCollisionResponseToAllChannels(ECR_Ignore);
	CollisionSphere->SetGenerateOverlapEvents(true);
	// 只关心两件事：撞到地形（消失）、穿过角色（结算伤害）
	CollisionSphere->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
	CollisionSphere->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Overlap);
	CollisionSphere->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	CollisionSphere->SetNotifyRigidBodyCollision(true);
	RootComponent = CollisionSphere;

	ProjectileMovement = CreateDefaultSubobject<UProjectileMovementComponent>(TEXT("ProjectileMovement"));
	ProjectileMovement->SetUpdatedComponent(CollisionSphere);
	ProjectileMovement->bRotationFollowsVelocity = true;
	ProjectileMovement->bShouldBounce = false;
	ProjectileMovement->ProjectileGravityScale = 0.f;
	ProjectileMovement->bSweepCollision = true;
	ProjectileMovement->InitialSpeed = 0.f;   // 真正速度在 BeginPlay 里按参数设
	ProjectileMovement->MaxSpeed = 0.f;

	// 可见外观：核心球。具体网格 / 材质 / 颜色在 BeginPlay 里按 Params 配。
	// 这一层是"看得见来弹"的底线 —— 没有它，弹幕就是一团看不见的伤害。
	VisualMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("VisualMesh"));
	VisualMesh->SetupAttachment(CollisionSphere);
	VisualMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	VisualMesh->SetGenerateOverlapEvents(false);
	VisualMesh->SetCastShadow(false);
	VisualMesh->bReceivesDecals = false;
	VisualMesh->SetCanEverAffectNavigation(false);

	// 辉光点光：让弹幕飞过时地面被照亮，弹幕才有"从那边飞过来"的空间感
	GlowLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("GlowLight"));
	GlowLight->SetupAttachment(CollisionSphere);
	GlowLight->SetMobility(EComponentMobility::Movable);
	GlowLight->SetCastShadows(false);
	GlowLight->SetAttenuationRadius(420.f);
	GlowLight->SetLightingChannels(true, false, false);

	// 拖尾 = 弹道本体。放在组件里是因为突进斩 / 蓄力也要用同一套。
	Trail = CreateDefaultSubobject<UDemoTrailComponent>(TEXT("Trail"));

	InitialLifeSpan = 0.f;   // 由 Params.LifeSpan 决定，BeginPlay 里 SetLifeSpan
}

// ------------------------------------------------------------------ 生成
ADemoProjectile* ADemoProjectile::SpawnProjectile(
	UObject* WorldContextObject,
	TSubclassOf<ADemoProjectile> ProjectileClass,
	AActor* InInstigator,
	const FVector& SpawnLocation,
	const FVector& Direction,
	const FBossProjectileParams& InParams)
{
	UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull) : nullptr;
	if (!World)
	{
		return nullptr;
	}

	const TSubclassOf<ADemoProjectile> ClassToUse = ProjectileClass ? ProjectileClass : TSubclassOf<ADemoProjectile>(ADemoProjectile::StaticClass());

	// 方向：优先用传入方向（保留俯仰角，可以做抛物线弹幕），零向量时用发射者正前方
	FVector Dir = Direction.GetSafeNormal();
	if (Dir.IsNearlyZero())
	{
		Dir = InInstigator ? InInstigator->GetActorForwardVector().GetSafeNormal() : FVector::ForwardVector;
	}

	const FTransform SpawnTM(FRotationMatrix::MakeFromX(Dir).Rotator(), SpawnLocation);

	// 延迟生成：先把参数塞进去，再由 FinishSpawning 触发 BeginPlay，避免“先飞一帧再改速度”
	ADemoProjectile* Projectile = World->SpawnActorDeferred<ADemoProjectile>(
		ClassToUse, SpawnTM, InInstigator, Cast<APawn>(InInstigator),
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);

	if (!Projectile)
	{
		return nullptr;
	}

	Projectile->Params = InParams;
	Projectile->SetOwner(InInstigator);
	Projectile->SetInstigator(Cast<APawn>(InInstigator));
	UGameplayStatics::FinishSpawningActor(Projectile, SpawnTM);

	return Projectile;
}

void ADemoProjectile::BeginPlay()
{
	Super::BeginPlay();

	if (Params.LifeSpan > 0.f)
	{
		SetLifeSpan(Params.LifeSpan);
	}

	if (CollisionSphere)
	{
		CollisionSphere->SetSphereRadius(FMath::Max(Params.CollisionRadius, 1.f));
		CollisionSphere->OnComponentBeginOverlap.AddDynamic(this, &ADemoProjectile::HandleBeginOverlap);
		CollisionSphere->OnComponentHit.AddDynamic(this, &ADemoProjectile::HandleHit);
	}

	if (ProjectileMovement)
	{
		ProjectileMovement->InitialSpeed = Params.Speed;
		ProjectileMovement->MaxSpeed = Params.Speed;
		ProjectileMovement->ProjectileGravityScale = FMath::Max(Params.GravityScale, 0.f);
		ProjectileMovement->Velocity = GetActorForwardVector() * Params.Speed;
		ProjectileMovement->Activate(true);
	}

	ApplyVisualSetup();
}

// ------------------------------------------------------------------ 外观
void ADemoProjectile::ApplyVisualSetup()
{
	if (VisualMesh && Params.bEnableCoreVisual)
	{
		if (UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, GSphereMeshPath))
		{
			VisualMesh->SetStaticMesh(Sphere);
		}

		// 球体网格直径 100cm → 缩放 = 目标直径 / 100
		const float CoreSize = FMath::Max(Params.CollisionRadius, 1.f) * 2.f * FMath::Max(Params.CoreVisualScale, 0.01f);
		VisualMesh->SetRelativeScale3D(FVector(CoreSize / 100.f));

		// 先按约定路径取，取不到用引擎基础材质兜底：
		// 宁可"一个实心亮球"也不要"看不见的弹幕"——后者玩家根本没法躲
		UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, GProjectileCoreMaterialPath);
		if (!Base)
		{
			Base = LoadObject<UMaterialInterface>(nullptr, GFallbackCoreMaterialPath);
		}

		if (Base)
		{
			if (UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Base, this))
			{
				MID->SetVectorParameterValue(TEXT("Color"), Params.CoreColor);
				MID->SetScalarParameterValue(TEXT("Intensity"), Params.CoreIntensity);
				MID->SetScalarParameterValue(TEXT("Opacity"), 1.f);
				VisualMesh->SetMaterial(0, MID);
			}
		}
	}
	else if (VisualMesh)
	{
		VisualMesh->SetVisibility(false);
	}

	if (GlowLight)
	{
		const bool bLight = Params.bCastGlowLight;
		GlowLight->SetVisibility(bLight);
		if (bLight)
		{
			GlowLight->SetLightColor(Params.CoreColor);
			GlowLight->SetIntensity(Params.GlowLightIntensity);
			GlowLight->SetAttenuationRadius(FMath::Max(Params.CollisionRadius, 1.f) * 14.f);
		}
	}

	if (Trail)
	{
		if (Params.bEnableTrail)
		{
			// 拖尾粗细：以前是碰撞半径的 1.9 倍 —— 24cm 半径的弹配 45cm 宽的光带，
			// 5 发并排就是一面墙。1.0 倍（= 和弹体等宽）已经足够读出弹道，
			// 且不会把弹幕糊成一片。
			Trail->ApplyPresetProjectile(
				FMath::Max(Params.CollisionRadius, 1.f) * 1.0f,
				Params.TrailColor,
				Params.TrailBrightness);

			// 段数由招式数据决定：单发弹幕可以给 16 段的长尾巴，
			// 一轮几十发的环形大招给 6 段就够（省下的是每帧的实例写入）
			Trail->SampleDistance = FMath::Max(Params.Speed * 0.016f, 10.f);
			Trail->SampleCount = FMath::Clamp(Params.TrailSampleCount, 2, 32);
			Trail->SetTrailEnabled(true);
		}
		else
		{
			Trail->SetTrailEnabled(false);
		}
	}
}

// ------------------------------------------------------------------ 碰撞
void ADemoProjectile::HandleBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
	UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	// FHitResult::ImpactPoint 是 FVector_NetQuantize，和 FVector 混进三元表达式会有类型歧义（C2446）
	const FVector ImpactPoint = SweepResult.bBlockingHit ? FVector(SweepResult.ImpactPoint) : GetActorLocation();
	ProcessTouch(OtherActor, ImpactPoint, SweepResult.ImpactNormal, false);
}

void ADemoProjectile::HandleHit(UPrimitiveComponent* HitComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp,
	FVector NormalImpulse, const FHitResult& Hit)
{
	ProcessTouch(OtherActor, Hit.ImpactPoint, Hit.ImpactNormal, true);
}

void ADemoProjectile::ProcessTouch(AActor* OtherActor, const FVector& ImpactLocation, const FVector& ImpactNormal, bool bBlockedByWorld)
{
	// 打到地形 / 静态物：直接消失（这是弹幕的“实体边界”）
	if (bBlockedByWorld)
	{
		if (bResolved)
		{
			return;
		}
		bResolved = true;
		ResolveAndDestroy(OtherActor, ImpactLocation, ImpactNormal);
		return;
	}

	// 打到自己（发射者）或自己身上的组件：忽略，继续飞
	if (!OtherActor || OtherActor == this || OtherActor == GetOwner())
	{
		return;
	}

	AActor* SourceActor = GetOwner();
	const ADemoCharacterBase* Source = Cast<ADemoCharacterBase>(SourceActor);
	const ADemoCharacterBase* Target = Cast<ADemoCharacterBase>(OtherActor);

	// 不是「有 ASC 的敌对角色」就一律忽略、继续飞：
	// 同伴、中立物件都不该吃掉 Boss 的弹幕（否则手感会莫名变弱）
	if (!Source || !Target || !Source->IsHostileTo(Target))
	{
		return;
	}

	// 已经结算过（穿透中）就不能重复打同一个目标
	if (bResolved)
	{
		return;
	}

	if (ApplyHitToTarget(OtherActor, ImpactLocation))
	{
		bResolved = true;
		ResolveAndDestroy(OtherActor, ImpactLocation, ImpactNormal);
	}
}

void ADemoProjectile::ResolveAndDestroy(AActor* HitActor, const FVector& ImpactLocation, const FVector& ImpactNormal)
{
	// 先停拖尾再销毁：拖尾是独立组件，留着它会在弹体消失后还自己飘一小段
	if (Trail)
	{
		Trail->SetTrailEnabled(false);
	}

	if (Params.bSpawnImpactBurst)
	{
		ADemoImpactBurst::SpawnBurst(
			this,
			ImpactLocation,
			ImpactNormal,
			Params.CoreColor,
			FMath::Max(Params.CollisionRadius, 1.f) * Params.ImpactBurstRadiusScale,
			Params.ImpactBurstDuration,
			/*bSpawnLight=*/Params.bCastGlowLight,
			Params.ImpactBurstClass);
	}

	// 蓝图表现钩子（爆点特效 / 音效）——放在最后，销毁之前
	OnProjectileImpact(HitActor, ImpactLocation, ImpactNormal);

	Destroy();
}

bool ADemoProjectile::ApplyHitToTarget(AActor* TargetActor, const FVector& ImpactLocation)
{
	AActor* SourceActor = GetOwner();
	if (!SourceActor || !TargetActor)
	{
		return true;
	}

	// 走统一伤害入口 —— 格挡、弹反、无敌帧（闪避）、暴击、顿帧、敌我过滤都在里面处理
	UDemoAbilitySystemLibrary::ApplyHit(
		SourceActor, TargetActor,
		Params.Damage, Params.PoiseDamage,
		Params.bCanBeParried, Params.bCanBeBlocked,
		GetActorLocation(), Params.bTriggerHitStop,
		Params.DamageSource);

	++HitCount;

	// 穿透：还没打够数量就继续飞
	if (Params.bPierceTargets && (Params.MaxPierceCount <= 0 || HitCount < Params.MaxPierceCount))
	{
		return false;
	}

	return true;
}
