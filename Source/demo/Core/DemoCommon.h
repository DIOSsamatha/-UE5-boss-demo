// 全模块公共头文件：日志分类、属性访问宏、通用枚举
#pragma once

#include "CoreMinimal.h"
#include "AttributeSet.h"
#include "AbilitySystemComponent.h"
#include "GameplayEffectTypes.h"

#ifndef DEMO_API
#define DEMO_API
#endif

// UE 5.7 不再提供全局 ATTRIBUTE_ACCESSORS，这里按引擎推荐的组合方式补回
#define ATTRIBUTE_ACCESSORS(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_GETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_SETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_INITTER(PropertyName)

DECLARE_LOG_CATEGORY_EXTERN(LogDemoCombat, Log, All);

// ------------------------------------------------------------------ 阵营
// 敌我判定的唯一依据。数值本身有意义，别随意改动：
//   0 = 玩家方（玩家、召唤物、友军 NPC）
//   1 = 敌对方（BOSS、小怪）
//   2 = 中立（木桩、可破坏物：谁都能打，但它不还手）
// 约定：TeamId 相同 = 友军（默认不结算伤害）；2 号中立对任何人都“可被攻击但不主动敌对”。
UENUM(BlueprintType)
enum class EDemoTeam : uint8
{
	Player	UMETA(DisplayName = "玩家方"),
	Enemy	UMETA(DisplayName = "敌对方"),
	Neutral	UMETA(DisplayName = "中立")
};

// ------------------------------------------------------------------ 伤害来源
// 只用于「表现层」：飘字颜色/大小、受击音效、顿帧强度。
// 不参与数值计算，所以加一个新种类不会影响战斗平衡。
UENUM(BlueprintType)
enum class EDemoDamageSource : uint8
{
	Unknown		UMETA(DisplayName = "未知"),
	Melee		UMETA(DisplayName = "近战挥砍"),
	Projectile	UMETA(DisplayName = "远程弹幕"),
	// 大范围的地面/冲击类伤害（突进斩的收尾冲击、震荡波）。
	// 原来叫 LeapImpact —— 跳砸招式已经换成突进斩了，名字跟着改过来，
	// 免得以后有人照着名字去找一个不存在的招式。
	HeavyImpact	UMETA(DisplayName = "重击冲击"),
	Ultimate	UMETA(DisplayName = "大招"),
	Environment	UMETA(DisplayName = "环境 / 陷阱")
};

// 受击方向，用于挑选对应的受击动画
UENUM(BlueprintType)
enum class EDemoHitDirection : uint8
{
	Front	UMETA(DisplayName = "正面"),
	Back	UMETA(DisplayName = "背面"),
	Left	UMETA(DisplayName = "左侧"),
	Right	UMETA(DisplayName = "右侧")
};

// 一次命中的结果类型，HUD / 特效 / 音效都靠它区分表现
UENUM(BlueprintType)
enum class EDemoHitResultType : uint8
{
	Normal		UMETA(DisplayName = "普通命中"),
	Blocked		UMETA(DisplayName = "被格挡"),
	Parried		UMETA(DisplayName = "被弹反"),
	Dodged		UMETA(DisplayName = "被闪避"),
	Critical	UMETA(DisplayName = "暴击")
};

// ------------------------------------------------------------------ 枚举 → 中文名
// 放在头文件里给所有模块共用，免得日志和 UI 各写一份 switch 然后慢慢走岔。
inline const TCHAR* DemoTeamToText(EDemoTeam Team)
{
	switch (Team)
	{
	case EDemoTeam::Player:		return TEXT("玩家方");
	case EDemoTeam::Enemy:		return TEXT("敌对方");
	case EDemoTeam::Neutral:	return TEXT("中立");
	default:					return TEXT("未知");
	}
}

inline const TCHAR* DemoDamageSourceToText(EDemoDamageSource Source)
{
	switch (Source)
	{
	case EDemoDamageSource::Melee:			return TEXT("近战");
	case EDemoDamageSource::Projectile:		return TEXT("弹幕");
	case EDemoDamageSource::HeavyImpact:	return TEXT("重击冲击");
	case EDemoDamageSource::Ultimate:		return TEXT("大招");
	case EDemoDamageSource::Environment:	return TEXT("环境");
	default:								return TEXT("未知");
	}
}

inline const TCHAR* DemoHitResultToText(EDemoHitResultType Result)
{
	switch (Result)
	{
	case EDemoHitResultType::Normal:	return TEXT("命中");
	case EDemoHitResultType::Blocked:	return TEXT("被格挡");
	case EDemoHitResultType::Parried:	return TEXT("被弹反");
	case EDemoHitResultType::Dodged:	return TEXT("被闪避");
	case EDemoHitResultType::Critical:	return TEXT("破防暴击");
	default:							return TEXT("未知");
	}
}
