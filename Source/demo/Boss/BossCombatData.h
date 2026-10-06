// BOSS 招式与阶段的数据资产 —— 招式池、距离条件、弹幕/突进参数、二阶段强化都在这儿配
//
// 设计思路：
//   一个 UBossAttackData = 一招。它自带「类型」（近战挥砍 / 远程弹幕 / 突进斩 / 大招），
//   技能按类型分派，Brain 只负责「什么时候出哪一招」。
//   二阶段不需要另建一套资产：把同一份 AttackData 放进 Phase2 的招式池，
//   勾上 bUpgradeInPhase2，运行时会自动按 Phase2* 那几个倍率强化（打得更疼、更快、弹更多）。
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Combat/DemoProjectile.h"
#include "BossCombatData.generated.h"

class UAnimMontage;
class UGameplayEffect;
class UCameraShakeBase;

// ------------------------------------------------------------------ 招式类型
UENUM(BlueprintType)
enum class EBossAttackType : uint8
{
	MeleeSlash		UMETA(DisplayName = "近战挥砍"),
	RangedBarrage	UMETA(DisplayName = "远程弹幕"),
	// 突进斩：蓄势 → 匀速突进拉近距离 → 收尾重斩 + 冲击。原来这里是「跳砸」，
	// 已按需求整体替换（跳砸那套抛物线滞空逻辑连同参数一起删干净了）。
	ChargeSlash		UMETA(DisplayName = "突进斩"),
	Ultimate		UMETA(DisplayName = "大招")
};

// ------------------------------------------------------------------ 弹幕参数
USTRUCT(BlueprintType)
struct FBossBarrageParams
{
	GENERATED_BODY()

	// 弹体类。留空则用原生 ADemoProjectile（无外观，但能造成伤害）
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Barrage")
	TSubclassOf<ADemoProjectile> ProjectileClass;

	// 单次齐射的弹数
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Barrage")
	int32 ProjectileCount = 5;

	// 扇形总张角（度）。0 = 所有弹重叠成一条直线；180 = 半圆
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Barrage")
	float SpreadAngleDeg = 40.f;

	// 环形齐射：true = 360° 均匀一圈（SpreadAngleDeg 失效），用于大招 / 落地震荡波
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Barrage")
	bool bRingPattern = false;

	// 环形齐射时的起始角度偏移（度），让连续几圈错开，视觉上更好看
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Barrage")
	float RingAngleOffsetDeg = 0.f;

	// 每发之间的间隔（秒）。0 = 同时齐射；>0 = 像机枪一样扫出去
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Barrage")
	float SpawnInterval = 0.f;

	// 发射点骨骼插槽。留空 / 找不到时用「角色前方偏移」兜底
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Barrage")
	FName SpawnSocket = TEXT("WeaponEnd");

	// 兜底发射点：角色位置 + 前方 * ForwardOffset + 上方 * HeightOffset
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Barrage")
	float SpawnForwardOffset = 70.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Barrage")
	float SpawnHeightOffset = 50.f;

	// 是否朝玩家方向发射（false = 朝角色正前方）
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Barrage")
	bool bAimAtTarget = true;

	// 预判系数：按玩家当前速度提前量瞄准。0 = 不预判，1 = 完全预判
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Barrage")
	float AimLeadScale = 0.f;

	// 单颗弹的飞行参数（速度 / 伤害 / 重力 / 穿透 等）
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Barrage")
	FBossProjectileParams Projectile;
};

// ------------------------------------------------------------------ 突进斩参数
//
// 位移怎么做（这一点决定了手感好不好）：
//   突进用的是 GAS 的 UAbilityTask_ApplyRootMotionConstantForce，匀速推固定距离。
//   ⚠ 不要用 LaunchCharacter 给初速度来"控距离"：实际位移 = v²/(2a)，
//     而 a 由 BrakingDecelerationWalking + GroundFriction + BrakingFriction 共同决定，
//     外部看不见也调不准（本项目在闪避上实打实踩过这个坑，650/500/350 一路试都"不对"）。
//     匀速推力则是「距离 = 速度 × 时长」，填多少就是多少，碰撞还安全。
USTRUCT(BlueprintType)
struct FBossChargeParams
{
	GENERATED_BODY()

	// 突进的水平距离（cm）—— 这就是真正会前进的距离
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Charge")
	float ChargeDistance = 520.f;

	// 突进耗时（秒）。⚠ 现在它是【上限参考值】而不是硬值：
	//   实际时长 = Clamp(距离 / ChargeSpeed, MinChargeDuration, MaxChargeDuration)
	// 以前直接拿它当固定时长用，导致速度 = 距离/0.26 随距离暴涨
	//（961cm 时能到 3700cm/s，BOSS 像炮弹一样撞过来，玩家被撞飞）。
	// 速度恒定后，距离远只是"冲得久一点"，撞击手感不会再失控。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Charge")
	float ChargeDuration = 0.26f;

