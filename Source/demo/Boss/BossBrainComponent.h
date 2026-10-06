// BOSS 大脑：状态机 + 阶段管理 + 大招调度 + 连招链
//
// 职责边界：这里只决定"什么时候出哪一招"，招式本身交给 GAS 技能执行（Abilities/BossAbilities.h）。
//
// 两阶段的设计（用户需求：单阶段 3 个攻击，2 个阶段，二阶段复用 + 强化原有招式，外加 1 个大招）：
//   阶段 1（血量 100% ~ Phase2Threshold）：招式池 = 近战挥砍 / 突进斩（远程弹幕已按需求移除）
//   阶段 2（血量 < Phase2Threshold）：**同一个招式池**（资产完全复用）+ 阶段 2 的招式池里多一个大招；
//                                    所有招式按 UBossAttackData 的 Phase2* 倍率自动强化（更疼、更快、弹更多、后摇更短）
//   → 好处：招式手感只需要调一套；二阶段的"变强"是数据驱动的，不用维护两份配置。
//
// 零配置也能打：Phases 留空时会自动构建一套内置默认阶段 + 招式（蒙太奇按约定路径加载），
// 想改数值就在编辑器里建数据资产覆盖它。
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameplayEffectTypes.h"
#include "Boss/BossCombatData.h"
#include "BossBrainComponent.generated.h"

class UBossAttackData;
class UBossPhaseData;
class UGameplayEffect;
class AAIController;
class ABossCharacterBase;
class UAbilitySystemComponent;

UENUM(BlueprintType)
enum class EBossState : uint8
{
	Idle			UMETA(DisplayName = "待机"),
	Chase			UMETA(DisplayName = "追击"),
	Reposition		UMETA(DisplayName = "走位"),
	Telegraph		UMETA(DisplayName = "起手预警"),
	Attack			UMETA(DisplayName = "出招中"),
	Recover			UMETA(DisplayName = "后摇"),
	Stagger			UMETA(DisplayName = "硬直"),
	PhaseTransition	UMETA(DisplayName = "阶段转换"),
	Dead			UMETA(DisplayName = "死亡")
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FDemoOnBossStateChanged, EBossState, OldState, EBossState, NewState);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDemoOnBossTelegraph, const UBossAttackData*, AttackData);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDemoOnBossPhaseChanged, int32, PhaseIndex);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FDemoOnBossUltimateStarted);

UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class DEMO_API UBossBrainComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UBossBrainComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Brain")
	EBossState GetState() const { return CurrentState; }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Brain")
	UBossAttackData* GetCurrentAttack() const { return CurrentAttack; }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Brain")
	int32 GetPhaseIndex() const { return CurrentPhaseIndex; }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Brain")
	int32 GetPhaseCount() const { return Phases.Num(); }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Brain")
	bool IsPhase2() const { return CurrentPhaseIndex > 0; }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Brain")
	UBossPhaseData* GetCurrentPhase() const;

	// 战斗目标：突进斩的冲刺方向、弹幕瞄准、技能取朝向全部统一从这里拿，
	// 避免每个技能各自 GetPlayerPawn 一遍（多玩家 / 换目标时会不一致）
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Brain")
	AActor* GetTarget() const { return CachedTarget; }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Brain")
	float GetDistanceToTarget() const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Brain")
	float GetUltimateCooldownRemaining() const { return UltimateCooldownRemaining; }

	// ---------------- 起手预警（HUD 读招用）----------------
	//
	// 这三条是"玩家能不能打出弹反"的前提：动作游戏里，
	// 玩家必须**看得见**敌人还有多久出招，才可能在正确时机按键。
	// 以前 OnAttackTelegraph 委托发了但没有任何监听者 ——
	// 玩家只能靠肉眼看动画猜时机，弹反自然变成玄学。
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Brain")
	bool IsTelegraphing() const { return CurrentState == EBossState::Telegraph; }

	// 起手预警进度 0~1（1 = 马上就要出招）
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Brain")
	float GetTelegraphProgress() const;

	// 本次起手的总时长（秒），配合进度可以算出"还剩几秒"
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Brain")
	float GetTelegraphTotalTime() const { return CachedTelegraphTime; }

	// 当前这招能不能被弹反 —— HUD 用它换颜色：
	// 金色 = 可以弹（该按右键），红色 = 弹不了（只能闪避）
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Brain")
	bool CanCurrentAttackBeParried() const;

	// 复仇：被玩家打满 N 下后由角色侧调用，强制立刻起手还手一次。
	// 仅在 BOSS 当前空闲（未在出招 / 起手 / 硬直 / 死亡 / 阶段转换）时才生效，避免打断正在进行的动作。
	UFUNCTION(BlueprintCallable, Category = "Boss|Brain")
	bool RequestRevengeAttack();

	// 阶段配置：按血量阈值从高到低填（[0]=1.0、[1]=0.6）。
	// 留空且 bAutoBuildDefaultPhases = true 时，会构建内置默认两阶段（零配置可玩）。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Brain|Phase")
	TArray<TObjectPtr<UBossPhaseData>> Phases;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Brain|Phase")
	bool bAutoBuildDefaultPhases = true;

	// 玩家进入这个距离才开打
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Brain")
	float AggroRange = 1600.f;

	// 超出这个距离就不出招了（否则会在半个地图外朝玩家丢弹幕，很出戏）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Brain")
	float MaxEngageRange = 2400.f;

	// 连续追击这么久还没进招式和距离就改为走位
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Brain")
	float RepositionDelay = 3.f;

	// 阶段转换演出期间的无敌 GE（可选，留空 = 演出期间可以白打，这是给玩家的输出窗口）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Brain|Phase")
	TSubclassOf<UGameplayEffect> PhaseTransitionInvincibleEffect;

	// 调试绘制：状态、目标连线、各招的距离带、大招冷却
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Debug")
	bool bDrawDebugBrain = false;

	// 调试：强制只出某一类招（-1 = 正常抽取）。
	// 0=近战挥砍 1=远程弹幕 2=突进斩 3=大招（用来单独调某一招的手感）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Debug")
	int32 DebugForceAttackType = -1;

	UPROPERTY(BlueprintAssignable, Category = "Boss|Events")
	FDemoOnBossStateChanged OnStateChanged;

	UPROPERTY(BlueprintAssignable, Category = "Boss|Events")
	FDemoOnBossTelegraph OnAttackTelegraph;

	UPROPERTY(BlueprintAssignable, Category = "Boss|Events")
	FDemoOnBossPhaseChanged OnPhaseChanged;

	UPROPERTY(BlueprintAssignable, Category = "Boss|Events")
	FDemoOnBossUltimateStarted OnUltimateStarted;

