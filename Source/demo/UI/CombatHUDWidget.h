// 战斗 HUD
//
// 这一版的核心变化：**控件树在 C++ 里搭**，所以你不需要在 UMG 里拖任何东西。
//   · WBP_CombatHUD（继承 UCombatHUDWidget）→ 立刻有玩家血/耐力/韧性条 + 伤害飘字 + 命中反馈
//   · WBP_BossBar（继承 UBossHealthBarWidget）→ 立刻有 BOSS 大血条 + 韧性条 + 阶段刻度
//
// 为什么这么设计：
//   这两个 WBP 之前是空壳（WidgetTree 里一个控件都没有），而"三条状态 + 飘字 + 反馈字"的布局
//   在任何项目里都长得差不多，手工拖一遍纯属浪费。C++ 搭好、想美化再在子类里覆写即可。
//
// 数值靠每帧轮询（血/耐/韧读属性是纯内存读），事件靠委托（弹反、破防、死亡、阶段切换）。
//   这样即使某个委托漏发，血条也不会停在错误的数值上。
//
// 数据流：
//   ApplyHit（唯一伤害入口）
//     └→ 目标 AttributeSet::PostGameplayEffectExecute  → OnDamageTaken(真实伤害, 判定结果, 来源, 方向)
//          └→ ADemoCharacterBase::OnDamagedDetailed
//               ├→ 本 HUD：飘字 + 命中反馈大字
//               └→ UBossHealthBarWidget：BOSS 挨打时的飘字
#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Core/DemoCommon.h"
#include "CombatHUDWidget.generated.h"

class ADemoCharacterBase;
class ABossCharacterBase;
class UDemoAttributeSet;
class UDemoStatusBarWidget;
class UDemoDamageNumberLayer;
class UCanvasPanel;
class UTextBlock;
class UImage;
struct FGameplayEventData;