	// 突进速度（cm/s）。真正决定"冲得有多凶"的是这个值，不是 ChargeDuration。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Charge")
	float ChargeSpeed = 1100.f;

	// 时长的上下夹：近了不至于瞬移，远了不至于慢吞吞
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Charge")
	float MinChargeDuration = 0.22f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Charge")
	float MaxChargeDuration = 0.70f;

	// 单次突进的距离上限（玩家太远时截断，避免冲出关卡边界）
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Charge")
	float MaxChargeDistance = 900.f;

	// 收尾时停在目标身前多远。留一点距离是为了让"收尾重斩"看起来是砍在刀尖上，
	// 而不是整个人糊在玩家身上。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Charge")
	float StopDistance = 170.f;

	// ---- 拖刀（突进途中就造成伤害）----
	// 打开后突进全程算作一次命中判定：玩家原地不动就会被"拖到"。
	// 关掉则只有收尾重斩有判定 —— 那样突进就纯粹是位移，更容易躲。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Charge")
	bool bDamageAlongDash = true;

	// 拖刀伤害 / 削韧（0 = 用招式本体的伤害与削韧）
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Charge")
	float DashDamage = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Charge")
	float DashPoiseDamage = 0.f;

	// ---- 收尾重斩 ----
	// 突进结束后接的重斩蒙太奇（留空 = 直接结算，不播动作）
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Charge")
	TObjectPtr<UAnimMontage> ImpactMontage;

	// 收尾判定：以 BOSS 为圆心、朝向正前方的一个扇形
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Charge")
	float ImpactRadius = 300.f;

	// 扇形半角（度）。90 = 前方半圆；180 = 整圈（等于不做方向过滤）
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Charge")
	float ImpactHalfAngleDeg = 75.f;

	// 收尾伤害 / 削韧（0 = 用招式本体的伤害与削韧）
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Charge")
	float ImpactDamage = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Charge")
	float ImpactPoiseDamage = 0.f;

	// 收尾能不能被弹反。红色重击类建议设 false —— 逼玩家闪避而不是站着弹。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Charge")
	bool bImpactCanBeParried = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Charge")
	bool bImpactCanBeBlocked = true;

	// 击退：水平推力 / 上抛速度（0 = 不击退）
	// ⚠ 这两个值会被 DemoBalance::BossMaxKnockback* 再夹一次（见 BossCharacterBase::ApplyAreaImpact）：
	//   上抛硬夹成 0（永不把玩家掀离地面），水平最多 110cm/s —— 只保留"被震开一下"的反馈。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Charge")
	float KnockbackStrength = 110.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Charge")
	float KnockbackUpward = 0.f;

	// 收尾是否再向外放一圈震荡波（二阶段 / 大招收尾用）
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Charge")
	bool bSpawnShockwave = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Charge")
	FBossBarrageParams Shockwave;
};

