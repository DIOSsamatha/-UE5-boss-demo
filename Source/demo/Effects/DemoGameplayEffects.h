// 战斗基础 GE 集合（C++ 直接定义，蓝图可继承覆盖数值）
//
// 注意：需要 GrantedTags / BlockAbilities / CancelAbilities 的 GE 继承自
//       UDemoGameplayEffectBase，Tag 配置在 ConfigureXxx 虚方法中完成
//       （UE 5.7 禁止在构造函数内调用 FindOrAddComponent）。
#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "Effects/DemoGameplayEffectHelpers.h"
#include "DemoGameplayEffects.generated.h"

// ====================================================================
//  基类：将 GE 组件配置从构造函数延迟到 PostInitProperties
//  （解决 UE 5.7 的 "NewObject with empty name" 崩溃）
// ====================================================================
UCLASS(Abstract)
class DEMO_API UDemoGameplayEffectBase : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UDemoGameplayEffectBase();

	// ---- 子类重写这些方法来注册 Tag 操作（在 PostInitProperties 中自动调用） ----

	virtual void ConfigureGrantedTags(FInheritedTagContainer& OutTags) {}
	virtual void ConfigureBlockedAbilities(FInheritedTagContainer& OutTags) {}
	virtual void CanceledAbilityOnApply(FInheritedTagContainer& OutTags) {}

protected:
	virtual void PostInitProperties() override;

private:
	void ApplyComponentConfig();
};

// ====================================================================
//  瞬时伤害：经 UDemoDamageExecution 计算最终数值（无组件需求）
// ====================================================================
UCLASS(BlueprintType)
class DEMO_API UDemoDamageEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UDemoDamageEffect();
};

// ---------------------------------------------------------------- 双异常条积累
// 瞬时 GE：两个 SetByCaller 通道，分别往冲击槽(橙) / 元素槽(青)里加积累量。
//
// 为什么异常积累必须走 GE 而不是直接改属性：
//   直接 SetAnomalyImpact 会绕过 PreAttributeChange / PostGameplayEffectExecute，
//   也就是绕过"打满 → 挂失衡 tag"这条状态流转，UI 与材质两条通道就都收不到信号；
//   而且跨帧直接改属性在 networked 场景下不会被复制，客户端和服务器会算出两套条。
// 用 GE + SetByCaller 就能让"谁打了多少、推进了哪条槽"全程可追溯、可复制。
UCLASS(BlueprintType)
class DEMO_API UGE_AnomalyGainEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGE_AnomalyGainEffect();
};

// ---------------------------------------------------------------- 架势条变化
// 瞬时 GE：一个 SetByCaller 通道，正数=返还、负数=格挡消耗。
// 走 GE 的理由同异常条：直接改属性不会触发"打空 → 破防"这条状态流转。
UCLASS(BlueprintType)
class DEMO_API UGE_PostureChangeEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGE_PostureChangeEffect();
};

// ---------------------------------------------------------------- 硬直
UCLASS(BlueprintType)
class DEMO_API UGE_StaggerEffect : public UDemoGameplayEffectBase
{
	GENERATED_BODY()
public:
	UGE_StaggerEffect();
	virtual void ConfigureGrantedTags(FInheritedTagContainer& OutTags) override;
	virtual void ConfigureBlockedAbilities(FInheritedTagContainer& OutTags) override;
	virtual void CanceledAbilityOnApply(FInheritedTagContainer& OutTags) override;
};

// ---------------------------------------------------------------- 破防大硬直
UCLASS(BlueprintType)
class DEMO_API UGE_BreakEffect : public UDemoGameplayEffectBase
{
	GENERATED_BODY()
public:
	UGE_BreakEffect();
	virtual void ConfigureGrantedTags(FInheritedTagContainer& OutTags) override;
	virtual void ConfigureBlockedAbilities(FInheritedTagContainer& OutTags) override;
	virtual void CanceledAbilityOnApply(FInheritedTagContainer& OutTags) override;
};

