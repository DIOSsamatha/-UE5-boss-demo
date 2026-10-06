// 弹幕投射物 —— BOSS 的远程手段（远程弹幕 / 落地震荡波 / 大招弹幕圈 共用）
//
// 设计要点：
//  1) 用「球体碰撞 + ProjectileMovement」飞行，不开物理模拟（QueryOnly），性能好、轨迹完全可控。
//  2) 命中一律走 UDemoAbilitySystemLibrary::ApplyHit → 玩家的格挡 / 弹反 / 无敌帧逻辑自动生效，
//     不需要在弹体里重复实现一套战斗规则。
//  3) 敌我判定用 ADemoCharacterBase::IsHostileTo（TeamId 比较），不会误伤 BOSS 自己或同伴。
//  4) 外观与特效全部留给蓝图子类（BP_BossProjectile）：挂 Niagara、拖尾、Mesh；C++ 只留一个空槽位。
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Core/DemoCommon.h"
#include "DemoProjectile.generated.h"

class USphereComponent;
class UProjectileMovementComponent;
class UStaticMeshComponent;
class UPointLightComponent;
class UDemoTrailComponent;
class ADemoImpactBurst;

// ------------------------------------------------------------------ 弹体参数
// 由 UBossAttackData 配置、技能在发射时传入。
// 好处：同一个弹体类可以打出完全不同手感的弹幕（快速直射 / 慢速抛物线 / 穿透巨型弹）。
USTRUCT(BlueprintType)
struct FBossProjectileParams
{
	GENERATED_BODY()

	// 飞行速度（cm/s）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile")
	float Speed = 900.f;

	// 命中伤害
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile")
	float Damage = 12.f;

	// 命中削韧
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile")
	float PoiseDamage = 6.f;

	// 自动销毁时间（秒）。飞太久还没打中也会消失，避免弹幕在场景里堆积
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile")
	float LifeSpan = 6.f;

	// 碰撞球半径（cm）。放大 = 更好命中，但更容易被地形吃掉
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile")
	float CollisionRadius = 26.f;

	// 重力倍率：0 = 直线飞行；>0 会下坠（做抛物线弹幕）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile")
	float GravityScale = 0.f;

	// 能否被玩家弹反（默认否 —— 弹幕讲究走位躲避，不是弹刀素材）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile")
	bool bCanBeParried = false;

	// 能否被格挡（默认可以：举盾挡弹幕，但要吃耐力消耗）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile")
	bool bCanBeBlocked = true;

	// 穿透：命中目标后不消失，可以继续打下一个
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile")
	bool bPierceTargets = false;

	// 最多穿透几个目标（0 = 不限，仅在 bPierceTargets 为真时有意义）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile")
	int32 MaxPierceCount = 0;

	// 命中是否触发顿帧。弹幕默认关：一次齐射命中好几颗会顿得很难受
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile")
	bool bTriggerHitStop = false;

	// 打出来算哪一类伤害（只影响表现：飘字颜色、音效）。
	// 震荡波 / 大招弹幕圈可以改成 HeavyImpact / Ultimate，UI 就能区分开。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile")
	EDemoDamageSource DamageSource = EDemoDamageSource::Projectile;

	// ================= 弹道表现（纯视觉，缺资产会自动降级，不影响判定）=================
	// 这一整块的存在意义：让玩家「看得见来弹、来得及躲」。
	// 一发没有弹道的弹幕在动作游戏里等于随机伤害，玩家的挫败感全部来自这里。

	// 发光核心球（不依赖任何自制资产 —— 材质缺失时会用引擎基础形状材质兜底成一个实心亮球）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile|Visual")
	bool bEnableCoreVisual = true;

	// 核心球大小相对碰撞半径的倍数。
	//
	// ⚠ 刻意取 < 1：核心球外面还套着一圈加法混合的辉光（Bloom 会再糊大一圈），
	//   实测"看到的直径"大约是核心球的 1.5 倍。
	//   以前给 1.15 → 24cm 碰撞半径生成 55cm 的核心球、看上去快 80cm，弹幕大得离谱。
	//   0.55 时"可见大小 ≈ 碰撞体积"，既不虚胖也不至于看不见。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile|Visual")
	float CoreVisualScale = 0.55f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile|Visual")
	FLinearColor CoreColor = FLinearColor(0.30f, 0.70f, 1.f, 1.f);

	// 自发光倍率（HDR，>1 才有 Bloom 辉光）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile|Visual")
	float CoreIntensity = 12.f;

	// 拖尾（一条沿飞行轨迹的光带，也就是"弹道"本身）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile|Visual")
	bool bEnableTrail = true;