protected:
	virtual void BeginPlay() override;

	// ---------------- 状态机 ----------------
	void SetState(EBossState NewState);

	void UpdateIdle(float DeltaTime);
	void UpdateChase(float DeltaTime);
	void UpdateReposition(float DeltaTime);
	void UpdateTelegraph(float DeltaTime);
	void UpdateAttack(float DeltaTime);
	void UpdateRecover(float DeltaTime);
	void UpdateStagger(float DeltaTime);
	void UpdatePhaseTransition(float DeltaTime);

	// ---------------- 阶段 ----------------
	void UpdatePhase();
	void StartPhaseTransition(int32 NewPhaseIndex);
	void FinishPhaseTransition();
	void ApplyPhaseSettings();

	// ---------------- 选招 / 出招 ----------------
	UBossAttackData* PickAttack();
	// bIgnoreCooldown：固定背板取招时传 true。
	// 重复冷却是"随机选招时代"的东西（防止随机连出同一招）；固定背板的顺序是设计出来的，
	// 再让冷却插手只会把设计好的节奏打乱 —— 长冷却的招永远轮不到，短冷却的招反复出现。
	bool CanUseAttack(const UBossAttackData* Attack, float Distance, bool bIgnoreCooldown = false) const;
	// 返回 false = 技能没激活成功（技能没授予 / 被标签阻挡），调用方要自己兜底
	bool ExecuteAttack(UBossAttackData* Attack);
	bool ShouldUseUltimate() const;

	// 连招链：出招结束后按概率立刻接下一招（2~3 连），二阶段接续概率更高
	bool TryStartFollowUp();

	// ---------------- 移动 ----------------
	void MoveToTarget(float AcceptRadius);
	void MoveAwayFromTarget(float Distance);

	// 一旦确认这个关卡没有可用导航（缺 NavMesh），就别再反复调 AI 寻路了：
	// 每次失败的请求都可能中止移动、把刚下发的速度请求清掉，BOSS 会彻底不动。
	bool bNavPathUnavailable = false;

	// BOSS 进入战斗后【持续追踪玩家朝向】—— 不让它在出招过程中被绕后打空气。
	// 三个阶段的转向速率，按"读条要看得出来 / 出招别拧身体 / 后摇要回正"分配：
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Combat")
	float TelegraphTrackYawRate = 6.f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Combat")
	float AttackTrackYawRate = 2.2f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Combat")
	float RecoverTrackYawRate = 3.5f;

	// ⚠⚠ 追击（Chase）时的转向速率 —— 2026-10-03 新增，之前 Chase 状态【完全不转向】。
	// 视频实证：BOSS 全程背对玩家站着不动（影子都朝着侧面）。
	// 取 4.0：比出招时(2.2)快、比起手读条(6.0)慢 —— 追得上玩家绕后，但不会显得急躁。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Combat")
	float ChaseTrackYawRate = 4.f;

	// ⚠ 突进位移期间（bDashFacingLocked）与角色自身一次性转向期间会自动让路。
	void TrackTargetDuringCombat(float DeltaTime, float YawRate, bool bUseDeadZone);

	// 绕着玩家走位（出招间隔里的"对峙"行为）：切线方向侧移 + 把距离拉回理想值。
	// 存在的意义：BOSS 追到交战距离后别钉在原地站着 —— 那会让移动动画几乎看不到。
	void StrafeAroundTarget(float DesiredDistance, float DeltaTime);

	// 绕圈时保持的理想距离（各招的舒适区大致在这个量级）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Combat")
	float StrafeDistance = 260.f;

	// 隔多久随机换一次绕行方向（一直同向绕会像跑圈，太好预判）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Combat")
	float StrafeFlipInterval = 2.5f;

	float StrafeFlipTimer = 0.f;
	bool bStrafeClockwise = true;

	// 无导航直驱的节流日志时间戳。
	// ⚠ 2026-10-03 原来是函数里的 `static float` —— 那是【跨实例共享的全局】，
	//   多 BOSS / 重开关卡时会互相把日志节流掉，诊断信息会莫名消失。改成实例成员。
	float LastNoNavLogTime = -999.f;

	// 无导航直驱：没有 NavMesh（或寻路失败）时，用 AI 专用的速度请求通道驱动 BOSS 移动。
	// ⚠ 不能用 AddMovementInput —— 它只对【本地控制】的角色生效，AI 角色会被静默丢弃
	//   （引擎 CharacterMovementComponent.cpp:1723 的 IsLocallyControlled() 门）。
	void DriveMoveNoNav(ABossCharacterBase* Boss, const FVector& Dir, const TCHAR* Reason);
	void StopMovement();
	AAIController* GetAIController() const;
	ABossCharacterBase* GetBoss() const;
	UAbilitySystemComponent* GetBossASC() const;
	void UpdateDebugDraw() const;

	// 内置默认阶段 / 招式（Phases 为空时用）
	void EnsureDefaultPhases();
	UBossAttackData* MakeDefaultAttack(FName AttackName, EBossAttackType AttackType,
		const TCHAR* MontagePath, float IdealRange, float RangeTolerance, float Damage, float Cooldown);