// ==================================================================================
// 玩家 HUD
// ==================================================================================
UCLASS()
class DEMO_API UCombatHUDWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	// 绑定到玩家角色，之后自动监听血量/耐力/韧性/受击/死亡
	UFUNCTION(BlueprintCallable, Category = "Demo|HUD")
	void BindToCharacter(ADemoCharacterBase* Character);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|HUD")
	ADemoCharacterBase* GetBoundCharacter() const { return BoundCharacter; }

	// 手动弹一条居中大字（"弹反!" / "格挡" / "完美闪避"）。Duration < 0 时用 FeedbackTextLifetime
	UFUNCTION(BlueprintCallable, Category = "Demo|HUD")
	void ShowFeedbackText(const FText& InText, FLinearColor Color, float Duration = -1.f);

	// 整屏闪一下（完美闪避 / 完美弹反的视觉冲击）。
	//
	// Color       闪光颜色（完美弹反=金黄，完美闪避=青白）
	// Duration    淡出总时长
	// PeakOpacity 起始不透明度（0~1）。别给太高，不然会短暂盖住 UI
	UFUNCTION(BlueprintCallable, Category = "Demo|HUD")
	void FlashScreen(FLinearColor Color, float Duration = 0.35f, float PeakOpacity = 0.35f);

	// 每帧驱动（由 AFightGameMode::Tick 调用）。
	//
	// 为什么不用 NativeTick：
	//   UMG 的 Tick 是否启用取决于 WidgetBlueprintGeneratedClass 上的
	//   bHasScriptImplementedTick / bClassRequiresNativeTick —— 这两个标志是"编译蓝图时"写死的，
	//   而我们这两个 WBP 是在加 C++ 逻辑之前建的，标志可能仍为 false，
	//   那样 NativeTick 就永远不会被调用（血条会一直停在初始值上）。
	//   交给 GameMode 驱动则 100% 确定会跑，跟蓝图怎么配都无关。
	UFUNCTION(BlueprintCallable, Category = "Demo|HUD")
	void TickHUD(float DeltaTime);

	// ---------------- 蓝图事件（WBP 里实现表现，可以不实现）----------------
	UFUNCTION(BlueprintImplementableEvent, Category = "Demo|HUD")
	void OnHealthChanged(float NewHealth, float MaxHealth);

	UFUNCTION(BlueprintImplementableEvent, Category = "Demo|HUD")
	void OnStaminaChanged(float NewStamina, float MaxStamina);

	UFUNCTION(BlueprintImplementableEvent, Category = "Demo|HUD")
	void OnPoiseChanged(float NewPoise, float MaxPoise);

	// 命中反馈：格挡、弹反、暴击各做各的表现
	UFUNCTION(BlueprintImplementableEvent, Category = "Demo|HUD")
	void OnHitFeedback(EDemoHitResultType ResultType, FVector WorldLocation);

	// 自己挨了一下的完整信息（想做自定义飘字就覆写它）
	UFUNCTION(BlueprintImplementableEvent, Category = "Demo|HUD")
	void OnPlayerDamagedDetailed(float AppliedDamage, EDemoHitResultType ResultType,
		EDemoDamageSource DamageSource, FVector HitDirection);

	UFUNCTION(BlueprintImplementableEvent, Category = "Demo|HUD")
	void OnParrySuccess();

	// 完美弹反（判定窗前 0.12s 内弹到）：比普通弹反更强的表现
	UFUNCTION(BlueprintImplementableEvent, Category = "Demo|HUD")
	void OnPerfectParry();

	// 拼刀稳心：双方刀相撞，取消硬直 + 回架势（"稳住了"的反馈，区别于弹反/闪避）
	UFUNCTION(BlueprintImplementableEvent, Category = "Demo|HUD")
	void OnClash();

	UFUNCTION(BlueprintImplementableEvent, Category = "Demo|HUD")
	void OnPerfectDodge();

	UFUNCTION(BlueprintImplementableEvent, Category = "Demo|HUD")
	void OnPlayerPoiseBroken();

	UFUNCTION(BlueprintImplementableEvent, Category = "Demo|HUD")
	void OnPlayerDeath();

	// ---------------- 布局开关（生成出来的控件也能在蓝图里改样式）----------------
	// 关掉它 = 完全用 UMG 里手拖的布局，C++ 不再插手
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|HUD|Layout")
	bool bBuildDefaultLayout = true;

	// 左下状态区：相对屏幕左下角的偏移（X 往右、Y 往下为负）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|HUD|Layout")
	FVector2D StatusPanelOffset = FVector2D(40.f, -44.f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|HUD|Layout")
	float StatusPanelWidth = 330.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|HUD|Layout")
	bool bShowStaminaBar = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|HUD|Layout")
	bool bShowPoiseBar = true;

	// 架势条：玩家"还能挡多久"。格挡扣、完美闪避/完美弹反回，打空即被破防。
	// 必须显示 —— 看不见的资源等于不存在，玩家只会觉得"怎么突然就被破防了"。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|HUD|Layout")
	bool bShowPostureBar = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|HUD|Layout")
	bool bShowDamageNumbers = true;

	// 左下角是否显示回血"酒"持有量（"酒 ×N/M"）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|HUD|Layout")
	bool bShowHealItemText = true;

	// 命中反馈大字的默认存活时长
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|HUD|Layout")
	float FeedbackTextLifetime = 1.15f;

	// 生成的控件（蓝图里可直接 Set Brush / 改字体）
	UPROPERTY(BlueprintReadOnly, Category = "Demo|HUD")
	TObjectPtr<UDemoStatusBarWidget> HealthBar;

	UPROPERTY(BlueprintReadOnly, Category = "Demo|HUD")
	TObjectPtr<UDemoStatusBarWidget> StaminaBar;

	UPROPERTY(BlueprintReadOnly, Category = "Demo|HUD")
	TObjectPtr<UDemoStatusBarWidget> PoiseBar;

	UPROPERTY(BlueprintReadOnly, Category = "Demo|HUD")
	TObjectPtr<UDemoStatusBarWidget> PostureBar;

	UPROPERTY(BlueprintReadOnly, Category = "Demo|HUD")
	TObjectPtr<UDemoDamageNumberLayer> DamageNumberLayer;

	UPROPERTY(BlueprintReadOnly, Category = "Demo|HUD")
	TObjectPtr<UTextBlock> FeedbackText;

	UPROPERTY(BlueprintReadOnly, Category = "Demo|HUD")
	TObjectPtr<UTextBlock> DeathText;

	// 回血"酒"持有量（"酒 ×N/M"），由 SyncFromCharacter 每帧刷新。
	UPROPERTY(BlueprintReadOnly, Category = "Demo|HUD")
	TObjectPtr<UTextBlock> HealItemText;

	// 全屏闪光层（最底层，完美判定时整屏泛光）
	UPROPERTY(BlueprintReadOnly, Category = "Demo|HUD")
	TObjectPtr<UImage> ScreenFlash;

	// ---------------- 锁定准星 ----------------
	// 锁定目标时在它身上叠一个四角括号准星。
	// 纯 C++ 用细长 Image 拼出来的，不需要任何贴图资产；想换成自己的样式
	// 就在 WBP 里把 LockOnMarkerRoot 的可见性关掉、用自己的控件做。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|HUD|LockOn")
	bool bShowLockOnMarker = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|HUD|LockOn")
	float LockOnMarkerSize = 58.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|HUD|LockOn")
	FLinearColor LockOnMarkerColor = FLinearColor(0.94f, 0.97f, 1.f, 0.95f);

	// 锁定/换目标时准星从大到小"咬住"的时长
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|HUD|LockOn")
	float LockOnMarkerIntroTime = 0.18f;

	UPROPERTY(BlueprintReadOnly, Category = "Demo|HUD")
	TObjectPtr<UCanvasPanel> LockOnMarkerRoot;

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

	void EnsureDefaultTree();
	void SyncFromCharacter();
	void SyncLockOnMarker(float InDeltaTime);

	// ---- 事件回调（动态委托用 UFUNCTION；普通委托用成员函数）----
	UFUNCTION()
	void HandleCharacterDeath();

	UFUNCTION()
	void HandleDamagedDetailed(float Damage, EDemoHitResultType ResultType, EDemoDamageSource DamageSource, FVector HitDirection);

	UFUNCTION()
	void HandlePoiseBroken();

	// GAS 事件：弹反成功 / 完美闪避（由 ApplyHit 发给双方）
	// 这几个走 AddUObject（GenericGameplayEventCallbacks），不需要 UFUNCTION。
	void HandleParryEvent(const struct FGameplayEventData* Payload);
	void HandleDodgeEvent(const struct FGameplayEventData* Payload);
	// 完美弹反（Event.Parry.Perfect）—— ApplyHit 在完美时【同时】发 Success 和 Perfect，
	// 所以两个回调都会跑到：Success 负责基础表现，Perfect 负责强化表现。
	void HandlePerfectParryEvent(const struct FGameplayEventData* Payload);
	// 普通闪避（Event.Dodge.Normal，无敌帧内但不在完美窗）
	void HandleNormalDodgeEvent(const struct FGameplayEventData* Payload);
	// 拼刀稳心（Event.Clash）—— 双方同窗挥刀，ResolveClash 已取消双方硬直 + 回架势，这里只做表现
	void HandleClashEvent(const struct FGameplayEventData* Payload);

	// ⚠ 必须 UFUNCTION：它是用 AddDynamic 绑到 OnHitLanded 动态多播委托上的，
	// 动态委托的绑定目标必须是反射函数，否则运行时会触发
	// “Unable to bind delegate to 'HandleHitLanded'” 的 ensure 失败，
	// 导致【所有命中飘字/反馈全部失效】（玩家既看不到伤害数字、也看不到命中表现）。
	// 之前的 HandleCharacterDeath 等都带 UFUNCTION，唯独这里漏了。
	UFUNCTION()
	void HandleHitLanded(AActor* HitActor, float Damage, EDemoHitResultType ResultType, FVector HitLocation);

	UPROPERTY(BlueprintReadOnly, Category = "Demo|HUD")
	TObjectPtr<ADemoCharacterBase> BoundCharacter;

	// 生成出来的布局根
	UPROPERTY(Transient)
	TObjectPtr<UCanvasPanel> RootCanvas;

private:
	float FeedbackTextRemaining = 0.f;
	bool bTreeReady = false;
	bool bDeathShown = false;

	// 准星用到的细条（只建一次，之后只改整块的缩放/位置）
	UPROPERTY(Transient)
	TArray<TObjectPtr<UImage>> LockOnMarkerBars;

	// <0 = 当前未锁定；>=0 = 入场动画已走过的秒数
	float LockOnMarkerAnim = -1.f;

	// 用来检测"换目标了"，从而重放入场动画
	TWeakObjectPtr<AActor> LastLockOnTarget;

	// ---- 全屏闪光状态（TickHUD 里线性淡出）----
	FLinearColor FlashColor = FLinearColor::White;
	float FlashRemaining = 0.f;
	float FlashDuration = 0.35f;
	float FlashPeakOpacity = 0.f;

	// HUD 每帧轮询属性的节流：没必要每帧都写一遍文字
	float SyncAccumulator = 0.f;
	float SyncInterval = 0.05f;

	// 上一帧同步过的数值 —— 只有真的变了才回调蓝图事件（避免每秒触发 20 次音效/特效）
	float LastHealthValue = -1.f;
	float LastHealthMax = -1.f;
	float LastStaminaValue = -1.f;
	float LastStaminaMax = -1.f;
	float LastPoiseValue = -1.f;
	float LastPoiseMax = -1.f;

	// "酒"持有量上一次刷新过的值（只有变了才重设文本，避免每帧 Invalidate Slate）
	int32 LastHealItemCount = -1;
	int32 LastHealItemMax = -1;

	// 架势条上一次是否处于"见底"状态（只在跨过阈值时换色，避免每帧重绘）
	bool bLastPostureLow = false;
};

