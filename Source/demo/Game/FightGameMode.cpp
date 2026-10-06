#include "Game/FightGameMode.h"
#include "UI/CombatHUDWidget.h"
#include "Player/FightPlayerCharacter.h"
#include "Boss/BossCharacterBase.h"

#include "Blueprint/UserWidget.h"
#include "Components/AudioComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundWave.h"
#include "TimerManager.h"

AFightGameMode::AFightGameMode()
{
	DefaultPawnClass = AFightPlayerCharacter::StaticClass();

	// 要驱动 HUD 的每帧刷新
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
}

void AFightGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// UMG 的 Tick 能不能跑取决于蓝图编译时写死的标志，不可靠（详见 UCombatHUDWidget::TickHUD）。
	// 这里显式驱动，保证血条/飘字/大招提示一定会刷新。
	if (HUDWidget)
	{
		HUDWidget->TickHUD(DeltaSeconds);
	}
	if (BossBarWidget)
	{
		BossBarWidget->TickHUD(DeltaSeconds);
	}

	// 每 1s 一次的"站位体检"。
	// 为什么必须有它：悬空 / 踩空 / BOSS 卡住不走 / 够不到玩家 这几类问题，
	// 画面上看着都一样（"BOSS 好像不动了"），光看血条和截图猜不出来。
	// 这一行给出双方的世界坐标 + 有没有踩在地上 + 距离 + 高度差，
	// 无头跑就能判断是"悬空"、"卡在台阶上"还是"仇恨范围外"。
	GroundProbeTimer -= DeltaSeconds;
	if (GroundProbeTimer <= 0.f)
	{
		GroundProbeTimer = 1.f;
		const APawn* PlayerPawn = UGameplayStatics::GetPlayerPawn(this, 0);
		const ABossCharacterBase* Boss = BoundBoss ? BoundBoss.Get() : FindBindableBoss();
		if (PlayerPawn || Boss)
		{
			const FVector PLoc = PlayerPawn ? PlayerPawn->GetActorLocation() : FVector::ZeroVector;
			const FVector BLoc = Boss ? Boss->GetActorLocation() : FVector::ZeroVector;
			// ⚠ APawn::GetMovementComponent() 返回的是 UPawnMovementComponent，
			//   而 IsMovingOnGround() 只在 UCharacterMovementComponent 上 ——
			//   不 include + Cast 的话是 C2027（类型不完整）。
			const UCharacterMovementComponent* PMove = PlayerPawn
				? Cast<UCharacterMovementComponent>(PlayerPawn->GetMovementComponent()) : nullptr;
			const UCharacterMovementComponent* BMove = Boss
				? Boss->GetCharacterMovement() : nullptr;
			const bool bPlayerGrounded = (PMove != nullptr) && PMove->IsMovingOnGround();
			const bool bBossGrounded = (BMove != nullptr) && BMove->IsMovingOnGround();
			UE_LOG(LogDemoCombat, Log,
				TEXT("[站位] 玩家 (%.0f, %.0f, %.0f) 踩地=%d | BOSS (%.0f, %.0f, %.0f) 踩地=%d | 距离=%.0f 高度差=%+.0f"),
				PLoc.X, PLoc.Y, PLoc.Z, bPlayerGrounded ? 1 : 0,
				BLoc.X, BLoc.Y, BLoc.Z, bBossGrounded ? 1 : 0,
				FVector::Dist(PLoc, BLoc), BLoc.Z - PLoc.Z);
		}
	}
}

