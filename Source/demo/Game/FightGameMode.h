// 战斗关卡的 GameMode：负责生成 HUD 并把它们绑到玩家 / BOSS 上
//
// 两条"零配置"原则：
//   1. HUDWidgetClass / BossBarWidgetClass 留空也能跑 —— 自动退回纯 C++ 的 HUD 类。
//      也就是说：就算 BP_FightGameMode 是空的，进 PIE 也能看到双方血条。
//   2. 绑定是"轮询补绑"，不是一次性绑定 —— Pawn 和 BOSS 都可能比 GameMode 晚出现
//      （尤其 BOSS 是关卡里延迟生成 / 死亡后重刷时），一次性绑定会漏。
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "FightGameMode.generated.h"

class UCombatHUDWidget;
class UBossHealthBarWidget;
class ABossCharacterBase;
class USoundBase;
class UAudioComponent;

UCLASS()
class DEMO_API AFightGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AFightGameMode();

	// HUD 的每帧刷新由这里驱动（不依赖 UMG 的 Tick 开关，见 UCombatHUDWidget::TickHUD 注释）
	virtual void Tick(float DeltaSeconds) override;

	// ---------- BGM ----------
	//
	// 为什么放在 GameMode 而不是关卡里摆 AudioComponent：
	//   · 关卡里摆 AudioComponent 要手工拖、换关卡还得重摆；GameMode::BeginPlay 一定会跑
	//   · CreateSound2D(..., bPersistAcrossLevelTransition=true, bAutoDestroy=false)
	//     保证切关卡不掐断、播完也不自毁
	//
	// 循环播放在 UE5.7 靠【SoundWave 资产的 Looping 勾】（不是组件上的开关，
	// UAudioComponent 根本没有 bLooping —— 详见 .cpp 里那段警告）。
	//
	// 资产：Content/BGM/BGM_BossTheme.uasset（Tools/ImportBgm.py 从 mp3 导入）
	// BgmTrack 留空时自动回落到 DefaultBgmPath，所以"零配置"也能响。
	//
	// ⚠ 这几个必须放 public：本项目踩过 protected 的 UFUNCTION 外部调不到（C2248）。
	UFUNCTION(BlueprintCallable, Category = "Demo|Audio")
	void StartBgm();

	UFUNCTION(BlueprintCallable, Category = "Demo|Audio")
	void StopBgm();

	/** 运行中改音量（0~4），就地更新，不用重开。*/
	UFUNCTION(BlueprintCallable, Category = "Demo|Audio")
	void SetBgmVolume(float Volume);

	/** 当前 BGM 组件（没配声音时为 nullptr）。给 UI 做淡入淡出用。*/
	UFUNCTION(BlueprintPure, Category = "Demo|Audio")
	UAudioComponent* GetBgmComponent() const { return BgmComponent; }

protected:
	virtual void BeginPlay() override;

	// ---------- BGM 资产（2026-10-06）----------
	//
	// BgmTrack 可以在 BP_FightGameMode 里直接换曲子；留空就回落到 DefaultBgmPath。
	// 两条路都留的原因：C++ 默认值压不过蓝图 CDO 的旧值（本项目踩过），
	// 留空 + 硬编码兜底路径 = 即使 BP 里存的是陈旧值也照样能响。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Audio")
	TSoftObjectPtr<USoundBase> BgmTrack;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Audio")
	FString DefaultBgmPath = TEXT("/Game/BGM/BGM_BossTheme.BGM_BossTheme");

	// 起始音量。0.35 = "有氛围但压得住打斗音效"的试听值，可在 BP 里改或运行时 SetBgmVolume
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Audio")
	float BgmVolume = 0.35f;

	UPROPERTY(Transient)
	TObjectPtr<UAudioComponent> BgmComponent;

	// 留空 → 用原生 UCombatHUDWidget（C++ 自建布局，一样能显示血条）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|UI")
	TSubclassOf<UCombatHUDWidget> HUDWidgetClass;

	// 留空 → 用原生 UBossHealthBarWidget
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|UI")
	TSubclassOf<UBossHealthBarWidget> BossBarWidgetClass;

	// 补绑检查间隔（秒）。稳态下开销只有"玩家 Pawn 命中判断"这一下
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|UI")
	float BindCheckInterval = 0.5f;

	// ---------- 出生兜底（2026-10-04）----------
	//
	// 为什么需要：换新关卡（/Game/Fab/FightArena）时实测踩到的坑 ——
	//   APlayerStart::IsSpawnAllowed() 会在玩家胶囊尺寸下做 EncroachingBlockingGeometry
	//   检查，出生点只要被场地里任何凸起 / 道具判为"被占据"，ChoosePlayerStart 就返回 null，
	//   引擎打印 "FindPlayerStart: PATHS NOT DEFINED or NO PLAYERSTART with positive rating"，
	//   然后退回用 WorldSettings 的原点生成玩家 —— 那里通常也被占，于是：
	//     "SpawnActor failed because of collision" → 整局没有玩家，只剩空场景。
	// 这个兜底保证"无论关卡怎么摆，玩家一定能出来"。
	//
	// 延迟几秒再检查：GameMode 的 PostLogin / RestartPlayer 都在 BeginPlay 之后才跑，
	// 太早判定会误以为玩家没生成。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Spawn")
	float FallbackSpawnDelay = 2.5f;

	// 兜底出生位置（世界坐标）。放在场地中心、离地 300cm 让它自然落下。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Spawn")
	FVector FallbackSpawnLocation = FVector(0.0, 0.0, 300.0);

	// 实际执行 SpawnActor 的位置：必须在【场地碰撞凸包之外】，否则 SpawnActor 报 collision。
	// 生成成功后再传送到 FallbackSpawnLocation。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Spawn")
	FVector FallbackSpawnOutsideLocation = FVector(0.0, -9000.0, 400.0);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Spawn")
	FRotator FallbackSpawnRotation = FRotator(0.0, 0.0, 0.0);

	// 检查并兜底生成玩家（由定时器调用）
	void EnsurePlayerExists();

	UPROPERTY(BlueprintReadOnly, Category = "Demo|UI")
	TObjectPtr<UCombatHUDWidget> HUDWidget;

	UPROPERTY(BlueprintReadOnly, Category = "Demo|UI")
	TObjectPtr<UBossHealthBarWidget> BossBarWidget;

	UPROPERTY(BlueprintReadOnly, Category = "Demo|UI")
	TObjectPtr<ABossCharacterBase> BoundBoss;

	// 手动重绑（例如玩家复活换了 Pawn）
	UFUNCTION(BlueprintCallable, Category = "Demo|UI")
	void RebindWidgets();

private:
	void TryBindWidgets();

	// 找场景里第一只还活着的 BOSS（都死了就返回 nullptr，等下一只刷出来再绑）
	ABossCharacterBase* FindBindableBoss() const;

	FTimerHandle BindTimerHandle;
	FTimerHandle FallbackSpawnTimerHandle;
	bool bPlayerBound = false;

	// 每 1s 记一次的"站位体检"节流（见 .cpp 里的说明：悬空/卡住这类问题只能靠它定位）
	float GroundProbeTimer = 0.f;
};
