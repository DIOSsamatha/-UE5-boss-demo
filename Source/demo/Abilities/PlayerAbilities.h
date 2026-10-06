// 玩家持剑技能集：连段普攻 / 技能 / 格挡 / 弹反 / 闪避
#pragma once

#include "CoreMinimal.h"
#include "Abilities/DemoGameplayAbility.h"
#include "GameplayEffectTypes.h"
#include "PlayerAbilities.generated.h"

class UAnimMontage;
class UAbilityTask_PlayMontageAndWait;
class UAbilityTask_WaitDelay;
class UAbilityTask_WaitGameplayEvent;
class UAbilityTask_ApplyRootMotionConstantForce;
class AFightPlayerCharacter;
class UAbilitySystemComponent;

// ------------------------------------------------------------------ 普通攻击连段
UCLASS()
class DEMO_API UGA_PlayerNormalAttack : public UDemoGameplayAbility
{
	GENERATED_BODY()

public:
	UGA_PlayerNormalAttack();

	// 连段每一段的蒙太奇，按顺序播放
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Combo")
	TArray<TObjectPtr<UAnimMontage>> ComboMontages;

	// 与 ComboMontages 一一对应的伤害
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Combo")
	TArray<float> ComboDamages;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Combo")
	float PoiseDamage = 12.f;

	// 连段窗口开放后，超过这个时间（秒）还没点左键就回退至普通攻击第一段
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Combo")
	float ComboResetDelay = 0.5f;

	// 攻击时角色的移动上限（占满速的比例）。
	// 注意：攻击时 WASD 输入本身就被锁（FightPlayerCharacter::SetMoveInputLocked），
	// 所以这里【不是】“边走边砍”的开关，而是控制“起手瞬间刹车有多硬”：
	//   0.0 = 把 MaxWalkSpeed 直接置 0 → 速度被瞬间钳到 0，角色像“钉”在原地（生硬）
	//   0.3 = 保留 30% 上限 → 速度平滑衰减停下（更自然），实际仍不会移动（输入被锁）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Combo")
	float AttackMoveSpeedScale = 0.3f;

	// 收招解除比例：每段动画播放到这个进度（0~1）后，提前把移动权还给玩家。
	// 例：0.70 = 最后 30% 的收招动作期间就能立刻走/跑，消除“打完愣一下才能动”的滞涩感。
	// 设 1.0 = 必须播完整段才能动（旧行为）。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Combo", meta = (ClampMin = "0.3", ClampMax = "1.0"))
	float ComboRecoverRatio = 0.70f;

	// 普攻根运动位移缩放：
	//   1.0 = 完整使用动画自带的前冲位移（前冲是真实作用到角色的，不会回弹）
	//   0.5 = 位移减半；0   = 完全原地挥砍（不位移、也不回弹）
	// 生效方式（两条同时给，都是同一个数）：
	//   ① PlayMontageAndWait 的第 7 个参数 AnimRootMotionTranslationScale；
	//   ② ACharacter::SetAnimRootMotionTranslationScale()（技能结束时还原为 1）。
	// ⚠ 前提：攻击蒙太奇引用的源动画必须勾 Enable Root Motion（见 Tools/SetupAttackComboMontages.py），
	//   否则蒙太奇没有根位移数据 —— 动画的位移会直接推着 Mesh 走，播完弹回原位，表现为“拉回”。
	//   运行时会打印 `[Combo] 第 N 段：HasRootMotion=?` 便于核对。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Combo")
	float AttackRootMotionScale = 1.f;

	// 连段加速：每多打一段，播放速率增加这么多（让连续点击时连段越来越快、更流畅）。0 = 不加速。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Combo")
	float ComboSpeedupPerStage = 0.12f;

	// 连段窗口提前开放的比例（相对本段时长）。越小=越早能接下一段 → 连段更紧凑跟手。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Combo")
	float ComboWindowOpenRatio = 0.20f;

protected:
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;

	virtual void EndAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		bool bReplicateEndAbility, bool bWasCancelled) override;