private:
	EBossState CurrentState = EBossState::Idle;
	float StateTimer = 0.f;
	float ChaseTimer = 0.f;

	// 本次起手预警的总时长，进入 Telegraph 时算一次并缓存。
	// 缓存而不是让 HUD 自己算：起手时长取决于 招式基准值 × 阶段倍率 × 连招缩放 × 二阶段倍率，
	// 让 UI 重算一遍迟早会和 AI 走岔（预警条走完了人还没出招，最典型的读招失真）。
	float CachedTelegraphTime = 0.6f;

	UPROPERTY()
	TObjectPtr<UBossAttackData> CurrentAttack = nullptr;

	int32 CurrentPhaseIndex = 0;

	// 招式冷却，按招式名索引（用名字而不是指针当 key：不受 UObject 有效性影响，
	// 而且调试时 print 出来直接能看懂是哪一招；代价是招式名必须唯一）
	TMap<FName, float> AttackCooldowns;

	// 固定背板出招的游标：指向 AttackPattern 里"下一招"的位置。
	// 换阶段 / 重新开战时归零，保证背板从头开始（玩家能记住"第一招是什么"）。
	int32 PatternCursor = 0;

	// 出招间隔倒计时：回到 Chase 时按阶段的 AttackIntervalMin/Max 掷一次，
	// 走完之前不许选下一招。没有它，BOSS 打完一招只等招式后摇（0.55s）就立刻再打，
	// 于是只有冷却最短的那一招能反复用 —— 表现就是"一直在重复同一个动作"。
	float AttackIntervalRemaining = 0.f;

	// 上一次实际放出去的招式名：随机兜底时用它避免连着放同一招
	FName LastAttackName = NAME_None;

	UPROPERTY()
	TObjectPtr<AActor> CachedTarget = nullptr;

	// ---------------- 阶段转换 ----------------
	// 演出的子阶段：0 = 进入动作，1 = 循环动作，2 = 收力动作（-1 = 未开始）
	int32 TransitionStage = -1;
	FActiveGameplayEffectHandle TransitionInvincibleHandle;

	// ---------------- 大招调度 ----------------
	float PhaseElapsedTime = 0.f;
	float UltimateCooldownRemaining = 0.f;
	int32 UltimateUseCount = 0;

	// ---------------- 连招链 ----------------
	// 后摇开始的第 1 帧掷一次骰子决定要不要接下一招（bFollowUpDecided 保证只掷一次）
	bool bFollowUpDecided = false;

	UPROPERTY()
	TObjectPtr<UBossAttackData> PendingFollowUp = nullptr;

	float FollowUpTelegraphScale = 1.f;

	// ---------------- 诊断 ----------------
	// 心跳计时器：每 1.5s 打一条「状态 / 停留时长 / 距离 / 出招间隔剩余」，
	// 用来定位"BOSS 打完一招后再也不出招"到底卡在哪个状态（2026-09-30 加）
	float HeartbeatTimer = 0.f;
};