// ==================================================================================
// BOSS 血条（屏幕顶部）
// ==================================================================================
UCLASS()
class DEMO_API UBossHealthBarWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "Demo|HUD")
	void BindToBoss(ABossCharacterBase* Boss);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|HUD")
	ABossCharacterBase* GetBoundBoss() const { return BoundBoss; }

	// 每帧驱动（由 AFightGameMode::Tick 调用，原因见 UCombatHUDWidget::TickHUD 注释）
	UFUNCTION(BlueprintCallable, Category = "Demo|HUD")
	void TickHUD(float DeltaTime);

	// ---- 蓝图事件 ----
	UFUNCTION(BlueprintImplementableEvent, Category = "Demo|HUD")
	void OnBossHealthChanged(float NewHealth, float MaxHealth);

	UFUNCTION(BlueprintImplementableEvent, Category = "Demo|HUD")
	void OnBossPoiseChanged(float NewPoise, float MaxPoise);

	UFUNCTION(BlueprintImplementableEvent, Category = "Demo|HUD")
	void OnBossPhaseChanged(int32 PhaseIndex);

	UFUNCTION(BlueprintImplementableEvent, Category = "Demo|HUD")
	void OnBossDeath();

	// BOSS 挨打的完整信息（飘字已在 C++ 里做）
	UFUNCTION(BlueprintImplementableEvent, Category = "Demo|HUD")
	void OnBossDamagedDetailed(float AppliedDamage, EDemoHitResultType ResultType,
		EDemoDamageSource DamageSource, FVector HitDirection);

	// 大招蓄力中（每帧刷新 RemainingSeconds，UI 可以做读条 / 闪红）
	UFUNCTION(BlueprintImplementableEvent, Category = "Demo|HUD")
	void OnBossUltimateCharging(float RemainingSeconds);

	// ---- 布局开关 ----
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|HUD|Layout")
	bool bBuildDefaultLayout = true;

	// 血条整体宽度（像素）；高度由三条内容决定
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|HUD|Layout")
	float BarWidth = 900.f;

	// 距屏幕顶部的距离
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|HUD|Layout")
	float TopOffset = 46.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|HUD|Layout")
	bool bShowName = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|HUD|Layout")
	bool bShowPoiseBar = true;

	// 双异常条（冲击=橙 / 元素=青）：挂在 BOSS 血条正下方。
	// 对照参考 Demo：这两条槽是"输出 → 失衡 → 击破"这条反馈链的读数。
	// 没有它们，玩家只能从"BOSS 偶尔变绿"去反推进度，读不出节奏，
	// 也就不知道"我该继续砍还是该去弹一刀"。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|HUD|Layout")
	bool bShowAnomalyBars = true;

	// 在血条上标出阶段分界（0.55 = 55% 处画一条竖线）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|HUD|Layout")
	bool bShowPhaseMarkers = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|HUD|Layout")
	TArray<float> PhaseMarkerRatios;

	// 二阶段时血条换色（狂化）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|HUD|Layout")
	FLinearColor Phase1FillColor = FLinearColor(0.76f, 0.13f, 0.11f, 1.f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Demo|HUD|Layout")
	FLinearColor Phase2FillColor = FLinearColor(0.96f, 0.52f, 0.08f, 1.f);

	// 生成的控件
	UPROPERTY(BlueprintReadOnly, Category = "Demo|HUD")
	TObjectPtr<UDemoStatusBarWidget> BossHealthBar;

	UPROPERTY(BlueprintReadOnly, Category = "Demo|HUD")
	TObjectPtr<UDemoStatusBarWidget> BossPoiseBar;

	// 双异常条：冲击槽（橙，由普攻连段/技能喂养）→ 满 = 失衡
	UPROPERTY(BlueprintReadOnly, Category = "Demo|HUD")
	TObjectPtr<UDemoStatusBarWidget> AnomalyImpactBar;

	// 双异常条：元素槽（青，由弹反/技能喂养）→ 失衡期间再满 = 击破
	UPROPERTY(BlueprintReadOnly, Category = "Demo|HUD")
	TObjectPtr<UDemoStatusBarWidget> AnomalyElementBar;

	// 状态浮动标签：UNSTABLE / BROKEN（状态信号规范要求的 UI 通道，
	// 另一条通道是 BOSS 的材质轮廓变色，两者必须同一帧出现）
	UPROPERTY(BlueprintReadOnly, Category = "Demo|HUD")
	TObjectPtr<UTextBlock> AnomalyStateText;

	UPROPERTY(BlueprintReadOnly, Category = "Demo|HUD")
	TObjectPtr<UTextBlock> BossNameText;

	UPROPERTY(BlueprintReadOnly, Category = "Demo|HUD")
	TObjectPtr<UTextBlock> BossPhaseText;

	UPROPERTY(BlueprintReadOnly, Category = "Demo|HUD")
	TObjectPtr<UTextBlock> BossHintText;

	// 起手预警条：显示"BOSS 还有多久出招"，并用颜色区分能不能弹反。
	// 这条 UI 是玩家能稳定打出弹反的前提 —— 详见 CombatHUDWidget.cpp 里的构建注释。
	UPROPERTY(BlueprintReadOnly, Category = "Demo|HUD")
	TObjectPtr<UDemoStatusBarWidget> TelegraphBar;

	UPROPERTY(BlueprintReadOnly, Category = "Demo|HUD")
	TObjectPtr<UDemoDamageNumberLayer> DamageNumberLayer;

	// "已造成伤害"读数。
	//
	// ⚠ 为什么要加这一行：BOSS 血量 10000、玩家单段普攻 28~56 —— 一刀只占血条的
	//   0.3%~0.6%，在 600px 宽的血条上就是 2~4 像素，肉眼根本看不出在掉。
	//   于是就有了"角色砍 BOSS 没扣血"的错觉（其实一直在正确结算）。
	//   这个累计数字 + 百分比是"确实在扣血"的直接证据，比血条长度可靠得多。
	UPROPERTY(BlueprintReadOnly, Category = "Demo|HUD")
	TObjectPtr<UTextBlock> DamageDealtText;

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

	void EnsureDefaultTree();
	void BuildPhaseMarkers();
	void SyncFromBoss();

	// 刷新"已造成伤害"读数（受击回调里累加，这里只负责写文本）
	void UpdateDamageDealtText();

	UPROPERTY(Transient)
	float AccumulatedDamage = 0.f;

	// 起手预警条：每帧更新（不走 SyncFromBoss 的 20Hz 节流）。
	//
	// 为什么单独给一条不节流的路径：起手窗通常只有 0.5~0.85s，
	// 20Hz 采样只有十来个点，进度条会一顿一顿的，
	// 而"条填满的瞬间"正是玩家要按键的时机 —— 这里不能省精度。
	void SyncTelegraph();

	// 双异常条读数（20Hz 足够：槽是慢变量，逐帧刷新只会白白让 Slate 重绘）
	void SyncAnomalyBars();

	// 状态标签（每帧查 tag：这是"跃迁瞬间"，晚一帧玩家就会觉得状态提示跟不上）
	void SyncAnomalyStateLabel();

	UFUNCTION()
	void HandleBossDeath();

	UFUNCTION()
	void HandleBossDamagedDetailed(float Damage, EDemoHitResultType ResultType, EDemoDamageSource DamageSource, FVector HitDirection);

	UFUNCTION()
	void HandleBossPhaseChanged(int32 PhaseIndex);

	UPROPERTY(BlueprintReadOnly, Category = "Demo|HUD")
	TObjectPtr<ABossCharacterBase> BoundBoss;

	UPROPERTY(Transient)
	TObjectPtr<UCanvasPanel> RootCanvas;

	// 阶段刻度线（每帧只需要 SetVisibility，不用重建）
	UPROPERTY(Transient)
	TArray<TObjectPtr<UImage>> PhaseMarkers;

private:
	// 上一次的状态标签（只在跃迁时改文字/颜色，避免每帧 Invalidate Slate）
	bool bLastUnstableShown = false;
	bool bLastBrokenShown = false;
	float AnomalyStatePulse = 0.f;

	int32 LastPhaseIndex = -1;
	bool bTreeReady = false;
	bool bDeathShown = false;
	float SyncAccumulator = 0.f;
	float SyncInterval = 0.05f;

	// 预警条当前是不是"可弹反"色 —— 只在翻转时写 Brush，避免每帧 Invalidate
	bool bLastTelegraphParriable = false;

	float LastBossHealth = -1.f;
	float LastBossHealthMax = -1.f;
	float LastBossPoise = -1.f;
	float LastBossPoiseMax = -1.f;
};