private:
	void PlayCurrentCombo();

	UFUNCTION() void OnMontageCompleted();
	UFUNCTION() void OnMontageCancelled();
	// 连段窗口 / 命中窗口改为 C++ 计时驱动（不再依赖蒙太奇里的 ComboWindow 通知）
	UFUNCTION() void OnComboWindowOpen();
	UFUNCTION() void OnComboWindowClose();
	UFUNCTION() void OnMeleeWindowOpen();
	UFUNCTION() void OnMeleeWindowClose();
	UFUNCTION() void OnComboInput(FGameplayEventData Payload);

	// 收招窗口：动画播到 ComboRecoverRatio 后触发 → 提前把移动权还给玩家（恢复移速 + 解锁 WASD）
	UFUNCTION() void OnRecoveryWindowOpen();

	// 即时跳到下一段：结束当前蒙太奇、立刻播放下一段（让连点非常跟手、不卡顿）
	UFUNCTION() void AdvanceToNextCombo();

	// 锁住 WASD 输入 + 压低移速上限（每段开始时调用；默认压到 AttackMoveSpeedScale，平滑刹车不“钉住”）
	void LockMovement();

	// 把攻击期间压低/锁住的移动能力还原（收招窗口和 EndAbility 都会调用，幂等）
	void RestoreMovement();

	int32 ComboIndex = 0;
	bool bComboWindowOpen = false;

	// 贪刀状态：连段打满一轮（第4段）之后还继续按、段数绕回第 1 段 → 置 true。
	// 之后每一刀都后摇更长、伤害更低，直到本次技能结束。
	// 目的不是禁止连点，而是让"见好就收"变成一个真实决策 ——
	// 连点仍然能打，只是越往后越亏。
	bool bOvercommitted = false;
	bool bBufferedNext = false;        // 窗口未开时提前点左键 → 缓冲，等窗口一开立即续段
	double BufferedInputTime = 0.0;    // 缓冲按下时刻（世界秒）。超过 ComboBufferExpiryTime 的陈旧输入直接丢弃，
	                                   // 否则"这一段刚起手时误触的那一下"会在窗口打开时突然自动接段
	bool bAdvancing = false;           // 正在“即时跳段”过程中，忽略旧蒙太奇的完成/取消回调

	// ⚠⚠⚠ 2026-10-03：普攻是【多段连招】，每一段都会调 LockMovement()（见 PlayCurrentCombo），段数最多 4。
	// 移动锁现在是【引用计数】——如果这里还用单个 bool 记账，就会"锁了 4 次只减 1 次"
	// → 计数永久多 3 层 → **攻击后角色彻底动不了**。
	// （这是我把移动锁改成引用计数时引入的回归：bool 时代重复 Lock 无害，计数时代会累积。）
	// 正确做法：记【本技能累计加锁了几次】，RestoreMovement 一次性全部减掉。
	int32 MovementLockHeld = 0;
	// 移速是否已被本技能压低过。防止四段连招把 MaxWalkSpeed 叠成 0.3^4 ≈ 0.008（等于原地钉死）。
	bool bSpeedReduced = false;
	float SavedMaxWalkSpeed = 0.f;     // EndAbility 时恢复用的原移速

	// 当前段正在跑的蒙太奇 / 窗口计时器任务；跳段时先结束它们，避免串到新一段
	UPROPERTY() TObjectPtr<UAbilityTask_PlayMontageAndWait> ActiveMontageTask = nullptr;
	UPROPERTY() TObjectPtr<UAbilityTask_WaitDelay> ActiveHitOn = nullptr;
	UPROPERTY() TObjectPtr<UAbilityTask_WaitDelay> ActiveHitOff = nullptr;
	UPROPERTY() TObjectPtr<UAbilityTask_WaitDelay> ActiveWinOpen = nullptr;
	UPROPERTY() TObjectPtr<UAbilityTask_WaitDelay> ActiveWinClose = nullptr;
	UPROPERTY() TObjectPtr<UAbilityTask_WaitDelay> ActiveRecover = nullptr;
};