// ------------------------------------------------------------------ 单个招式
UCLASS(BlueprintType)
class DEMO_API UBossAttackData : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack")
	FName AttackName = TEXT("Attack");

	// 招式类型：决定交给哪个技能执行（近战挥砍 / 远程弹幕 / 突进斩 / 大招）
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack")
	EBossAttackType AttackType = EBossAttackType::MeleeSlash;

	// 该招式使用的蒙太奇。
	// 近战挥砍 / 突进斩的收尾斩，判定窗口都是按这个蒙太奇时长的比例推出来的。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack")
	TObjectPtr<UAnimMontage> Montage;

	// 蒙太奇播放速率（1 = 原速）。二阶段还会再乘 Phase2SpeedScale
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack")
	float PlayRate = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack")
	float Damage = 20.f;

	// 削韧值，累计打空玩家韧性就会破防
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack")
	float PoiseDamage = 15.f;

	// 起手预警时长：这段时间玩家要读招并决定格挡 / 闪避 / 弹反
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack")
	float TelegraphTime = 0.6f;

	// 招式结束后的空档，玩家可以反击
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack")
	float RecoverTime = 0.8f;

	// 出招期间是否霸体（默认是：BOSS 出招不会被普通攻击打断，只能靠打空韧性破防）
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack")
	bool bSuperArmor = true;

	// 起手时是否转向玩家（近战挥砍、突进斩都需要；弹幕通常也需要）
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack")
	bool bFaceTargetOnStart = true;

	// 该招式期望的攻击距离
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack")
	float IdealRange = 250.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack")
	float RangeTolerance = 120.f;

	// 忽略距离判断：任何距离都能放（大招一般勾上）
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack")
	bool bIgnoreRangeCheck = false;

	// 红色重击类招式建议设为 false，逼玩家用闪避而不是弹反
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack")
	bool bCanBeParried = true;

	// 能否被【完美弹反打断动画】：true = 完美弹反会取消当前招式并给长硬直；
	// false = 即使被完美弹反也只免伤+奖励，招式继续（玩家必须改走闪避）。
	// 重型招式（突进斩/大招）建议 false —— 这是"完美格挡只打断部分攻击"的实现依据。
	// 注意：bCanBeParried=true 但 bInterruptibleByParry=false 是合法组合
	// （突进斩蓄势可弹反免伤，但不被打断，仍要躲后续突进）。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack")
	bool bInterruptibleByParry = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack")
	bool bCanBeBlocked = true;

	// 招式池中的抽取权重
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack")
	float Weight = 1.f;

	// 同一招式的重复冷却
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack")
	float Cooldown = 3.f;

	// 只在二阶段可用（大招一般勾上）
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack")
	bool bOnlyInPhase2 = false;

	// ============================ 连招链 ============================
	// 出招结束（进入后摇之前）有概率立刻接下一招，形成 2~3 连。
	// 例：挥砍 → 挥砍 → 突进斩，压迫感比单发出招强得多。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack|Chain")
	TArray<TObjectPtr<UBossAttackData>> FollowUpAttacks;

	// 接续概率（0~1）。二阶段会按 Phase2FollowUpBonus 加成
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack|Chain")
	float FollowUpChance = 0.f;

	// 接续时跳过起手预警（已经贴身连打了，再给一次长预警会很拖沓）
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack|Chain")
	float FollowUpTelegraphScale = 0.35f;

	// ============================ 弹幕参数 ============================
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack|Ranged")
	FBossBarrageParams Barrage;

	// ============================ 突进斩参数 ============================
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack|Charge")
	FBossChargeParams Charge;

	// ============================ 大招参数 ============================
	// 蓄力起手（例如 In-Place/Buff_In）
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack|Ultimate")
	TObjectPtr<UAnimMontage> ChargeMontage;

	// 蓄力循环（例如 In-Place/Buff_Loop）—— 循环 ChargeDuration 秒，玩家有时间跑位
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack|Ultimate")
	TObjectPtr<UAnimMontage> ChargeLoopMontage;

	// 收力（例如 In-Place/Buff_out），可留空
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack|Ultimate")
	TObjectPtr<UAnimMontage> ChargeOutMontage;

	// 蓄力循环持续时长（秒）—— 这是玩家的“跑位窗口”，别太短
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack|Ultimate")
	float ChargeDuration = 1.6f;

	// 蓄力期间是否无敌（默认否：可以被打断，但需要打空韧性）
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack|Ultimate")
	bool bInvincibleWhileCharging = false;

	// 环形弹幕圈数
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack|Ultimate")
	int32 RingCount = 3;

	// 每圈弹数
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack|Ultimate")
	int32 ProjectilesPerRing = 12;

	// 每圈间隔（秒）
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack|Ultimate")
	float RingInterval = 0.35f;

	// 每圈角度错开量（度）
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack|Ultimate")
	float RingAngleStepDeg = 15.f;

	// 大招结尾是否接一次突进斩收尾（冲到玩家面前 → 大范围冲击 + 震荡波，
	// 把被弹幕圈逼到远处、或者躲进角落的玩家重新抓回战场中央）
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack|Ultimate")
	bool bEndWithChargeFinisher = true;

	// ==================== 二阶段强化（复用同一份资产时的倍率） ====================
	// 打开后，这一招在二阶段会被下面这些倍率强化 —— 不需要另建一套数据
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack|Phase2")
	bool bUpgradeInPhase2 = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack|Phase2")
	float Phase2DamageScale = 1.35f;

	// 动作更快（蒙太奇播放速率倍率）
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack|Phase2")
	float Phase2SpeedScale = 1.15f;

	// 起手预警更短（< 1 = 更难读招）
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack|Phase2")
	float Phase2TelegraphScale = 0.7f;

	// 后摇更短（< 1 = 反击窗口更窄）
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack|Phase2")
	float Phase2RecoverScale = 0.75f;

	// 冷却更短（< 1 = 出招更密）
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack|Phase2")
	float Phase2CooldownScale = 0.7f;

	// 弹幕：额外弹数 / 额外张角
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack|Phase2")
	int32 Phase2ExtraProjectiles = 4;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack|Phase2")
	float Phase2ExtraSpreadDeg = 0.f;

	// 弹速倍率
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack|Phase2")
	float Phase2ProjectileSpeedScale = 1.2f;

	// 突进斩：冲得更远 / 收尾范围更大
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack|Phase2")
	float Phase2ChargeDistanceScale = 1.3f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack|Phase2")
	float Phase2ImpactRadiusScale = 1.25f;

	// 连招链：二阶段额外增加的接续概率
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack|Phase2")
	float Phase2FollowUpBonus = 0.35f;

	// 大招：额外圈数
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack|Phase2")
	int32 Phase2ExtraRings = 1;

	// ---- 判定窗口（占蒙太奇时长的比例）----
	// 为什么不挂在蒙太奇通知上：Boss 的蒙太奇是脚本按源动画生成的，逐个手拖通知既麻烦又容易漏；
	// 而"挥砍能砍到人的那一段"本质就是时间的函数，用比例驱动既精确又能统一调参。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack|Window")
	float HitWindowStartRatio = 0.28f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack|Window")
	float HitWindowEndRatio = 0.52f;

	// 弹幕在蒙太奇的哪个比例齐射（0.45 ≈ 抬手的最高点）
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack|Window")
	float BarrageFireRatio = 0.45f;

	// 出招期间是否钉住自身移动（原地施法用；突进斩不要开，它靠根运动任务推出去）
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Attack")
	bool bRootSelfDuringAttack = false;

	// ==================== 二阶段生效后的实际参数 ====================
	// 技能与 Brain 统一从这里取值，保证"同一个招式、同一个数字"，不会两处各算一遍算岔。
	// bPhase2 = false 时全部退化为原始值，所以一阶段完全不受影响。
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Attack")
	float GetEffectiveDamage(bool bPhase2) const
	{
		return (bPhase2 && bUpgradeInPhase2) ? Damage * Phase2DamageScale : Damage;
	}

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Attack")
	float GetEffectivePoiseDamage(bool bPhase2) const
	{
		return (bPhase2 && bUpgradeInPhase2) ? PoiseDamage * Phase2DamageScale : PoiseDamage;
	}

	// 蒙太奇播放速率（>1 = 动作更快、留给玩家的反应时间更短）
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Attack")
	float GetEffectivePlayRate(bool bPhase2) const
	{
		return (bPhase2 && bUpgradeInPhase2) ? PlayRate * Phase2SpeedScale : PlayRate;
	}

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Attack")
	float GetEffectiveTelegraphTime(bool bPhase2) const
	{
		return (bPhase2 && bUpgradeInPhase2) ? TelegraphTime * Phase2TelegraphScale : TelegraphTime;
	}

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Attack")
	float GetEffectiveRecoverTime(bool bPhase2) const
	{
		return (bPhase2 && bUpgradeInPhase2) ? RecoverTime * Phase2RecoverScale : RecoverTime;
	}

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Attack")
	float GetEffectiveCooldown(bool bPhase2) const
	{
		return (bPhase2 && bUpgradeInPhase2) ? Cooldown * Phase2CooldownScale : Cooldown;
	}

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Attack")
	float GetEffectiveFollowUpChance(bool bPhase2) const
	{
		return (bPhase2 && bUpgradeInPhase2)
			? FMath::Min(FollowUpChance + Phase2FollowUpBonus, 1.f)
			: FollowUpChance;
	}

	// 弹幕：二阶段多加几发（扇形变宽、环形变密都靠它）
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Attack")
	int32 GetEffectiveProjectileCount(bool bPhase2) const
	{
		return (bPhase2 && bUpgradeInPhase2)
			? Barrage.ProjectileCount + Phase2ExtraProjectiles
			: Barrage.ProjectileCount;
	}

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Attack")
	float GetEffectiveProjectileSpeedScale(bool bPhase2) const
	{
		return (bPhase2 && bUpgradeInPhase2) ? Phase2ProjectileSpeedScale : 1.f;
	}

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Attack")
	float GetEffectiveChargeDistance(bool bPhase2) const
	{
		return (bPhase2 && bUpgradeInPhase2) ? Charge.ChargeDistance * Phase2ChargeDistanceScale : Charge.ChargeDistance;
	}

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Attack")
	float GetEffectiveImpactRadius(bool bPhase2) const
	{
		return (bPhase2 && bUpgradeInPhase2) ? Charge.ImpactRadius * Phase2ImpactRadiusScale : Charge.ImpactRadius;
	}

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Attack")
	int32 GetEffectiveRingCount(bool bPhase2) const
	{
		return (bPhase2 && bUpgradeInPhase2) ? RingCount + Phase2ExtraRings : RingCount;
	}

	// ---- 便捷判定 ----
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Attack")
	bool UsesProjectiles() const
	{
		return AttackType == EBossAttackType::RangedBarrage || AttackType == EBossAttackType::Ultimate;
	}

	// 距离是否落在这招的"舒适区"里（Brain 选招用）
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Attack")
	bool IsInRange(float Distance) const
	{
		return bIgnoreRangeCheck || FMath::Abs(Distance - IdealRange) <= RangeTolerance;
	}
};