// ---------------------------------------------------------------- BOSS 普通受击短硬直
// 复用 State_Stagger：Brain 的全局判断（Tick 里"有 Stagger tag 就进 Stagger 状态"）
// 会自动接管 —— BOSS 被打中（非霸体、非破防）后短暂冻结，硬直结束后回到 Chase，
// 再经过 AttackInterval 的出招间隔才再次出招，正好实现"被攻击要有僵直，僵直 CD 后才攻击"。
// 时长远短于弹反/破防硬直，且 OnDamagedReaction 里做了"已是 Stagger 就不重复施加"的守卫，
// 不会被玩家连击无限刷新成永控。
UCLASS(BlueprintType)
class DEMO_API UGE_BossHitstunEffect : public UDemoGameplayEffectBase
{
	GENERATED_BODY()
public:
	UGE_BossHitstunEffect();
	virtual void ConfigureGrantedTags(FInheritedTagContainer& OutTags) override;
	virtual void ConfigureBlockedAbilities(FInheritedTagContainer& OutTags) override;
	virtual void CanceledAbilityOnApply(FInheritedTagContainer& OutTags) override;
};

// ---------------------------------------------------------------- 玩家受击僵直
// 被命中后短暂无法出招：命中瞬间【中断】当前普攻/技能，并在剩余时间内【封禁】它们。
//
// ⚠ 刻意【不封】闪避 / 弹反 / 格挡 —— 玩家永远保留"翻滚脱身"这条路。
//   动作游戏里受击僵直一旦把脱身手段也封了，玩家就会被打成"连击锁死"，
//   主观感受从"我失误了"变成"这游戏耍赖"，是新手劝退的主要来源之一。
//
// 用独立标签 State_Hitstun（不复用破防的 State_Stagger），避免和"架势被打空 → 破防"
// 那条链互相干扰 —— 玩家被打一下远没到破防的程度，两者不该共用状态。
UCLASS(BlueprintType)
class DEMO_API UGE_PlayerHitstunEffect : public UDemoGameplayEffectBase
{
	GENERATED_BODY()
public:
	UGE_PlayerHitstunEffect();
	virtual void ConfigureGrantedTags(FInheritedTagContainer& OutTags) override;
	virtual void ConfigureBlockedAbilities(FInheritedTagContainer& OutTags) override;
	virtual void CanceledAbilityOnApply(FInheritedTagContainer& OutTags) override;
};

// ---------------------------------------------------------------- 无敌帧
UCLASS(BlueprintType)
class DEMO_API UGE_InvincibleEffect : public UDemoGameplayEffectBase
{
	GENERATED_BODY()
public:
	UGE_InvincibleEffect();
	virtual void ConfigureGrantedTags(FInheritedTagContainer& OutTags) override;
};

// ---------------------------------------------------------------- 降级无敌帧（低体力闪避用）
// 体力低于 StaminaDodgeWeakRatio(15%) 时，闪避改用这条短无敌帧替代 UGE_InvincibleEffect。
// 不改成"给原 GE 传动态时长"：那条路径被多处复用（完美闪避窗也依赖它的节奏），
// 动它容易把正常闪避的手感一起改坏。单独一条 GE 的代价只有一个类，却把影响面锁死。
UCLASS(BlueprintType)
class DEMO_API UGE_InvincibleWeakEffect : public UDemoGameplayEffectBase
{
	GENERATED_BODY()
public:
	UGE_InvincibleWeakEffect();
	virtual void ConfigureGrantedTags(FInheritedTagContainer& OutTags) override;
};

// ---------------------------------------------------------------- 完美闪避派生窗
// 完美闪避成功时挂在玩家身上，只活 PerfectDodgeFollowupWindow（0.45s）。
// 这段时间内按攻击 → FightPlayerCharacter 取消闪避后摇、直接接招（"闪A"派生）。
// 普通闪避不挂这个 tag，所以只有"卡着出招瞬间闪"才能派生，无脑连按闪避没用。
UCLASS(BlueprintType)
class DEMO_API UGE_DodgePerfectFollowupEffect : public UDemoGameplayEffectBase
{
	GENERATED_BODY()
public:
	UGE_DodgePerfectFollowupEffect();
	virtual void ConfigureGrantedTags(FInheritedTagContainer& OutTags) override;
};

// ---------------------------------------------------------------- 完美闪避判定窗
// 闪避一开始就挂上，只活 0.22s（DemoBalance::PerfectDodgeWindow）。
// 这 0.22s 内被打到 → ApplyHit 判定为【完美闪避】：子弹时间 + 屏幕泛白 + 清闪避冷却。
// 闪避的无敌帧（UGE_InvincibleEffect）比它长得多 —— 那个管"免不免伤"，
// 本 Effect 只管"免伤的同时算不算打得好"。
UCLASS(BlueprintType)
class DEMO_API UGE_PerfectDodgeWindowEffect : public UDemoGameplayEffectBase
{
	GENERATED_BODY()
public:
	UGE_PerfectDodgeWindowEffect();
	virtual void ConfigureGrantedTags(FInheritedTagContainer& OutTags) override;
};

