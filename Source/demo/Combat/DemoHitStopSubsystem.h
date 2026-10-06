// 顿帧 / 时缓子系统：命中瞬间冻结时间，完美闪避时进入子弹时间
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "DemoHitStopSubsystem.generated.h"

UCLASS()
class DEMO_API UDemoHitStopSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	// 顿帧：极短时间几乎完全静止，用来放大打击感
	UFUNCTION(BlueprintCallable, Category = "Demo|Feedback")
	void RequestHitStop(float Duration = 0.08f, float TimeDilation = 0.02f);

	// 时缓：完美闪避 / 破防瞬间放慢，给玩家反应窗口
	UFUNCTION(BlueprintCallable, Category = "Demo|Feedback")
	void RequestSlowMotion(float Duration = 0.8f, float TimeDilation = 0.35f);

	// 顿帧 → 紧接着慢动作，两段式。
	//
	// 为什么需要它：RequestHitStop 和 RequestSlowMotion 共用同一个"恢复时刻"，
	// 连续调用后者会直接覆盖前者 —— 于是"先冻住再放慢"的层次感完全消失，
	// 只剩一段普通时缓。完美弹反最需要的就是这个层次：
	//   瞬间冻住（撞击感）→ 缓慢回放（看清自己弹开了什么 + 有时间接反击）
	// 这里把慢动作登记为"待执行"，等顿帧结束后再启动。
	UFUNCTION(BlueprintCallable, Category = "Demo|Feedback")
	void RequestHitStopThenSlowMotion(float HitStopDuration, float HitStopDilation,
		float SlowMoDuration, float SlowMoScale);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|Feedback")
	bool IsTimeFrozen() const { return RestoreAtRealTime > 0.f; }

	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

private:
	float RestoreAtRealTime = 0.f;

	// 顿帧结束后要接着执行的慢动作（0 = 没有）
	float PendingSlowMoDuration = 0.f;
	float PendingSlowMoScale = 1.f;
};