// ------------------------------------------------------------------ 技能攻击
UCLASS()
class DEMO_API UGA_PlayerAttackSkill : public UDemoGameplayAbility
{
	GENERATED_BODY()

public:
	UGA_PlayerAttackSkill();

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Skill")
	TObjectPtr<UAnimMontage> SkillMontage;

	// 数值来源 Core/DemoCombatTuning.h 的 PlayerSkillDamage / PlayerSkillPoiseDamage。
	// 这里写字面量而不是引用常量：UHT 对 UPROPERTY 默认值表达式的解析有限，
	// 引用跨文件变量容易在生成的代码里翻车。改平衡表时记得同步这两个数字。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Skill")
	float Damage = 120.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Skill")
	float PoiseDamage = 50.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Skill")
	float StaminaCost = 25.f;

protected:
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;

private:
	UFUNCTION() void OnMontageCompleted();
	UFUNCTION() void OnMontageCancelled();
};

// ------------------------------------------------------------------ 格挡（按住）
UCLASS()
class DEMO_API UGA_PlayerDefense : public UDemoGameplayAbility
{
	GENERATED_BODY()

public:
	UGA_PlayerDefense();

protected:
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;

	virtual void EndAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		bool bReplicateEndAbility, bool bWasCancelled) override;

private:
	FActiveGameplayEffectHandle BlockingEffectHandle;
};

// ------------------------------------------------------------------ 弹刀（鼠标右键，瞬时窗口）
//
// 流程（三种动作，全部走真实动画）：
//   按下右键 → 播放【起手】抬剑动作 + 开启判定窗（State.Parrying）
//     ├ 窗内被 Boss 的攻击命中 → ApplyHit 判定弹反成功 → 发 Event.Parry.Success
//     │   → 播放【弹刀成功】反击动作（同时立刻关掉判定窗，避免白嫖第二次）
//     └ 窗内没被打到 → 窗口到点 → 播放【收招】动作后结束
//   【持剑架势】可选：起手播完、窗口还没结束时接上，让招架姿态不空档。
UCLASS()
class DEMO_API UGA_PlayerParry : public UDemoGameplayAbility
{
	GENERATED_BODY()

public:
	UGA_PlayerParry();

	// 弹刀判定窗时长（秒）。
	// 这个值决定"技能何时收招/结束"，并必须与真正让 ApplyHit 认账的 UGE_ParryWindowEffect
	// 挂在身上的 State.Parrying 时长保持一致（两者都来自 Core/DemoCombatTuning.h 的
	// ParryWindowDuration，当前 0.55s）。CloseWindowEffects 会在技能结束时主动摘掉
	// State.Parrying，所以这里若比 GE 短，窗口会被提前截断。改动时两边要一起改。
	//
	// 窗内又分两档：开场 PerfectParryWindow（0.15s）内弹到 = 完美弹反，
	// 之后弹到 = 普通弹反。判定由 UGE_PerfectParryWindowEffect 提供的
	// State.PerfectParryWindow 完成，这里不需要额外参数。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Parry")
	float WindowDuration = 0.55f;

	// 弹刀成功后的反击动作（弹到 Boss 攻击时播放）。留空则自动按路径加载 Parry_Montage。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Parry")
	TObjectPtr<UAnimMontage> ParryMontage;

	// 招架起手：按下右键立刻播放的抬剑动作（留空则加载 Parry_Enter_Montage）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Parry")
	TObjectPtr<UAnimMontage> ParryEnterMontage;

	// 持剑架势：起手播完、判定窗还没结束时的过渡姿势（可留空 = 不播）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Parry")
	TObjectPtr<UAnimMontage> ParryHoldMontage;

	// 收招：判定窗结束但没弹到任何东西时播放（留空则加载 Parry_Exit_Montage）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Parry")
	TObjectPtr<UAnimMontage> ParryExitMontage;

	// 弹刀期间锁住 WASD（避免招架动作和移动叠加成“滑步”）。false = 可以边移边弹。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Parry")
	bool bLockMoveDuringParry = true;

