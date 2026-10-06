// BOSS 出招技能：由 UBossBrainComponent 动态指定招式数据后激活
//
// 四种招式各对应一个技能，具体走哪个由 UBossAttackData::AttackType 决定：
//   UGA_BossAttack        近战挥砍  —— 播蒙太奇 + 按比例开命中窗口（复用基类的武器扫掠）
//   UGA_BossRangedAttack  远程弹幕  —— 施法动作 → 扇形 / 环形齐射投射物（带拖尾弹道）
//   UGA_BossChargeSlash   突进斩    —— 蓄势 → 匀速突进拉近 → 收尾重斩（扇形）+ 冲击 + 震荡波
//   UGA_BossUltimate      大招      —— 蓄力 → 多圈环形弹幕 → 突进斩收尾
//
// 四个贯穿全文的设计要点：
//  1) 所有位移都是"真位移"（根位移 / 根运动源），没有"动画推着 Mesh 走"，
//     因此不会出现播完被拉回、胶囊体没动、相机不跟的情况。
//  2) 二阶段强化不另建技能：同一个技能读 UBossAttackData::GetEffective*(bPhase2) 的倍率即可，
//     bPhase2 由 Boss 身上的 State.Boss.Phase2 标签决定 —— 招式资产可以完全复用。
//  3) 命中判定窗口用 C++ 按"蒙太奇时长比例"驱动，不依赖蒙太奇上拖的通知：
//     Boss 的蒙太奇是脚本按源动画生成的，逐个手拖通知既麻烦又容易漏。
//  4) 位移距离一律用 UAbilityTask_ApplyRootMotionConstantForce 的"匀速推固定距离"，
//     不用 LaunchCharacter 给初速度 —— 后者的实际距离取决于摩擦/减速度，外部不可见也调不准。
#pragma once

#include "CoreMinimal.h"
#include "Abilities/DemoGameplayAbility.h"
#include "Boss/BossCombatData.h"
#include "BossAbilities.generated.h"

class UAnimMontage;
class UAbilityTask_PlayMontageAndWait;
class UAbilityTask_WaitDelay;
class UAbilityTask_ApplyRootMotionConstantForce;
class ABossCharacterBase;
class UCharacterMovementComponent;

// ================================================================== 基类
UCLASS(Abstract)
class DEMO_API UGA_BossAttackBase : public UDemoGameplayAbility
{
	GENERATED_BODY()

public:
	UGA_BossAttackBase();

	// 由 UBossBrainComponent 在 TryActivateAbility 之前塞进来
	UFUNCTION(BlueprintCallable, Category = "Boss|Ability")
	void SetPendingAttack(UBossAttackData* InAttack);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Ability")
	UBossAttackData* GetAttackData() const { return PendingAttack; }

protected:
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;