	// 拖尾残影段数。大招那种一轮几十发的场合调小（6~8），单发弹幕可以给到 12~16
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile|Visual")
	int32 TrailSampleCount = 12;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile|Visual")
	FLinearColor TrailColor = FLinearColor(0.22f, 0.60f, 1.f, 1.f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile|Visual")
	float TrailBrightness = 9.f;

	// 动态点光：让弹幕飞过时真的照亮地面。
	// ⚠ 一轮齐射 40 发就是 40 个动态光，会明显掉帧 —— 环形大招记得关掉
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile|Visual")
	bool bCastGlowLight = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile|Visual")
	float GlowLightIntensity = 2600.f;

	// 命中时生成爆点（ADemoImpactBurst）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile|Visual")
	bool bSpawnImpactBurst = true;

	// 爆点半径相对碰撞半径的倍数。以前 3.4 → 命中时炸出一团直径 1.7m 的白光，
	// 打中一次就糊掉半个屏幕；1.8 大概是一人高，看得见但不遮视线。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile|Visual")
	float ImpactBurstRadiusScale = 1.8f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile|Visual")
	float ImpactBurstDuration = 0.30f;

	// 爆点 / 命中特效类。留空 → 原生 ADemoImpactBurst
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile|Visual")
	TSubclassOf<ADemoImpactBurst> ImpactBurstClass;
};

UCLASS()
class DEMO_API ADemoProjectile : public AActor
{
	GENERATED_BODY()

public:
	ADemoProjectile();

	// 统一生成入口：远程弹幕技能 / 落地震荡波 / 大招弹幕圈 全走这里。
	// Direction 零向量时退回生成者的正前方；ProjectileClass 为空时用原生 ADemoProjectile。
	UFUNCTION(BlueprintCallable, Category = "Demo|Projectile", meta = (WorldContext = "WorldContextObject"))
	static ADemoProjectile* SpawnProjectile(
		UObject* WorldContextObject,
		TSubclassOf<ADemoProjectile> ProjectileClass,
		AActor* InInstigator,
		const FVector& SpawnLocation,
		const FVector& Direction,
		const FBossProjectileParams& InParams);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|Projectile")
	const FBossProjectileParams& GetParams() const { return Params; }

	virtual void BeginPlay() override;

	// ---- 蓝图表现钩子（在 BP_BossProjectile 里实现）----
	// 命中角色 / 打到地形时调用：在这里播爆点特效、音效（销毁之前）
	UFUNCTION(BlueprintImplementableEvent, Category = "Demo|Projectile")
	void OnProjectileImpact(AActor* HitActor, const FVector& ImpactLocation, const FVector& ImpactNormal);

	// 超时 / 飞出关卡时调用
	UFUNCTION(BlueprintImplementableEvent, Category = "Demo|Projectile")
	void OnProjectileExpired();

protected:
	UFUNCTION()
	void HandleBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp,
		int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);

	UFUNCTION()
	void HandleHit(UPrimitiveComponent* HitComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp,
		FVector NormalImpulse, const FHitResult& Hit);

	// 真正处理一次「碰到了东西」
	void ProcessTouch(AActor* OtherActor, const FVector& ImpactLocation, const FVector& ImpactNormal, bool bBlockedByWorld);

	// 对某个敌对角色结算一次伤害；返回 true 表示弹体该消失了
	bool ApplyHitToTarget(AActor* TargetActor, const FVector& ImpactLocation);

	// 弹出命中表现 + 销毁。所有"这条弹该没了"的路径都收敛到这里，
	// 避免爆点在某些分支漏掉（漏了会表现成"这发弹幕凭空消失"）
	void ResolveAndDestroy(AActor* HitActor, const FVector& ImpactLocation, const FVector& ImpactNormal);

	// 按 Params 配置核心球外观 / 拖尾 / 点光
	void ApplyVisualSetup();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Demo|Projectile")
	TObjectPtr<USphereComponent> CollisionSphere;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Demo|Projectile")
	TObjectPtr<UProjectileMovementComponent> ProjectileMovement;

	// 核心球：纯视觉，碰撞/飞行都由 CollisionSphere 负责
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Demo|Projectile")
	TObjectPtr<UStaticMeshComponent> VisualMesh;

	// 弹道拖尾（就是玩家看到的"弹道"本身 —— 让来弹方向可读）
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Demo|Projectile")
	TObjectPtr<UDemoTrailComponent> Trail;

	// 弹体自带的辉光点光（会把地面照出一圈光，弹幕才有"存在感"）
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Demo|Projectile")
	TObjectPtr<UPointLightComponent> GlowLight;

	UPROPERTY(BlueprintReadOnly, Category = "Demo|Projectile")
	FBossProjectileParams Params;

	// 已经打中过几个目标（穿透计数）
	UPROPERTY(BlueprintReadOnly, Category = "Demo|Projectile")
	int32 HitCount = 0;

	// 保险丝：一次 Sweep 里 Overlap 与 Hit 可能都触发，用它保证只结算一次
	bool bResolved = false;
};