	// 弹到之后要不要播那段"反击动作"（ParryMontage）。
	//
	// 默认 false = 只狼式：弹到就是弹到，不播额外动作，玩家可以【立刻】按下一刀
	// 去弹连招的第二段。BOSS 的三段连击能连着弹下来，靠的就是这一点。
	// 勾上 true = 表演性更强（每次弹开都有一段反手），但会明显拖慢连弹节奏。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Parry")
	bool bPlayCounterOnParry = false;

protected:
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;

	virtual void EndAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		bool bReplicateEndAbility, bool bWasCancelled) override;

private:
	// 当前动作阶段：决定一个蒙太奇播完后是接下一段还是收尾
	enum class EParryPhase : uint8 { Enter, Hold, Success, Exit };

	UFUNCTION() void OnWindowFinished();
	UFUNCTION() void OnParrySuccessEvent(FGameplayEventData Payload);
	UFUNCTION() void OnParryMontageFinished();
	UFUNCTION() void OnParryMontageCancelled();

	// ---------------- 连弹（只狼式弹刀节奏的骨架）----------------
	//
	// 问题：技能是 InstancedPerActor，已经激活时 TryActivateAbility 直接失败。
	//       于是弹刀成功后的"收势"期间按第二下右键石沉大海 ——
	//       BOSS 的三段连击只能弹掉第一段。
	// 解法：控制器在激活失败时补发 Event_Ability_ParryRewindow，本技能收到就
	//       把判定窗重新推开一次（不重启动作、不动移动锁）。
	UFUNCTION() void OnRewindowEvent(FGameplayEventData Payload);

	// 收势计时结束（到点就收技能；期间玩家又开了新窗则由新窗接管）
	UFUNCTION() void OnSuccessLockFinished();

	// ---------------- 挂死兜底（看门狗）----------------
	//
	// 为什么需要它：本技能有【多条】收尾路径 —— 窗口计时、收势计时、蒙太奇完成/被打断、
	// 连弹重开…… 只要任何一条因为时序问题"让路"给了别人，EndAbility 就永远不会被调用，
	// 后果是移动锁不解 + 攻击被 BlockAbilities 挡住 = **玩家彻底失去操作**。
	// 这种 bug 依赖特定时序、很难在代码里穷举，所以用一条独立的世界定时器兜底：
	// 技能激活超过 ParrySafetyTimeout 秒还没结束 → 强制结束。
	// 它不依赖 GAS 的任务系统（任务系统本身可能就是故障的一环），因此更可靠。
	UFUNCTION() void OnParrySafetyTimeout();
	FTimerHandle ParrySafetyTimerHandle;

	// 兜底时限：要明显长于"一次正常弹反"的总时长（起手 + 判定窗 + 收势 + 反击动作），
	// 否则正常流程也会被它掐断；但也不能太长，否则玩家要干等。
	// ⚠ 不能加 BlueprintReadOnly —— UHT 禁止 Blueprint* 说明符用在 private 成员上。
	UPROPERTY(EditDefaultsOnly, Category = "Demo|Parry")
	float ParrySafetyTimeout = 4.f;

	// 开 / 重开判定窗：挂两个窗口 GE + 重新挂成功监听 + 重启窗口计时。
	// 起手和连弹共用同一条路径，保证两边的行为不会漂移。
	void OpenParryWindow();

	// 把两个窗口 GE 摘掉（成功弹到、或重开窗之前都要先收掉上一轮）
	void CloseWindowEffects();

	// 播放某个阶段的动作（会先停掉上一个动作，避免旧任务的回调串到新阶段）
	void PlayParryMontage(UAnimMontage* Montage, EParryPhase InPhase);

	// 只解绑回调（技能自己在结束时用：蒙太奇由 bStopWhenAbilityEnds 负责停）
	void UnbindParryMontage();
	// 解绑 + 停掉当前动作（切换到另一个动作时用）
	void StopParryMontage();

	// 优先用蓝图配的动作，没配就按路径兜底加载
	static UAnimMontage* ResolveMontage(const TObjectPtr<UAnimMontage>& Configured, const TCHAR* FallbackPath);

	EParryPhase Phase = EParryPhase::Enter;
	bool bWindowOpen = false;
	bool bParrySucceeded = false;
	bool bLockedMoveByParry = false;   // 移动锁是本技能加的吗（只释放自己加的那次）

	// "这次弹反真的起手了"的标记 —— 只有它为 true 时，EndAbility 才结算【空弹】体力惩罚。
	// 不能直接用 !bParrySucceeded 判断：体力耗尽时会【拒绝】起手并立刻 EndAbility，
	// 那条路径上 bParrySucceeded 也是 false，照搬判断会给"根本没举起刃"也扣一笔体力。
	bool bEmptyParryCostArmed = false;

	FActiveGameplayEffectHandle ParryWindowHandle;
	FActiveGameplayEffectHandle PerfectParryWindowHandle;

	UPROPERTY() TObjectPtr<UAbilityTask_PlayMontageAndWait> ActiveParryMontage = nullptr;
	UPROPERTY() TObjectPtr<UAbilityTask_WaitGameplayEvent> ParrySuccessListener = nullptr;
	// 连弹监听：OnlyTriggerOnce=false，每次按右键都能重开窗
	UPROPERTY() TObjectPtr<UAbilityTask_WaitGameplayEvent> ParryRewindowListener = nullptr;
	// 判定窗计时（起手 / 连弹都会重建，所以要留引用以便先停掉旧的）
	UPROPERTY() TObjectPtr<UAbilityTask_WaitDelay> ActiveWindowTask = nullptr;
	// 收势计时（到点收技能）
	UPROPERTY() TObjectPtr<UAbilityTask_WaitDelay> SuccessLockTask = nullptr;
};