	virtual void EndAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		bool bReplicateEndAbility, bool bWasCancelled) override;

	// 子类在这里做真正的出招（基类已完成：取数据、取 Boss、写入攻击参数）
	virtual void OnAttackStart() {}

	// ---------------- 取用 ----------------
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Ability")
	ABossCharacterBase* GetBoss() const;

	// 当前是否二阶段（Boss 身上有 State.Boss.Phase2 标签）
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Ability")
	bool IsPhase2() const;

	// ---- 二阶段强化后的实际参数（一阶段自动退化为原始值）----
	float GetDamage() const;
	float GetPoiseDamage() const;
	float GetPlayRate() const;

	UCharacterMovementComponent* GetMoveComp() const;

	// ---------------- 通用动作 ----------------
	// 播放主蒙太奇（会自动停掉上一个主蒙太奇任务，避免"切段被当成被打断"）
	// 单节点关键修复：不再返回 UAbilityTask_PlayMontageAndWait（单节点下其蒙太奇实例不推进、
	// 永不回调 → 能力卡死），改为走 Boss->PlayBossMontage（PlayAnimation 设为当前资产 + 定时器回调）。
	bool PlayMontageTask(UAnimMontage* Montage, float Rate = 1.f,
		FName TaskName = TEXT("BossMontage"), float RootMotionScale = 1.f);

	// 主蒙太奇"播完"回调（由 Boss->PlayBossMontage 的定时器触发，等价原 UAbilityTask 的 OnCompleted）
	UFUNCTION()
	void HandleMontageTaskFinished(UAnimMontage* FinishedMontage);

	// 停掉主蒙太奇任务（单节点下改为让 Boss 停掉 CurrentAsset 蒙太奇 + 取消"播完"定时器）
	void StopActiveMontageTask();

	// 按蒙太奇时长比例开 / 关命中判定窗口（近战挥砍，以及突进斩的收尾斩）
	void SetupMeleeHitWindow(UAnimMontage* Montage, float PlayRate);

	// 立刻开关命中判定窗口（拖刀这类"时长由代码决定"的场景用）
	void SetMeleeHitWindow(bool bOpen);

	// 齐射一波弹幕。CountOverride < 0 时用 Params.ProjectileCount。
	// 返回实际生成的弹数（0 = 发射失败，日志里会说明原因）。
	int32 FireBarrage(const FBossBarrageParams& Params, int32 CountOverride = INDEX_NONE,
		float SpeedScale = 1.f, float AngleOffsetDeg = 0.f);

	// 起手前转向目标（只转 Yaw，俯仰不动）
	void FaceTarget();

	// 把自身移速压到 0 / 还原（原地挥砍、原地施法用）
	void PinMovement(bool bPin);

	// ---------------- 贴地（杜绝"BOSS 飞起来"）----------------
	//
	// 出招期间把 BOSS 的运动约束到一个【过当前位置的水平面】上，Z 轴彻底锁死。
	//
	// 为什么需要这一层：
	//   招式蒙太奇全部取自 SwordAnimsetPro 的 Root-Motion 目录，其中冲刺 / 重击类动作的
	//   根位移带垂直分量（起身、跃砍）。只要那一帧的根位移被提取，胶囊体就会被抬离地面 ——
	//   表现出来就是"BOSS 飘着打"。而且它是连锁伤害：胶囊体一浮空，
	//   玩家的武器扫掠高度就够不着它，于是"看着砍中了却不掉血"。
	//   把 Z 锁住比逐招去改动画安全得多，也不会影响寻路 / 转向 / 水平突进。
	//
	// 进入前的平面约束设置会被存下来，退出时原样还原（不写死任何值）。
	void SetBossGrounded(bool bGrounded);

	// 结束整段出招的统一出口（突进还在进行时不会结束）
	void FinishAttack();

	// ---------------- 突进（突进斩收尾 / 大招收尾共用）----------------
	//
	// 实现：ApplyRootMotionConstantForce，Strength = 距离 / 时长 → 恰好前进"距离"厘米，
	// 匀速、碰撞安全、结束时速度归零（收招不会继续滑）。
	// 位移完全由这个任务给出，所以突进蒙太奇的根位移缩放会被设成 0 ——
	// 否则"动画位移 + 代码位移"叠加 = 冲出去两倍距离，而且方向会打架。
	//
	// 方向与距离都在这里算好：目标是玩家当前位置，但会留出 StopDistance，
	// 让收尾重斩看起来是砍在刀尖上，而不是整个人糊到玩家身上。
	void BeginChargeDash(const FBossChargeParams& InCharge, float EffectiveDistance,
		UAnimMontage* DashMontage);

	// 突进结束 → 结算收尾重斩（扇形判定 + 击退 + 可选震荡波）
	void FinishChargeImpact();

	bool IsDashing() const { return bDashActive; }

	// ---------------- 回调 ----------------
	// 命中判定窗口的开 / 关（由 WaitDelay 任务按蒙太奇时长比例驱动）
	UFUNCTION()
	void OnHitWindowOpen();

	UFUNCTION()
	void OnHitWindowClose();

	UFUNCTION()
	virtual void OnMontageCompleted();

	UFUNCTION()
	virtual void OnMontageCancelled();

	// 突进任务结束
	UFUNCTION()
	virtual void OnDashFinished();

	// 突进超时保险：根运动任务万一没回调（技能被打断 / 角色被卡住），也不会把技能吊死
	UFUNCTION()
	virtual void OnDashTimeout();

	UPROPERTY()
	TObjectPtr<UBossAttackData> PendingAttack = nullptr;

	// 主蒙太奇是否正在播放（单节点模式下由 Boss->PlayBossMontage 的定时器驱动，不再持有 UAbilityTask）
	bool bMontageTaskActive = false;
	UPROPERTY() TObjectPtr<UAbilityTask_WaitDelay> ActiveHitOnTask = nullptr;
	UPROPERTY() TObjectPtr<UAbilityTask_WaitDelay> ActiveHitOffTask = nullptr;
	UPROPERTY() TObjectPtr<UAbilityTask_WaitDelay> ActiveDelayTask = nullptr;
	UPROPERTY() TObjectPtr<UAbilityTask_WaitDelay> ActiveDashTimeoutTask = nullptr;

	// 根运动源位移任务（突进 / 后撤）。用 UAbilityTask 这一层存是为了不必给每个具体任务类型加前向声明。
	UPROPERTY() TObjectPtr<UAbilityTask> ActiveRootMotionTask = nullptr;

	// ---- 突进运行状态 ----
	bool bDashActive = false;
	FBossChargeParams ActiveCharge;
	FVector ChargeTargetLocation = FVector::ZeroVector;

	// ---- 贴地状态（SetBossGrounded 用）----
	// 只在第一次进入时保存，避免嵌套调用把"原始值"覆盖成"被我们改过的值"
	bool bGroundedApplied = false;
	bool bSavedConstrainToPlane = false;
	FVector SavedPlaneConstraintNormal = FVector::UpVector;
	FVector SavedPlaneConstraintOrigin = FVector::ZeroVector;

	// 出招期间是否把移速压到 0
	bool bMovementPinned = false;
	float SavedMaxWalkSpeed = 0.f;
};

