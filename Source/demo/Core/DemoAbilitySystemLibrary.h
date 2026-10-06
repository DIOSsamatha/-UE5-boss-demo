// GAS 统一入口：取 ASC / 取属性 / 伤害结算 / 施加状态效果
// 所有战斗逻辑都从这里走，方便统一处理弹反、格挡、无敌帧等交互
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GameplayTagContainer.h"
#include "GameplayEffectTypes.h"
#include "DemoCommon.h"
#include "DemoAbilitySystemLibrary.generated.h"

class UAbilitySystemComponent;
class UDemoAttributeSet;
class UGameplayEffect;

UCLASS()
class DEMO_API UDemoAbilitySystemLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|GAS")
	static UAbilitySystemComponent* GetAbilitySystemComponent(const AActor* Actor);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|GAS")
	static UDemoAttributeSet* GetAttributeSet(const AActor* Actor);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|GAS")
	static bool HasTag(const AActor* Actor, FGameplayTag Tag);

	// 核心：结算一次命中，返回本次命中的类型（普通 / 格挡 / 弹反 / 闪避 / 暴击）
	//
	// 内部按顺序做这些"拦截"，任何一条命中都会直接返回、不产生伤害：
	//   1. 空指针 / 自己打自己
	//   2. 目标已死亡
	//   3. 阵营不敌对（同队）—— 除非开了控制台开关 demo.FriendlyFire 1
	//   4. 目标处于无敌帧（闪避成功）→ 返回 Dodged 并广播完美闪避事件
	//   5. 目标处于弹反窗口且这招可被弹反 → 返回 Parried（弹反方吃无敌 + 攻击方吃大硬直）
	//   6. 目标处于格挡且这招可被格挡 → 伤害 ×0.25、削韧 ×0.5、扣耐力
	// 破防状态下命中自动视为暴击。返回值是给调用方做表现用的（例：只有真的打中才播音效）。
	UFUNCTION(BlueprintCallable, Category = "Demo|Combat")
	static EDemoHitResultType ApplyHit(AActor* SourceActor, AActor* TargetActor, float BaseDamage, float PoiseDamage,
		bool bCanBeParried, bool bCanBeBlocked, const FVector& HitFromLocation, bool bTriggerHitStop = true,
		EDemoDamageSource DamageSource = EDemoDamageSource::Unknown);

	// ---------- 敌我关系 ----------
	// 两个 Actor 是否敌对（任一方不是战斗单位 → false）
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|Combat")
	static bool IsHostile(const AActor* A, const AActor* B);

	// 这一次攻击能不能真的造成伤害（= 敌对 且 目标活着 且 不是自己）
	// 武器扫掠 / 弹幕 / AI 选目标都该先问它，避免"对着友军挥空"或"打尸体"
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|Combat")
	static bool CanDamage(const AActor* SourceActor, const AActor* TargetActor);

	// 找出 SourceActor 附近一定半径内**最近的**敌人（AI 选目标 / 技能选冲刺方向用）
	UFUNCTION(BlueprintCallable, Category = "Demo|Combat")
	static AActor* FindNearestHostile(AActor* SourceActor, float Radius, bool bRequireLineOfSight = false);

	// 找出半径内的所有敌人（弹幕命中、范围伤害用）
	UFUNCTION(BlueprintCallable, Category = "Demo|Combat")
	static void FindHostilesInRadius(const AActor* SourceActor, const FVector& Origin, float Radius,
		TArray<AActor*>& OutActors, bool bIncludeDead = false);

	// 阵营敌对关系总开关（等价于控制台 demo.FriendlyFire 1）
	UFUNCTION(BlueprintCallable, Category = "Demo|Combat")
	static void SetAllowFriendlyFire(bool bAllow);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|Combat")
	static bool IsFriendlyFireAllowed();

	// 施加任意 GE（状态效果、恢复、增益等）
	UFUNCTION(BlueprintCallable, Category = "Demo|Combat")
	static FActiveGameplayEffectHandle ApplyEffectToSelf(AActor* TargetActor, TSubclassOf<UGameplayEffect> EffectClass);

	UFUNCTION(BlueprintCallable, Category = "Demo|Combat")
	static FActiveGameplayEffectHandle ApplyEffectToTarget(AActor* SourceActor, AActor* TargetActor, TSubclassOf<UGameplayEffect> EffectClass);

	UFUNCTION(BlueprintCallable, Category = "Demo|Combat")
	static void RemoveActiveEffect(AActor* TargetActor, FActiveGameplayEffectHandle Handle);

	// 让目标进入硬直（弹反成功、破防时使用）
	UFUNCTION(BlueprintCallable, Category = "Demo|Combat")
	static void ApplyStagger(AActor* TargetActor, bool bIsBreak = false);

	// BOSS 普通受击的【短硬直】（非霸体、非破防时被命中触发）。
	// 时长很短（Core/DemoCombatTuning.h 的 BossHitstunTime），配合 Brain 的 AttackInterval
	// 实现"被攻击要有僵直，僵直 CD 后才攻击"；OnDamagedReaction 里有"已是 Stagger 就不重复施加"
	// 的守卫，避免被玩家连击无限刷新成永控。
	// ⚠ DurationScale：轻击/重击分级用。轻击传 <1（更短的"踉跄"而非整段硬直），重击传 1.0。
	UFUNCTION(BlueprintCallable, Category = "Demo|Combat")
	static void ApplyBossHitstun(AActor* TargetActor, float DurationScale = 1.f);

	// 玩家受击僵直：命中落地后给【受击方（玩家）】挂短僵直（UGE_PlayerHitstunEffect）。
	// 与 BOSS 侧 ApplyBossHitstun 对称，但封的是玩家的【进攻手段】、保留【防御手段】：
	// 被打中后短暂挥不了刀，但随时能翻滚/弹反脱身 —— 避免被打成连击锁死。
	UFUNCTION(BlueprintCallable, Category = "Demo|Combat")
	static void ApplyPlayerHitstun(AActor* TargetActor);

	// 完美弹反专用：更长的大硬直（时长见 Core/DemoCombatTuning.h 的 PerfectParryStaggerTime）。
	// 单独开一个入口而不是给 ApplyStagger 加参数，是为了让"完美弹反"这条路径
	// 在调用点上就一眼可辨，不会被误当成普通硬直。
	UFUNCTION(BlueprintCallable, Category = "Demo|Combat")
	static void ApplyPerfectParryStagger(AActor* TargetActor);

	// 只削韧、不造成任何伤害。
	//
	// 完美弹反的额外奖励：把攻击方的韧性打掉一截，让玩家"主动去弹"能更快把 BOSS 打进破防。
	// 实现上仍然走 UDemoDamageEffect —— 只有走 GE 的 Execution 才能触发
	// AttributeSet 里"韧性清零 → 破防"的判定链；
	// 直接调 SetPoise() 虽然数字会变，但永远不会破防（这是个很隐蔽的坑）。
	UFUNCTION(BlueprintCallable, Category = "Demo|Combat")
	static void ApplyPoiseDamageOnly(AActor* SourceActor, AActor* TargetActor, float PoiseDamage);

	// 双异常条积累：往目标身上加「冲击(橙) / 元素(青)」两条槽的积累量。
	//
	// 这是反馈链的起点：命中 → 异常积累 → 槽满 → 失衡/击破 → 输出窗口 → 再命中。
	// 必须走 GE（UGE_AnomalyGainEffect）而不是直接改属性，理由同 ApplyPoiseDamageOnly：
	// 直接改属性不会经过 PostGameplayEffectExecute，也就不会触发"打满 → 挂失衡标签"，
	// UI 与材质两条信号通道都会哑火。
	UFUNCTION(BlueprintCallable, Category = "Demo|Combat")
	static void ApplyAnomalyGain(AActor* SourceActor, AActor* TargetActor, float ImpactGain, float ElementGain);

	// 架势条变化：正数=返还（完美闪避 / 完美弹反"稳心"），负数=格挡消耗。
	// 同样必须走 GE：直接改属性不会触发"架势打空 → 被破防"这条链。
	UFUNCTION(BlueprintCallable, Category = "Demo|Combat")
	static void ApplyPostureChange(AActor* SourceActor, AActor* TargetActor, float DeltaPosture);

	// 通用体力消耗：Cost 传【正数】= 扣多少（内部取负后走 UGE_StaminaCostEffect）。
	// 扣完立即检测是否见底 —— 归零就挂 UGE_StaminaExhaustEffect（软惩罚，2.5s 恢复减半）。
	//
	// 为什么收成一个 helper 而不是各处自己拼 Spec：普攻连段四段价位不同、技能 25、空弹 5，
	// 任何一个调用点漏掉"见底检测"，耗尽状态就会时灵时不灵，而这类 bug 极难复现。
	UFUNCTION(BlueprintCallable, Category = "Demo|Combat")
	static void ApplyStaminaCost(AActor* TargetActor, float Cost);

	// 拼刀（双方同一窗口内都在挥刀）→ 稳心：抵消双方硬直 + 回一截架势。
	// 参考 Demo 的"拼刀"设计：它是第三种防御性交互，和弹反/闪避平级，
	// 让玩家在"挡不住也来不及闪"时还有一个正面解法，而不是只能挨打。
	UFUNCTION(BlueprintCallable, Category = "Demo|Combat")
	static void ResolveClash(AActor* A, AActor* B);

	// 纯掉血、不削韧（用于"被破防那一下多吃的部分伤害"）。
	// 语义上必须和 ApplyPoiseDamageOnly 严格分开：那是只削韧不掉血，这是只掉血不削韧。
	UFUNCTION(BlueprintCallable, Category = "Demo|Combat")
	static void ApplyDamageOnly(AActor* SourceActor, AActor* TargetActor, float Damage);

	// 计算受击方向，用于挑选受击动画
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|Combat")
	static EDemoHitDirection GetHitDirection(const AActor* TargetActor, const FVector& HitFromLocation);
};
