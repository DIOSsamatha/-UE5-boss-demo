// 玩家与 BOSS 的共享基类：ASC、武器扫掠命中检测、受击/破防/死亡处理
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "AbilitySystemInterface.h"
#include "DemoCommon.h"
#include "DemoCharacterBase.generated.h"

class UAbilitySystemComponent;
class UDemoAttributeSet;
class UGameplayEffect;
class UDemoGameplayAbility;
class UWidgetComponent;
class UDemoNameplateWidget;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FDemoOnHitLanded, AActor*, HitActor, float, Damage, EDemoHitResultType, ResultType, FVector, HitLocation);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FDemoOnDamaged, float, Damage, EDemoHitDirection, Direction, EDemoHitResultType, ResultType);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FDemoOnCharacterPoiseBroken);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FDemoOnCharacterDeath);

// 一次受击的完整信息：真实伤害（已结算）+ 判定结果 + 伤害来源 + 受击方向。
// UI 的伤害飘字、命中反馈文字、受击音效全都监听它 —— 一处发、多处收，不会各算各的。
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FDemoOnDamagedDetailed,
	float, Damage,
	EDemoHitResultType, ResultType,
	EDemoDamageSource, DamageSource,
	FVector, HitDirection);

UCLASS()
class DEMO_API ADemoCharacterBase : public ACharacter, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:
	ADemoCharacterBase();

	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|GAS")
	UDemoAttributeSet* GetDemoAttributeSet() const { return AttributeSet; }

	// ---------- 动画通知回调 ----------
	UFUNCTION(BlueprintCallable, Category = "Demo|Combat")
	void SetMeleeHitWindowEnabled(bool bEnabled);

	UFUNCTION(BlueprintCallable, Category = "Demo|Combat")
	void SetComboWindowEnabled(bool bEnabled);

	// 由技能在出招前设置本次攻击的参数
	UFUNCTION(BlueprintCallable, Category = "Demo|Combat")
	void SetCurrentAttackParams(float Damage, float PoiseDamage, bool bCanBeParried, bool bCanBeBlocked);

	// ---------- 阵营 ----------
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|Combat")
	bool IsHostileTo(const AActor* Other) const;

	// 弹刀成功的角色侧反馈（ApplyHit 在弹反分支里调用）。
	//
	// 火花 / 爆点这种"世界里的东西"由 ApplyHit 直接生成；
	// 而镜头冲击是【角色自己】的事，所以留一个虚函数给角色覆写 ——
	// 用虚函数而不是在 Core 里直接 Cast<AFightPlayerCharacter>，
	// 是为了不让 Core 反向依赖 Player（谁被打都可以有各自的反馈）。
	// 基类空实现；玩家角色覆写它做镜头冲击。
	virtual void PlayParryFeedback(const FVector& ContactLocation, bool bPerfect) {}

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|Combat")
	EDemoTeam GetTeam() const { return static_cast<EDemoTeam>(TeamId); }

	UFUNCTION(BlueprintCallable, Category = "Demo|Combat")
	void SetTeam(EDemoTeam NewTeam) { TeamId = static_cast<uint8>(NewTeam); }

	// ---------- 头顶血条 ----------
	UFUNCTION(BlueprintCallable, Category = "Demo|UI")
	void SetHealthNameplateVisible(bool bVisible);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|UI")
	UDemoNameplateWidget* GetHealthNameplate() const;

	// 显示名（头顶血条 / 击杀提示用）。留空则退回 Actor 名
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|UI")
	FText DisplayName;

	// ---------- 属性便捷访问 ----------
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|Vital")
	float GetHealth() const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|Vital")
	float GetMaxHealth() const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|Vital")
	float GetStamina() const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|Vital")
	float GetMaxStamina() const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|Vital")
	float GetPoise() const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|Vital")
	float GetMaxPoise() const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|Vital")
	bool IsDead() const;

	// 最近一次移动输入（闪避用它选方向）。
	// ⚠ 注意：松手后这个值会“残留”，所以判断“现在到底有没有按方向键”请用 HasMoveInput()，
	//   否则会出现“明明没按方向，闪避却朝上次按过的方向闪”。
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|Movement")
	const FVector& GetLastMoveInput() const { return LastMoveInput; }

	UFUNCTION(BlueprintCallable, Category = "Demo|Movement")
	void SetLastMoveInput(const FVector& InMoveInput);

	// 松开方向键时立刻清空（闪避就不会复用上一次的旧方向了）
	UFUNCTION(BlueprintCallable, Category = "Demo|Movement")
	void ClearLastMoveInput();

	// 手上是否“正按着”方向键：要求 MaxAge 秒内刚更新过、且方向不为零。
	// 这是“没按方向键就默认向前闪”的判据。
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|Movement")
	bool HasMoveInput(float MaxAge = 0.25f) const;

	// 取“有效”的移动方向；没有有效输入时返回零向量（由调用方决定默认方向）
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|Movement")
	FVector GetMoveInputOrZero(float MaxAge = 0.25f) const;

	// ---------- 广播 ----------
	UPROPERTY(BlueprintAssignable, Category = "Demo|Events")
	FDemoOnHitLanded OnHitLanded;

	UPROPERTY(BlueprintAssignable, Category = "Demo|Events")
	FDemoOnDamaged OnDamaged;

	UPROPERTY(BlueprintAssignable, Category = "Demo|Events")
	FDemoOnDamagedDetailed OnDamagedDetailed;

	UPROPERTY(BlueprintAssignable, Category = "Demo|Events")
	FDemoOnCharacterPoiseBroken OnPoiseBroken;

	// 失衡（异常条打满）：UI 标签 + BOSS 材质变色两条通道都从这里出发
	UPROPERTY(BlueprintAssignable, Category = "Demo|Events")
	FDemoOnCharacterPoiseBroken OnAnomalyUnstable;

	// 击破（失衡期间元素槽再满）：失去格挡 + 虚弱窗口
	UPROPERTY(BlueprintAssignable, Category = "Demo|Events")
	FDemoOnCharacterPoiseBroken OnAnomalyBroken;

	// 这个角色是否有「双异常条」（冲击 / 元素两条积累槽）。当前只有 BOSS 打开。
	// 伤害库在积累异常前会先查它 —— 否则 BOSS 每打玩家一下都在给玩家攒"失衡"，
	// 反馈链的方向就反了（玩家的对应资源是架势条，职责不同）。
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|Combat")
	bool HasAnomalyBars() const { return bHasAnomalyBars; }

	// 我成功格挡了一次攻击（由 ApplyHit 的格挡分支调用）。
	//
	// 为什么需要一个事件而不是在伤害库里直接写 BOSS 逻辑：
	//   伤害库（UDemoAbilitySystemLibrary）不该依赖 Boss 模块，否则以后给小怪/玩家加
	//   "被压着打就还手"的行为时，又要在结算函数里塞一层 Cast。
	//   这里只发事件，谁关心谁重写 —— BOSS 用它累计被格挡次数并反制。
	UFUNCTION(BlueprintNativeEvent, Category = "Demo|Combat")
	void OnBlockedIncomingHit(AActor* Attacker);

	// ★ 受击【伤害结算之前】的自愿格挡判定（2026-10-03）。
	//
	// 为什么必须在结算之前问：
	//   玩家的格挡是"按住键 → 身上挂着 State.Blocking 标签"，ApplyHit 一查就知道；
	//   而 BOSS 的格挡是【概率触发】的，没有可查的持续状态 —— 如果等到
	//   OnDamagedReaction（伤害已经扣完的回调）里再掷骰子，就只能补播一个动画，
	//   血照掉、硬直照吃，玩家读到的是"它在挨打"而不是"它挡住了"。
	//
	// 返回 true → ApplyHit 按"被格挡"结算（伤害 ×0.25、扣攻方架势、发 OnBlockedIncomingHit）。
	// 默认 false：玩家 / 小怪不走这条路。
	virtual bool TryGuardIncomingHit(AActor* Attacker, float IncomingDamage, bool bCanBeBlocked) { return false; }

	// ---------- 拼刀（双方同时出手）----------
	// 记录"我这把刀正在挥"的时刻。拼刀判定必须基于【真的在挥刀】，
	// 而不是"处于出招状态" —— 出招有前摇/后摇，只看状态会把"我刚起手、对方已经收招"
	// 也判成拼刀，玩家会觉得判定很水。
	void MarkSwing();

	// 距离最近一次挥刀是否还在 Window 秒内
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|Combat")
	bool HasRecentSwing(float Window) const;

	// 冷却：拼刀不能每一刀都触发（否则连段会变成无限稳心）
	bool HasRecentClash(float Window) const;
	void MarkClash();

	// 架势条读数（HUD 直接读这里）
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|Combat")
	float GetPosture() const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|Combat")
	float GetMaxPosture() const;

	// 双异常条读数（HUD 的两条槽直接读这里）。
	// ⚠ 定义在 .cpp：这里只有 UDemoAttributeSet 的前向声明，
	//    在头文件里写 `AttributeSet->GetXxx()` 会报 C2027（使用了未定义类型）。
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|Combat")
	float GetAnomalyImpact() const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|Combat")
	float GetMaxAnomalyImpact() const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|Combat")
	float GetAnomalyElement() const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|Combat")
	float GetMaxAnomalyElement() const;

	// 清空双异常条（失衡到期 / 被击破时调用）。
	// 用 ASC->SetNumericAttributeBase 而不是再发一个 GE：这是"重置"不是"结算"，
	// 走 GE 会再触发一次 PostGameplayEffectExecute，于是"清零的瞬间又被判定成打满"，
	// 状态会无限自激。改 base 值只走 PreAttributeChange，安全。
	UFUNCTION(BlueprintCallable, Category = "Demo|Combat")
	void ClearAnomalyBars();

	UPROPERTY(BlueprintAssignable, Category = "Demo|Events")
	FDemoOnCharacterDeath OnCharacterDeath;

	// ---------- 最近一次受击（UI 不想订阅委托时可以直接读）----------
	UPROPERTY(BlueprintReadOnly, Category = "Demo|Combat")
	float LastDamageTaken = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Demo|Combat")
	EDemoHitResultType LastHitResultType = EDemoHitResultType::Normal;

	UPROPERTY(BlueprintReadOnly, Category = "Demo|Combat")
	EDemoDamageSource LastDamageSource = EDemoDamageSource::Unknown;

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

	// 子类覆写以播放各自的表现（受击动画、死亡动画等）
	virtual void OnDamagedReaction(EDemoHitDirection Direction, float Damage);
	virtual void OnDeathReaction();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Demo|GAS")
	TObjectPtr<UAbilitySystemComponent> AbilitySystemComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Demo|GAS")
	TObjectPtr<UDemoAttributeSet> AttributeSet;

	// 头顶浮动血条（屏幕空间 WidgetComponent）。默认隐藏，由 bShowHealthNameplate 打开
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Demo|UI")
	TObjectPtr<UWidgetComponent> HealthNameplateComponent;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|UI")
	bool bShowHealthNameplate = false;

	// 这个角色是否有「双异常条」（冲击 / 元素两条积累槽）。当前只有 BOSS 打开。
	// 为什么需要一个开关而不是对所有人一视同仁：
	//   玩家侧对应的资源是【架势条】（格挡消耗、过低破防），职责完全不同；
	//   如果两边共用异常条，玩家被 BOSS 打几下就会进"失衡"，反馈链的意思就反了。
	// ApplyAnomalyGain 会先查这个开关，所以普通小怪/玩家不会被灌入 BOSS 的异常资源。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Combat")
	bool bHasAnomalyBars = false;

	// 血条相对角色原点的高度偏移（cm）。BOSS 体型大就往上调
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|UI")
	FVector NameplateOffset = FVector(0.f, 0.f, 130.f);

	// 初始授予的技能列表（在蓝图里配）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|GAS")
	TArray<TSubclassOf<UDemoGameplayAbility>> DefaultAbilities;

	// 武器扫掠用的两个骨骼插槽
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Combat")
	FName WeaponStartSocket = TEXT("WeaponStart");

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Combat")
	FName WeaponEndSocket = TEXT("WeaponEnd");

	// 近战判定球半径（cm）：每个采样点按这个半径做球形重叠，扫掠上一帧→这一帧的刀锋轨迹。
	// ⚠ 2026-10-02 由 30 提到 55：BOSS 的刀锋判定用的是"骨骼推导的虚拟刀刃"（插槽缺失），
	//   判定线段不可能和真实刀刃完全重合，靠半径留出容错；
	//   30 太窄 → 贴身距离下常常判不到 → 玩家看到"BOSS 砍中了却没伤害"。
	//   55 ≈ 略大于玩家胶囊半径(42)，贴身互砍能稳定命中，不会误伤到背后的人。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Combat")
	float WeaponTraceRadius = 55.f;

	// 武器插槽只在第一次挥砍时打印一行解析结果（排查"判定位置和刀锋对不上"用的），
	// 不是配置项，所以不进 UPROPERTY
	bool bLoggedWeaponSockets = false;

	// 破防持续时间，结束后自动回韧
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Combat")
	float BreakDuration = 2.5f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Combat")
	uint8 TeamId = 0;

	// ============================ 平衡数值（真正生效的地方）============================
	//
	// ⚠ 为什么叫 Balance* 而不是 Default*：蓝图会把属性按【名字】序列化，
	//   如果沿用旧名，BP_Boss / BP_FightPlayer 里存着的上一版数值会在加载时
	//   盖掉这里的新默认值 —— 那就是"改了 C++ 但游戏里没变"的经典事故。
	//   换个新名字，旧值自然失效，这里的值一定会生效。
	//
	// 全部数值的来源与推导见 Core/DemoCombatTuning.h（目标：一场 BOSS 战 4~6 分钟）。
	// 基类默认 = 玩家数值；BOSS 在自己的构造函数里覆盖成 BOSS 数值。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Balance")
	float BalanceMaxHealth = 2000.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Balance")
	float BalanceMaxStamina = 200.f;

	// 韧性上限。
	// ⚠⚠ 2026-10-04：从 BalanceMaxPoise 【改名】为 BalancePoiseCap。
	//   两个原因叠加，导致"破防阈值调高"一直没生效：
	//     ① BP_Boss 的 CDO 里序列化过 BalanceMaxPoise=320 → 盖掉 C++ 的新默认值；
	//     ② ABossCharacterBase 构造函数里又【硬编码】BalanceMaxPoise = 320.f，
	//        而 Core/DemoCombatTuning.h 的 DemoBalance::BossMaxPoise
	//        （2026-09-30 已经上调到 520，2026-10-04 再到 720）压根没有任何代码去读。
	//   改名让 CDO 里的旧记录作废，同时构造函数改读平衡表 → tuning 成为唯一真源。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Balance")
	float BalancePoiseCap = 260.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Balance")
	float BalanceAttackPower = 0.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Balance")
	float BalanceDefensePower = 25.f;

	// 0 = 不改动 AttributeSet 里的移速
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Balance")
	float BalanceMoveSpeed = 0.f;

	// 把 Balance* 写进 AttributeSet。在 BeginPlay 最开始调用。
	//
	// 历史坑：以前这里的守卫是 `if (GetMaxHealth() > 0) return;`，而 UDemoAttributeSet
	// 的构造函数已经 InitMaxHealth(100) —— 守卫永远命中，兜底从未执行过。
	// 于是 BOSS 血量实际是 100（不是配置里的 1600），双方几下就能打死对方、
	// 整场战斗只有十几秒。现在改用"构造哨兵值"判定：只要还是 AttributeSet 的
	// 构造默认值，就说明没有任何蓝图/数据资产配过，兜底就该接管。
	virtual void EnsureDefaultAttributes();