// ==================================================================================
// BGM
//
// ⚠⚠ UE5.7 的"循环播放"只有一条路：**USoundWave::bLooping**
//   （源码注释：If set, when played directly (not through a sound cue)
//     the wave will be played looping）。
//   · UGameplayStatics::CreateSound2D 返回的是 **UAudioComponent**，它【没有 bLooping】
//   · USoundComponent（老的那种）在 5.7 的 Classes/Components 下【已经不存在】
//     → 网上抄的 `SoundComp->bLooping = true` 一模上去就编译不过（C2065 未声明）
//   · USoundBase::IsLooping() 的基类实现是 `GetDuration() >= INDEFINITELY_LOOPING_DURATION`，
//     对普通 SoundWave 不生效，所以必须看 USoundWave 自己那个 bLooping
//   所以：资产上勾 Looping（Tools/ImportBgm.py 已经勾了）+ 这里再兜底补一刀。
// ==================================================================================
void AFightGameMode::StartBgm()
{
	// 幂等：BeginPlay 可能被重复触发（PIE 多次 / 手工再调），已经在播就别叠第二个
	if (BgmComponent && BgmComponent->IsPlaying())
	{
		return;
	}

	USoundBase* Sound = BgmTrack.IsNull()
		? LoadObject<USoundBase>(nullptr, *DefaultBgmPath)
		: BgmTrack.LoadSynchronous();
	if (!Sound)
	{
		// 查音效路径：Content/BGM/BGM_BossTheme.uasset 是否存在
		// （Tools/ImportBgm.py 是生成它的脚本；没声音不影响战斗，所以只警告不崩）
		UE_LOG(LogDemoCombat, Warning,
			TEXT("[BGM] ✗ 没找到音频资产：BgmTrack=%s DefaultBgmPath=%s"),
			BgmTrack.IsNull() ? TEXT("(空→用兜底路径)") : *BgmTrack.ToString(),
			*DefaultBgmPath);
		return;
	}

	// 兜底：SoundWave 上的 Looping 没勾就在内存里补上（打包后资产不可写，
	// 但运行时改内存值对本次播放有效；正式做法还是编辑器里勾好存盘）
	if (USoundWave* Wave = Cast<USoundWave>(Sound))
	{
		if (!Wave->bLooping)
		{
			Wave->bLooping = true;
			UE_LOG(LogDemoCombat, Warning,
				TEXT("[BGM] 资产「%s」没勾 Looping，已在内存里补上（建议在编辑器里勾一次并保存）"),
				*Wave->GetName());
		}
	}
	else
	{
		UE_LOG(LogDemoCombat, Warning,
			TEXT("[BGM] 资产「%s」不是 SoundWave（是 %s），循环要靠 SoundCue 的 Loop 节点"),
			*Sound->GetName(), *Sound->GetClass()->GetName());
	}

	// ⚠ CreateSound2D 三个必须对的参数：
	//   bAutoDestroy = false                默认 true = 播完自毁 → 循环第二轮就没声音了
	//   bPersistAcrossLevelTransition = true 切关卡时不掐断（以后做无缝旅行也稳）
	BgmComponent = UGameplayStatics::CreateSound2D(
		this, Sound, FMath::Clamp(BgmVolume, 0.f, 4.f), /*PitchMultiplier=*/1.f,
		/*StartTime=*/0.f, /*ConcurrencySettings=*/nullptr,
		/*bPersistAcrossLevelTransition=*/true, /*bAutoDestroy=*/false);

	if (!BgmComponent)
	{
		// ⚠ 这里返回空最常见的原因是【音频设备没起来】，不是路径写错：
		//   命令行带 -nosound / 无头验证跑（RunHeadlessGame.py 默认就带 -NoSound）
		//   → LogAudio: "Audio Device Manager not initializing due to all audio being disabled"
		//   → CreateSound2D 直接返回空。判断依据：上面那行"✗ 没找到音频资产"没有出现
		//      就说明路径没问题、SoundWave 加载成功了。
		UE_LOG(LogDemoCombat, Warning,
			TEXT("[BGM] ✗ CreateSound2D 返回空 = 音频设备没起来（无头/带 -nosound 时属正常，"
			     "要在编辑器或打包版里听）"));
		return;
	}

	// CreateSound2D 内部已经 Play 过一次，这里再调一次是幂等的（已在播就重播到当前位置）
	BgmComponent->Play();

	UE_LOG(LogDemoCombat, Log, TEXT("[BGM] ▶ 循环播放：%s 时长=%.1fs 循环=%d 音量=%.2f"),
		*Sound->GetName(), Sound->GetDuration(),
		Sound->IsLooping() ? 1 : 0, BgmComponent->VolumeMultiplier);
}

void AFightGameMode::StopBgm()
{
	if (!BgmComponent)
	{
		return;
	}
	BgmComponent->Stop();
	BgmComponent->DestroyComponent();
	BgmComponent = nullptr;
	UE_LOG(LogDemoCombat, Log, TEXT("[BGM] ■ 已停止并销毁组件"));
}