// ================================================================== 近战挥砍
UCLASS()
class DEMO_API UGA_BossAttack : public UGA_BossAttackBase
{
	GENERATED_BODY()

public:
	UGA_BossAttack();

protected:
	virtual void OnAttackStart() override;

	// 挥砍时钉住移动：前冲由动画根位移提供，再让 AI 同时走位就会"脚底打滑"
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Melee")
	bool bPinMovementWhileSwinging = true;
};

// ================================================================== 远程弹幕
UCLASS()
class DEMO_API UGA_BossRangedAttack : public UGA_BossAttackBase
{
	GENERATED_BODY()

public:
	UGA_BossRangedAttack();

protected:
	virtual void OnAttackStart() override;

	// 施法期间钉住移动（否则会边放弹幕边往玩家脸上走，读招窗口就没了）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Ranged")
	bool bPinMovementWhileCasting = true;

	// 齐射后是否再补一段后撤（弹幕型 BOSS 拉开距离的手段）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Ranged")
	bool bBackstepAfterFire = false;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Ranged")
	float BackstepDistance = 300.f;

	// 后撤耗时（秒）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Ranged")
	float BackstepDuration = 0.22f;

	UFUNCTION()
	void OnFireWindow();

	UFUNCTION()
	void OnBackstepWindow();

private:
	int32 FiredCount = 0;
	bool bBackstepped = false;
};

// ================================================================== 突进斩
//
// 三段式：
//   ① 蓄势（WindupTime）—— 钉住不动 + 预警条，这是玩家唯一能"弹反"的窗口
//   ② 突进（Charge.ChargeDuration）—— 匀速冲向玩家，可选的拖刀判定
//   ③ 收尾重斩 —— 扇形范围伤害 + 击退 + 可选震荡波
UCLASS()
class DEMO_API UGA_BossChargeSlash : public UGA_BossAttackBase
{
	GENERATED_BODY()

public:
	UGA_BossChargeSlash();

protected:
	virtual void OnAttackStart() override;

	// 蓄势时长（秒）。这段时间 BOSS 站着不动，给玩家读招 / 走位。
	// ⚠ 它不替代 Brain 的 TelegraphTime —— 那个是"预警条"的长度，
	//   这里再停一下是为了让"抬刀蓄力"这个动作本身也被看见。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Charge")
	float WindupTime = 0.28f;

	// 收尾斩播完后是否把收招动作也播完（false = 结算完立刻回 AI）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Charge")
	bool bPlayImpactMontage = true;

	UFUNCTION()
	void OnWindupFinished();

	// 突进结束 → 交给基类结算收尾冲击
	virtual void OnDashFinished() override;

	// 收尾蒙太奇播完 → 结束技能
	virtual void OnMontageCompleted() override;
};

// ================================================================== 大招
UCLASS()
class DEMO_API UGA_BossUltimate : public UGA_BossAttackBase
{
	GENERATED_BODY()

public:
	UGA_BossUltimate();

protected:
	// 大招步骤机：蓄力 → 循环蓄力 → 弹幕圈 → 收力 → （可选）突进斩收尾
	enum class EUltStep : uint8
	{
		ChargeIn,
		ChargeLoop,
		Rings,
		ChargeOut,
		ChargeFinisher,
		Done
	};

	virtual void OnAttackStart() override;
	virtual void OnMontageCompleted() override;

	// 结束前要把 State.Boss.Charging 标记摘掉，否则 HUD 会一直显示"蓄力中"
	virtual void EndAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		bool bReplicateEndAbility, bool bWasCancelled) override;

	// 蓄力期间把移速压到 0（站桩蓄力，玩家能安心跑位）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Ultimate")
	bool bPinMovementWhileCharging = true;

	// 每圈弹幕的发射高度偏移（从胸口高度往外扩散，视觉上更像"爆发"）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Ultimate")
	float RingSpawnHeight = 60.f;

	// 大招是否收尾接突进斩（还要数据资产的 bEndWithChargeFinisher 也为真）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Ultimate")
	bool bAllowChargeFinisher = true;

	void StartCharge();
	void EnterChargeLoop();
	void EnterRings();
	void RunRingStep();
	void EnterChargeOut();
	void EnterChargeFinisher();
	void FinishUltimate();

	UFUNCTION()
	void OnRingIntervalFinished();

	// 收尾突进：结算完冲击即结束大招（不再走基类的"再播收招动作"）
	virtual void OnDashFinished() override;

	EUltStep Step = EUltStep::ChargeIn;
	float ChargeElapsed = 0.f;
	int32 RingsFired = 0;
	bool bUltimateChargeFinisher = false;
};
