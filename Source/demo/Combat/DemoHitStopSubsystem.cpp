#include "Combat/DemoHitStopSubsystem.h"
#include "Kismet/GameplayStatics.h"

void UDemoHitStopSubsystem::RequestHitStop(float Duration, float TimeDilation)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const float NewTime = World->GetRealTimeSeconds() + Duration;

	// 已有更长的冻结时不覆盖，避免连续命中互相打断
	if (NewTime <= RestoreAtRealTime)
	{
		return;
	}

	RestoreAtRealTime = NewTime;
	UGameplayStatics::SetGlobalTimeDilation(World, TimeDilation);
}

void UDemoHitStopSubsystem::RequestSlowMotion(float Duration, float TimeDilation)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const float NewTime = World->GetRealTimeSeconds() + Duration;

	if (NewTime <= RestoreAtRealTime)
	{
		return;
	}

	RestoreAtRealTime = NewTime;
	UGameplayStatics::SetGlobalTimeDilation(World, TimeDilation);
}

void UDemoHitStopSubsystem::RequestHitStopThenSlowMotion(float HitStopDuration, float HitStopDilation,
	float SlowMoDuration, float SlowMoScale)
{
	// 先登记慢动作，再触发顿帧 —— Tick 里顿帧一结束就会自动接上
	PendingSlowMoDuration = FMath::Max(SlowMoDuration, 0.f);
	PendingSlowMoScale = FMath::Clamp(SlowMoScale, 0.01f, 1.f);

	RequestHitStop(HitStopDuration, HitStopDilation);
}

void UDemoHitStopSubsystem::Tick(float DeltaTime)
{
	if (RestoreAtRealTime <= 0.f)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// 用真实时间判定，否则慢动作下永远恢复不了
	if (World->GetRealTimeSeconds() >= RestoreAtRealTime)
	{
		// 顿帧刚结束 → 如果登记了待执行的慢动作，立刻接上（这就是"两段式"的全部实现）
		if (PendingSlowMoDuration > 0.f)
		{
			const float Duration = PendingSlowMoDuration;
			const float Scale = PendingSlowMoScale;
			PendingSlowMoDuration = 0.f;
			PendingSlowMoScale = 1.f;

			RestoreAtRealTime = 0.f;          // 先清空，否则 RequestSlowMotion 会被"已有更长冻结"挡掉
			RequestSlowMotion(Duration, Scale);
			return;
		}

		UGameplayStatics::SetGlobalTimeDilation(World, 1.f);
		RestoreAtRealTime = 0.f;
	}
}

TStatId UDemoHitStopSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UDemoHitStopSubsystem, STATGROUP_Tickables);
}