// ---------------------------------------------------------------- 完美弹反判定窗
// 弹刀一开始挂上，只活 0.12s（DemoBalance::PerfectParryWindow）。
// 比闪避的窗窄得多 —— 弹反回报更高（攻击方长硬直 + 额外削韧），
// 入口严格一点，玩家才有"练出来的手感"。
UCLASS(BlueprintType)
class DEMO_API UGE_PerfectParryWindowEffect : public UDemoGameplayEffectBase
{
	GENERATED_BODY()
public:
	UGE_PerfectParryWindowEffect();
	virtual void ConfigureGrantedTags(FInheritedTagContainer& OutTags) override;
};

// ---------------------------------------------------------------- 完美判定奖励（回耐力）
// 完美闪避 / 完美弹反成功时发给玩家：Instant 加一笔耐力。
// 用 GE 而不是直接调 AttributeSet 的 Setter —— Clamp、属性变化广播、
// 以及后续可能加的"耐力上限 buff"都能正常参与运算。
UCLASS(BlueprintType)
class DEMO_API UGE_PerfectRewardEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGE_PerfectRewardEffect();
};

// ---------------------------------------------------------------- 完美闪避专用返还
// 与完美弹反（UGE_PerfectRewardEffect，+25）分开：闪避返还 20，比弹反少。
// 理由是两者风险不同 —— 闪避只要"贴着攻击按 Shift"，弹反要"正面迎上去卡窗口"，
// 回报必须拉开档次，否则玩家会退化成只用更安全的那一个。
UCLASS(BlueprintType)
class DEMO_API UGE_PerfectDodgeRewardEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGE_PerfectDodgeRewardEffect();
};

// ---------------------------------------------------------------- 弹反窗口
UCLASS(BlueprintType)
class DEMO_API UGE_ParryWindowEffect : public UDemoGameplayEffectBase
{
	GENERATED_BODY()
public:
	UGE_ParryWindowEffect();
	virtual void ConfigureGrantedTags(FInheritedTagContainer& OutTags) override;
};

// ---------------------------------------------------------------- 完美弹反专用的大硬直
// 继承普通硬直，只把时长拉长（1.3s vs 0.6s）—— 这样"弹反成功"的两种档位
// 共用同一套"封技能 + 中断出招"配置，不会出现两处配置慢慢走岔。
// 完美弹反把 BOSS 钉在原地 1.3s，玩家足够接一套 3~4 段连击，
// 这是"高风险高回报"里回报的那一半。
UCLASS(BlueprintType)
class DEMO_API UGE_PerfectParryStaggerEffect : public UGE_StaggerEffect
{
	GENERATED_BODY()
public:
	UGE_PerfectParryStaggerEffect();
	// ⚠ 关键改动：完美弹反的硬直【不】自动取消 BOSS 的出招能力。
	//   BOSS 招式是否可被完美弹反打断，改由 ApplyHit 按招式的 bInterruptibleByParry 显式决定
	//   （重型招式如突进斩/大招不可打断 → 完美弹反只免伤+奖励、不冻结、不打断动画）。
	//   这里只保留"封玩家自己的普攻/技能"，避免玩家在 BOSS 硬直期间还能乱按出招。
	virtual void CanceledAbilityOnApply(FInheritedTagContainer& OutTags) override;
};

// ---------------------------------------------------------------- 误弹重招的短硬直
// 用普通弹反去接【不可打断】的重型招式（突进斩 / 大招）时，玩家自己吃这一下。
// 继承普通硬直、只把时长换成 0.4s —— 与完美弹反硬直同一个套路（继承改时长），
// 好处是"封技能 / 中断出招"这些配置和普通硬直永远同步，不会各自走岔。
// 0.4s 是刻意选的：够疼到让人记住"这招弹不得"，但不至于像被一击判死刑。
UCLASS(BlueprintType)
class DEMO_API UGE_MisParryStunEffect : public UGE_StaggerEffect
{
	GENERATED_BODY()
public:
	UGE_MisParryStunEffect();
};