void AFightGameMode::SetBgmVolume(float Volume)
{
	BgmVolume = FMath::Clamp(Volume, 0.f, 4.f);
	if (BgmComponent)
	{
		// 用 SetVolumeMultiplier 而不是直接写字段：它会通知音频设备立刻生效
		BgmComponent->SetVolumeMultiplier(BgmVolume);
	}
	UE_LOG(LogDemoCombat, Log, TEXT("[BGM] 音量 → %.2f"), BgmVolume);
}

void AFightGameMode::BeginPlay()
{
	Super::BeginPlay();

	// BGM 放在最前面：下面那条"没有 PlayerController 就 return"不能把音乐一起吞掉
	StartBgm();

	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PC)
	{
		UE_LOG(LogDemoCombat, Warning, TEXT("[HUD] 没有 PlayerController，HUD 不会创建"));
		return;
	}

	// 蓝图里配了就用蓝图，没配就用原生 C++ 类（两者布局都由 C++ 兜底生成）
	const TSubclassOf<UCombatHUDWidget> EffectiveHUDClass = HUDWidgetClass
		? HUDWidgetClass
		: TSubclassOf<UCombatHUDWidget>(UCombatHUDWidget::StaticClass());

	const TSubclassOf<UBossHealthBarWidget> EffectiveBossBarClass = BossBarWidgetClass
		? BossBarWidgetClass
		: TSubclassOf<UBossHealthBarWidget>(UBossHealthBarWidget::StaticClass());

	// 玩家 HUD 在最底层（ZOrder 0），BOSS 血条压在上面
	HUDWidget = CreateWidget<UCombatHUDWidget>(PC, EffectiveHUDClass);
	if (HUDWidget)
	{
		HUDWidget->AddToViewport(0);
	}

	BossBarWidget = CreateWidget<UBossHealthBarWidget>(PC, EffectiveBossBarClass);
	if (BossBarWidget)
	{
		BossBarWidget->AddToViewport(1);
	}

	// Pawn / BOSS 都可能比 GameMode 晚出现 → 轮询补绑
	TryBindWidgets();
	GetWorldTimerManager().SetTimer(BindTimerHandle, this, &AFightGameMode::TryBindWidgets,
		FMath::Max(BindCheckInterval, 0.1f), true);

	// 出生兜底：见 FightGameMode.h 里 FallbackSpawnDelay 的注释
	if (FallbackSpawnDelay > 0.f)
	{
		GetWorldTimerManager().SetTimer(FallbackSpawnTimerHandle, this,
			&AFightGameMode::EnsurePlayerExists, FallbackSpawnDelay, /*bLoop=*/false);
	}
}