// ------------------------------------------------------------------ 一个阶段
UCLASS(BlueprintType)
class DEMO_API UBossPhaseData : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Phase")
	FText PhaseName;

	// 进入该阶段的血量比例（从 1.0 往下匹配，按顺序填 1.0 / 0.6）
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Phase")
	float HealthRatioThreshold = 1.f;

	// 该阶段的招式池
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Phase")
	TArray<TObjectPtr<UBossAttackData>> Attacks;

	// ---- 阶段转换演出（二阶段狂化用）----
	// 进入本阶段时播放的演出蒙太奇（例如 In-Place/Buff_In + Buff_Loop）
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Phase|Enter")
	TObjectPtr<UAnimMontage> EnterMontage;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Phase|Enter")
	TObjectPtr<UAnimMontage> EnterLoopMontage;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Phase|Enter")
	TObjectPtr<UAnimMontage> EnterOutMontage;

	// 演出时长（秒）。这段时间 BOSS 不出招，玩家可以白打 —— 换阶段的安全感来源
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Phase|Enter")
	float EnterDuration = 2.5f;

	// 演出期间是否无敌（默认否：给玩家输出窗口；想更硬就打开）
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Phase|Enter")
	bool bInvincibleDuringEnter = false;

	// 进入本阶段时施加给自己的增益（狂化：加攻 / 加速 / 加韧性）
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Phase|Enter")
	TSubclassOf<UGameplayEffect> EnrageEffect;

	// ---- 节奏 ----
	// 两次出招之间的间隔范围
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Phase")
	float AttackIntervalMin = 1.2f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Phase")
	float AttackIntervalMax = 2.5f;

	// ---- 固定背板出招 ----
	// 非空 = 走【确定性序列】：按这里的顺序循环出招，玩家可以背板记招、练出肌肉记忆。
	// 空   = 退回原来的加权随机（两套逻辑并存，改行为只需要清空/填入这个数组）。
	// 填的是招式的 AttackName（就是上面 Attacks 里那些招的名字）。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Phase|Pattern")
	TArray<FName> AttackPattern;

	// 背板里轮到的那招当前不可用（冷却没到 / 距离不合适）时，最多往后跳几招去找可用的。
	// 跳过的那几招会在下一轮循环里回来，所以背板顺序不会被"跳"破坏。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Phase|Pattern")
	int32 PatternSkipLimit = 3;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Phase")
	float MoveSpeed = 380.f;

	// 小于 1 表示起手更快（二阶段压迫感）
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Phase")
	float TelegraphTimeScale = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Phase")
	float DamageScale = 1.f;

	// 该阶段的移速倍率（狂化后跑得更快）
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Phase")
	float MoveSpeedScale = 1.f;

	// ---- 大招 ----
	// 本阶段的大招。留空 = 本阶段不放
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Phase|Ultimate")
	TObjectPtr<UBossAttackData> UltimateAttack;

	// 大招冷却（秒）：放完一次要等这么久才能再放
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Phase|Ultimate")
	float UltimateCooldown = 22.f;

	// 进入本阶段后，至少等这么久才允许放大招（别一进二阶段就丢大招）
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Phase|Ultimate")
	float UltimateInitialDelay = 6.f;

	// 血量低于该比例才解锁大招（1 = 不限制）
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Phase|Ultimate")
	float UltimateHealthRatioGate = 1.f;

	// 本阶段最多放几次大招（0 = 不限）
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Phase|Ultimate")
	int32 UltimateMaxCount = 0;

	// 血量低于该比例时强制放大招（>0 生效）。用来制造“濒死爆发”的戏剧点
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Phase|Ultimate")
	float UltimateDesperateHealthRatio = 0.f;

	// ---- 走位 ----
	// 距离小于此值时 AI 会主动后撤（弹幕型 BOSS 靠它拉开距离）
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Phase|Movement")
	float BackoffRange = 0.f;

	// 每次后撤走多远
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Boss|Phase|Movement")
	float BackoffDistance = 400.f;
};