// ---------------------------------------------------------------- 格挡状态
UCLASS(BlueprintType)
class DEMO_API UGE_BlockingEffect : public UDemoGameplayEffectBase
{
	GENERATED_BODY()
public:
	UGE_BlockingEffect();
	virtual void ConfigureGrantedTags(FInheritedTagContainer& OutTags) override;
};

// ---------------------------------------------------------------- 韧性恢复（无组件需求）
UCLASS(BlueprintType)
class DEMO_API UGE_PoiseRecoveryEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGE_PoiseRecoveryEffect();
};

// ---------------------------------------------------------------- 耐力持续恢复（无组件需求）
UCLASS(BlueprintType)
class DEMO_API UGE_StaminaRegenEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGE_StaminaRegenEffect();
};

// ---------------------------------------------------------------- 闪避耐力消耗（无组件需求）
UCLASS(BlueprintType)
class DEMO_API UGE_DodgeCostEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGE_DodgeCostEffect();
};

// ---------------------------------------------------------------- 格挡受击消耗（无组件需求）
UCLASS(BlueprintType)
class DEMO_API UGE_BlockImpactCostEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGE_BlockImpactCostEffect();
};

// ---------------------------------------------------------------- 通用体力消耗（无组件需求）
// 数值由调用方用 SetByCaller（DemoTag::Data_StaminaCost）塞进来，传【正数】= 扣多少。
// 一个 GE 覆盖"普攻连段每段不同价 / 技能 25 / 空弹 5"全部场景，不必为每个数值建一个类。
UCLASS(BlueprintType)
class DEMO_API UGE_StaminaCostEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGE_StaminaCostEffect();
};

// ---------------------------------------------------------------- 体力耗尽（软惩罚）
// 体力被扣到 0 时挂上：持续 2.5s，期间
//   · 授予 State_StaminaExhausted（技能侧据此拒绝弹反、普攻伤害打折）
//   · 每 0.1s 扣 1.5 体力增量，与恢复 GE(+3/0.1s) 抵消后净恢复降到 +15/s
// ⚠ 是"变弱"不是"坐牢"：不封移动、不硬控，玩家仍能走位拉扯。
UCLASS(BlueprintType)
class DEMO_API UGE_StaminaExhaustEffect : public UDemoGameplayEffectBase
{
	GENERATED_BODY()
public:
	UGE_StaminaExhaustEffect();
	virtual void ConfigureGrantedTags(FInheritedTagContainer& OutTags) override;
};

// ---------------------------------------------------------------- 技能冷却
UCLASS(BlueprintType)
class DEMO_API UGE_SkillCooldownEffect : public UDemoGameplayEffectBase
{
	GENERATED_BODY()
public:
	UGE_SkillCooldownEffect();
	virtual void ConfigureGrantedTags(FInheritedTagContainer& OutTags) override;
};

// ---------------------------------------------------------------- 闪避冷却
UCLASS(BlueprintType)
class DEMO_API UGE_DodgeCooldownEffect : public UDemoGameplayEffectBase
{
	GENERATED_BODY()
public:
	UGE_DodgeCooldownEffect();
	virtual void ConfigureGrantedTags(FInheritedTagContainer& OutTags) override;
};

// ================================================================ 道具：回血"酒"
// 瞬时 GE：往 Health 上加一笔（具体数值由调用方用 SetByCaller 塞入 = 最大生命 × 30%）。
// 走 GE 才能触发 PostGameplayEffectExecute 里的 clamp + OnHealthChanged 广播，
// 让血条 / HUD / 回血反馈全部正常联动（直接 SetHealth 不会走这条链）。
// 与 UGE_StaminaCostEffect 同套路：自己只负责"把 SetByCaller 的通道数送到 Health"，数值由外部定。
UCLASS(BlueprintType)
class DEMO_API UGE_HealEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGE_HealEffect();
};

// ---------------------------------------------------------------- 喝酒冷却
// 喝酒成功后挂上：持续 HealCooldown 秒，期间再按喝酒键会被 CD 挡住。
UCLASS(BlueprintType)
class DEMO_API UGE_HealCooldownEffect : public UDemoGameplayEffectBase
{
	GENERATED_BODY()
public:
	UGE_HealCooldownEffect();
	virtual void ConfigureGrantedTags(FInheritedTagContainer& OutTags) override;
};