// 玩家没能自己生出来（PlayerStart 被判为"被占据"）时，在这里兜底生成一个。
//
// ⚠⚠ 这条兜底链是被 /Game/Fab/FightArena 逼出来的，真实日志如下：
//     FindPlayerStart: PATHS NOT DEFINED or NO PLAYERSTART with positive rating
//     SpawnActor failed because of collision at [X=0 Y=0 Z=0.000] for [BP_FightPlayer_C]
//   而 PlayerStart 明明就摆在关卡里（脚本自检能看到，位置 Z=200）。
//
//   真因：那份 Fab 竞技场网格的碰撞体是一个【包住整个内部的大凸包】，
//   连内部空腔都算"被占据"。于是无论把出生点放在 Z=300 还是 Z=1500，
//   SpawnActor 都报 collision —— 抬高到半空中也没用。
//
//   解法（两步）：
//     ① 先在【凸包之外】（FallbackSpawnOutsideLocation，场地外的空地）生成 —— 那里必定空着；
//     ② 生成成功后用 SetActorLocation(bSweep=false, TeleportPhysics) 传送到场地内部。
//        这条路径【不做碰撞检测、不做 penetration 处理】，所以能进凸包内部。
void AFightGameMode::EnsurePlayerExists()
{
	AController* PC = UGameplayStatics::GetPlayerController(this, 0);
	if (!PC)
	{
		UE_LOG(LogDemoCombat, Warning, TEXT("[Spawn] 找不到 PlayerController，兜底生成跳过"));
		return;
	}
	if (PC->GetPawn())
	{
		return;  // 正常出生，不用管
	}

	// ① 在凸包外生成
	RestartPlayerAtTransform(PC, FTransform(FallbackSpawnRotation, FallbackSpawnOutsideLocation));
	APawn* Spawned = PC->GetPawn();

	if (!Spawned)
	{
		// 连场外都被占 → 逐级抬高再试（万一场外也有别的东西）
		static const float ExtraHeights[] = { 300.f, 800.f, 2000.f, 5000.f };
		for (float Extra : ExtraHeights)
		{
			const FVector Try = FallbackSpawnOutsideLocation + FVector(0.f, 0.f, Extra);
			RestartPlayerAtTransform(PC, FTransform(FallbackSpawnRotation, Try));
			Spawned = PC->GetPawn();
			if (Spawned)
			{
				// ⚠ 格式参数写在 TEXT() 【外面】—— TEXT 只是字符串字面量宏，
				//   写成 TEXT("...%f...", X) 会报 C4002 + C2338（格式串没有对应实参）。
				UE_LOG(LogDemoCombat, Warning, TEXT("[Spawn] 场外 +%.0fcm 处生成成功"), Extra);
				break;
			}
		}
	}

	if (!Spawned)
	{
		UE_LOG(LogDemoCombat, Warning,
			TEXT("[Spawn] ✗ 兜底生成彻底失败：场外 +0~5000cm 全部被占，检查关卡里是不是塞满了碰撞体"));
		return;
	}

	// ② 传送进场（bSweep=false → 不做碰撞检测，这是能进凸包内部的关键）
	//    传送到场地中心上方 300cm，让它自己落到兜底地板上。
	Spawned->SetActorLocation(FallbackSpawnLocation, /*bSweep=*/false, nullptr,
		ETeleportType::TeleportPhysics);

	UE_LOG(LogDemoCombat, Warning,
		TEXT("[Spawn] 玩家未能自动出生（PlayerStart 被场地凸包判为'被占据'）→ 已在场外生成并传送到 (%.0f, %.0f, %.0f)：%s"),
		FallbackSpawnLocation.X, FallbackSpawnLocation.Y, FallbackSpawnLocation.Z,
		*Spawned->GetName());
}

ABossCharacterBase* AFightGameMode::FindBindableBoss() const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	TArray<AActor*> Bosses;
	UGameplayStatics::GetAllActorsOfClass(World, ABossCharacterBase::StaticClass(), Bosses);

	for (AActor* Actor : Bosses)
	{
		if (ABossCharacterBase* Boss = Cast<ABossCharacterBase>(Actor))
		{
			if (!Boss->IsDead())
			{
				return Boss;
			}
		}
	}

	return nullptr;
}

void AFightGameMode::TryBindWidgets()
{
	// ---------------- 玩家 ----------------
	if (!bPlayerBound && HUDWidget)
	{
		if (AFightPlayerCharacter* Player = Cast<AFightPlayerCharacter>(UGameplayStatics::GetPlayerPawn(GetWorld(), 0)))
		{
			HUDWidget->BindToCharacter(Player);
			Player->SetCombatHUD(HUDWidget);
			bPlayerBound = true;

			UE_LOG(LogDemoCombat, Log, TEXT("[HUD] 玩家 HUD 已绑定到 %s"), *Player->GetName());
		}
	}

	// ---------------- BOSS ----------------
	// 只在"没绑过"或"绑的那只已经死了"时才去场景里找，稳态下几乎没有开销
	if (BossBarWidget && (!BoundBoss || BoundBoss->IsDead()))
	{
		if (ABossCharacterBase* Boss = FindBindableBoss())
		{
			BoundBoss = Boss;
			BossBarWidget->BindToBoss(Boss);

			UE_LOG(LogDemoCombat, Log, TEXT("[HUD] BOSS 血条已绑定到 %s"), *Boss->GetName());
		}
	}
}

void AFightGameMode::RebindWidgets()
{
	bPlayerBound = false;
	BoundBoss = nullptr;

	if (HUDWidget)
	{
		HUDWidget->BindToCharacter(nullptr);
	}
	if (BossBarWidget)
	{
		BossBarWidget->BindToBoss(nullptr);
	}

	TryBindWidgets();
}