// ------------------------------------------------------------------ 闪避
UCLASS()
class DEMO_API UGA_PlayerDodge : public UDemoGameplayAbility
{
	GENERATED_BODY()

public:
	UGA_PlayerDodge();

	// 未配置方向蒙太奇时使用的默认闪避
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Dodge")
	TObjectPtr<UAnimMontage> DodgeMontage;

	// 方向蒙太奇。约定顺序：0=前(+X) / 1=后(-X) / 2=左(-Y) / 3=右(+Y)，留空则统一用 DodgeMontage。
	//
	// ⚠ 实际上【以资产名称为准】：代码会从每个蒙太奇的名字里认方向
	//   （Dodge_F / Dodge_B / Dodge_L / Dodge_R，或 ..._Forward/_Back/_Left/_Right），
	//   4 段都能认出来就按名字选，认不出才退回上面的下标顺序。
	//   这是为了防“数组被脚本/手滑写反 → 按 W 却向右闪”这类事故（历史上真发生过）。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Dodge")
	TArray<TObjectPtr<UAnimMontage>> DirectionalMontages;

	// 闪避位移距离（厘米）—— 控制“闪多远”的主参数。150 = 1.5 米。
	// 位移由 ApplyRootMotionConstantForce 以“匀速”在 DodgeDuration 内走完，
	// 所以这个数字就是实际位移量（不会像给初速度那样受摩擦力影响、忽长忽短）。
	// 同时闪避期间动画自带的根位移会被屏蔽（两层：AnimInstance 模式 + 蒙太奇实例级开关），
	// 两者不会叠加 —— 日志里的“实际位移”应当与这里的“目标距离”基本相等。
	//
	// ⚠ 2026-10-02 由 120 拉长到 150（用户要求"闪避距离拉长一点点"）。
	// ⚠⚠ 改这个值【必须同时改下面的 DodgeDistanceMax】，而且很可能还要动蓝图：
	//   蓝图子类（GA_Dodge）的 Class Defaults 里序列化着旧值，**蓝图 CDO 优先于 C++ 默认值**。
	//   只改 C++ 而没改蓝图 → 画面完全不变（日志会打"DodgeDistance=120 被上限夹到 150"）。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Dodge")
	float DodgeDistance = 150.f;

	// 闪避位移时长（秒）—— 位移在这段时间内匀速走完，之后收招动作原地播放。
	// 速度 = DodgeDistance / DodgeDuration。越短越“爆”，越长越“滑”。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Dodge")
	float DodgeDuration = 0.32f;