private:
	void PerformMeleeTrace();
	void HandlePoiseBroken();
	void HandleDeath();
	void RecoverFromBreak();

	// ---------- 双异常条（失衡 / 击破）----------
	void HandleAnomalyUnstable();
	void RecoverFromUnstable();
	void HandleAnomalyBroken();

	// ---------- 架势条（被破防）----------
	void HandlePostureBroken();
	void RecoverFromPostureBreak();
	void OnMoveSpeedChanged(const struct FOnAttributeChangeData& Data);
	void OnDamagedEventInternal(const struct FGameplayEventData* Payload);

	// AttributeSet 结算完真实伤害后回调（绑定 OnDamageTaken）
	void HandleDamageTakenInternal(float AppliedDamage, EDemoHitResultType ResultType,
		EDemoDamageSource DamageSource, FVector HitDirection);

	// 血量变化 → 刷新头顶血条
	void HandleHealthChangedForNameplate(float NewValue, float MaxValue);

	bool bMeleeHitWindowOpen = false;
	bool bComboWindowOpen = false;
	bool bHasPrevSocketLocations = false;

	FVector LastMoveInput = FVector::ZeroVector;

	// 最后一次收到移动输入的时刻（世界时间）。松手后旧方向会残留，
	// 靠这个时间戳判断“输入是否还新鲜”，实现“没按方向键就向前闪”。
	float LastMoveInputTime = -1000.f;

	FVector PrevStartLocation = FVector::ZeroVector;
	FVector PrevEndLocation = FVector::ZeroVector;

	// 武器插槽缺失时（BOSS 常态）额外铺开的判定采样点：
	// 沿【手臂骨链 hand→forearm→upperarm→clavicle】+【刀身中段等距取样】。
	// 只靠"手骨 + 外推端点"两个点，挥砍途中会甩掉贴身的玩家 ——
	// 手骨在举刀时离躯干很远、劈下时很近，单点判定必然漏。
	// 有了这些点，判定球始终覆盖整条挥砍轨迹。
	TArray<FVector> ExtraTracePoints;

	TArray<TWeakObjectPtr<AActor>> HitActorsThisSwing;

	float CurrentAttackDamage = 0.f;
	float CurrentPoiseDamage = 0.f;
	bool bCurrentAttackCanBeParried = false;
	bool bCurrentAttackCanBeBlocked = true;

	FTimerHandle BreakRecoverTimerHandle;
	FTimerHandle UnstableRecoverTimerHandle;
	FTimerHandle PostureRecoverTimerHandle;

	// 最近一次"正在挥刀"的世界时间（负数 = 从未挥过）
	float LastSwingTime = -1.e9f;
	// 最近一次拼刀的世界时间（用于内置冷却）
	float LastClashTime = -1.e9f;
};