	// 闪避位移距离上限（厘米）—— 安全阀。
	// 为什么需要它：蓝图子类（GA_Dodge）的 Class Defaults 里可能**序列化着早期版本存的旧距离**
	// （蓝图 CDO 的值优先于 C++ 默认值），于是出现"改了 C++ 默认值但画面没变"。
	// 实际生效的距离 = Clamp(DodgeDistance, 0, DodgeDistanceMax)，保证它一定收得住；
	// 真想让角色闪得更远，把这个上限一起调大即可。
	// ⚠ 2026-10-02 由 150 提到 200：原来 DodgeDistance=150 正好等于上限，
	//   一旦有人把 DodgeDistance 调大就会被"夹"回 150（曾经发生过），导致改不动。
	//   上限必须留出余量。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Dodge")
	float DodgeDistanceMax = 200.f;

	// 【兜底，仅当 DodgeDistance <= 0 时生效】直接给初速度（cm/s）。
	// 注意它的实际距离受地面摩擦力影响，不好预估 —— 建议优先用 DodgeDistance。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Dodge")
	float DodgeImpulse = 350.f;

	// 是否给闪避位移（默认开）。关掉 = 只播原地闪避动画、不产生位移。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Dodge")
	bool bApplyLaunchImpulse = true;

	// 闪避期间锁住 WASD（默认开）：防止玩家方向键输入和闪避位移互相打架、把方向带偏。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Dodge")
	bool bLockMoveDuringDodge = true;

	// ---------------- 无敌帧（默认开，强烈建议别关）----------------
	//
	// ⚠ 这条是"闪了却照样挨打"的根治点。
	//   以前闪避完全不发无敌帧，State.Invincible 只能靠闪避蒙太奇上的动画通知给，
	//   而闪避蒙太奇是 Tools/SetupDodgeMontages.py 批量生成的、里面根本没有通知 ——
	//   结果就是闪避只有位移、没有任何免伤，手感上等于"没有闪避"。
	//   现在由技能显式 ApplyEffectToSelf 挂上，不再依赖动画资产。
	//
	// 时长由 UGE_InvincibleEffect 决定（DemoCombatTuning.h 的 DodgeInvincibleDuration）。
	// 和它一起挂的还有一个"完美闪避判定窗"（PerfectDodgeWindow）：
	//   开场 0.22s 内被打到 → 完美闪避（子弹时间 + 回耐力）
	//   之后到无敌结束被打到 → 普通闪避（只免伤）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Dodge")
	bool bGrantInvincible = true;

protected:
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;

	// 闪避结束：恢复被临时屏蔽的动画根位移，并按需解锁 WASD
	virtual void EndAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		bool bReplicateEndAbility, bool bWasCancelled) override;

private:
	UFUNCTION() void OnMontageCompleted();
	UFUNCTION() void OnMontageCancelled();
	UAnimMontage* PickDodgeMontage() const;

	// 本次闪避的“镜头坐标系方向”（X=前（远离镜头），Y=右）；零向量 = 没按方向键 → 向前闪。
	// 用镜头 yaw 而不是角色 yaw 反算，保证“按 W 就朝屏幕前方闪”，与 WASD 移动完全一致。
	FVector PendingLocalDir = FVector::ZeroVector;

	// 上一次闪避结束的世界时间。
	// 技能是 InstancedPerActor（基类 DemoGameplayAbility 里设的），实例跨次激活保留，
	// 所以这个时间戳能被下一次闪避读到 —— 不需要额外挂到角色身上。
	// 用途：连点闪避惩罚（距上次太近就扑短），压掉"疯狂点 Shift"的保底打法。
	float LastDodgeEndRealTime = -1000.f;

	// 本次闪避实际走的世界方向（位移用），日志/核对方向是否跟 WASD 一致
	FVector PendingWorldDir = FVector::ZeroVector;

	// 本次闪避【实际生效】的位移距离（已按 DodgeDistanceMax 夹过上限）。
	// 起手日志的“目标距离”与结束日志的“实际位移”都用它，两个数才能对得上。
	float PendingDistance = 0.f;

	// 是否由本技能锁住了 WASD（结束时按此还原，避免误解锁其他技能加的锁）
	bool bLockedMoveByDodge = false;

	// 定距位移任务：以恒定速度把角色推进 DodgeDistance 厘米
	UPROPERTY() TObjectPtr<UAbilityTask_ApplyRootMotionConstantForce> DodgeForceTask = nullptr;

	// 本技能挂上去的 GE 句柄，结束时主动移除。
	//
	// 为什么不用"设个短时长让它自己过期"：
	//   闪避动作播完（或被取消）时无敌就该立刻结束。
	//   靠时长硬等会出现"闪避被格挡打断、人已经站定了还挂着无敌"的破绽。
	FActiveGameplayEffectHandle DodgeInvincibleHandle;
	FActiveGameplayEffectHandle PerfectDodgeWindowHandle;
};

// ------------------------------------------------------------------ 道具：使用"酒"回血
//
// 流程（参考 UGA_PlayerAttackSkill 的"播放蒙太奇 + 结算 GE"范式）：
//   按 R（或蓝图输入 IA_UseItem）→ 先检查玩家身上还有没有酒（AFightPlayerCharacter::TryConsumeHealItem）
//     ├ 没酒 → 直接 EndAbility（HUD 提示"酒已喝光"）
//     └ 有酒 → 消耗 1 瓶 + 播放喝酒蒙太奇 + 结算 UGE_HealEffect（SetByCaller=最大生命×30%）
//              + 触发 HUD 绿色反馈（整屏绿光 + "+600"飘字）
//
// ⚠ 喝血期间锁住进攻/闪避/格挡，避免"边喝边砍"破坏动作衔接（与出招共用 State.Attacking 忙碌态）。
//   回血数值走 GE（不是 SetHealth），否则不会触发 OnHealthChanged + clamp，血条不会动。
UCLASS()
class DEMO_API UGA_PlayerUseHeal : public UDemoGameplayAbility
{
	GENERATED_BODY()

public:
	UGA_PlayerUseHeal();

	// 喝酒动作蒙太奇（留空则按 Core/DemoCombatTuning.h 的 HealMontagePath 兜底加载）。
	// 占位源：SwordAnimsetPro 的 Buff_In 等"抬手到嘴边"序列（见 Tools/SetupHealDrinkMontage.py）。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Heal")
	TObjectPtr<UAnimMontage> HealMontage;

	// 喝酒动作时长（秒）。用来给"结算回血"一个合适的节拍：
	// 默认在动画播到 55% 时真正灌下（表现上"酒到嘴边才回血"），0 则一开始就回。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Heal")
	float HealApplyRatio = 0.55f;

protected:
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;

	// 收尾时必须解开本技能锁住的 WASD：否则喝完酒玩家会"卡住动不了"，
	// 直到下一次攻击/闪避才解锁（隐蔽 bug）。只在"确实是本技能锁的"前提下解锁，
	// 避免误解放其它技能（普攻/闪避）加的锁。
	virtual void EndAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		bool bReplicateEndAbility, bool bWasCancelled) override;

private:
	// 真正结算回血（在喝酒动作的 HealApplyRatio 进度点调用一次）
	UFUNCTION() void OnHealApplyPoint();
	UFUNCTION() void OnMontageCompleted();
	UFUNCTION() void OnMontageCancelled();

	// 施加 UGE_HealEffect（SetByCaller = 最大生命 × HealPercent）。
	// InMaxHealth<=0 时从 ASC 取当前最大生命。bHealApplied 保证只结算一次。
	void ApplyHeal(AFightPlayerCharacter* Player, UAbilitySystemComponent* ASC, float InMaxHealth);

	// 兜底：无论蒙太奇怎么结束（被打断/播完），都保证只结算一次回血
	bool bHealApplied = false;
	bool bMoveLockedByHeal = false;   // 本技能是否锁住了 WASD（结束时只释放自己加的那次）

	UPROPERTY() TObjectPtr<UAbilityTask_PlayMontageAndWait> ActiveMontageTask = nullptr;
	UPROPERTY() TObjectPtr<UAbilityTask_WaitDelay> HealApplyTimer = nullptr;
};
