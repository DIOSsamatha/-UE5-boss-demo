#include "Core/DemoGameplayTags.h"

// 全模块统一的日志分类（DemoCommon.h 里 DECLARE_LOG_CATEGORY_EXTERN，这里给出唯一的定义）
DEFINE_LOG_CATEGORY(LogDemoCombat);

namespace DemoTag
{
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_Attack_Normal,	"Ability.Attack.Normal",	"普通攻击（连段）");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_Attack_Skill,	"Ability.Attack.Skill",		"技能攻击");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_Defense_Block,	"Ability.Defense.Block",	"格挡（按住）");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_Defense_Parry,	"Ability.Defense.Parry",	"弹反（瞬时窗口）");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_Movement_Dodge,	"Ability.Movement.Dodge",	"闪避");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_Movement_Jump,	"Ability.Movement.Jump",	"跳跃");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_Boss_Attack,		"Ability.Boss.Attack",		"BOSS 招式");

	// ---------- 道具（回血"酒"）----------
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_UseItem_Heal,	"Ability.UseItem.Heal",		"使用道具：喝酒回血");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Cooldown_UseItem_Heal,	"Cooldown.UseItem.Heal",	"喝酒回血冷却");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Data_Heal,				"Data.Heal",				"SetByCaller：回血数值");

	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Blocking,	"State.Blocking",	"正在格挡");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Parrying,	"State.Parrying",	"弹反窗口开启中");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Invincible,	"State.Invincible",	"无敌帧");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Attacking,	"State.Attacking",	"正在出招");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Stagger,	"State.Stagger",	"硬直中");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Dead,		"State.Dead",		"已死亡");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Broken,	"State.Broken",		"破防（韧性清零）");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Unstable,	"State.Unstable",	"失衡（异常条打满：轮廓变绿、受伤加成）");

	// 完美判定窗：只在按键后的极短时间内存在，ApplyHit 靠它区分"完美 / 普通"
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_PerfectDodgeWindow,	"State.PerfectDodgeWindow",	"完美闪避判定窗（闪避开场 0.22s）");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_PerfectParryWindow,	"State.PerfectParryWindow",	"完美弹反判定窗（弹刀开场 0.12s）");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_DodgePerfect,	"State.DodgePerfect",	"完美闪避派生窗（窗内按攻击取消闪避后摇接招）");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_StaminaLow,		"State.StaminaLow",		"体力偏低：普攻只能打前两段（第3/4段锁定）");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_StaminaExhausted,	"State.StaminaExhausted",	"体力耗尽：无法弹反、普攻伤害打折，但可自由走位（软惩罚）");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Hitstun,		"State.Hitstun",		"玩家受击僵直：短暂无法出招，但可闪避脱身");

	// BOSS 专用状态（头文件里声明了就必须在这里定义，否则链接期 LNK2001）
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Boss_Phase2,	"State.Boss.Phase2",	"BOSS 已进入二阶段");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Boss_Charging,	"State.Boss.Charging",	"BOSS 大招蓄力中");

	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Debuff_Burn,			"State.Debuff.Burn",		"燃烧：持续掉血");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Debuff_Poison,		"State.Debuff.Poison",		"中毒：持续掉血 + 削韧");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Debuff_Bleed,		"State.Debuff.Bleed",		"流血：受击额外伤害");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Debuff_Vulnerable,	"State.Debuff.Vulnerable",	"易伤：受到伤害提升");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Debuff_Slow,			"State.Debuff.Slow",		"减速");

	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Buff_AttackUp,	"State.Buff.AttackUp",	"攻击提升");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Buff_DefenseUp,	"State.Buff.DefenseUp",	"防御提升");

	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Event_Hit_Dealt,			"Event.Hit.Dealt",			"命中结算事件");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Event_Parry_Success,		"Event.Parry.Success",		"普通弹反成功（判定窗后半段弹到）");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Event_Parry_Perfect,		"Event.Parry.Perfect",		"完美弹反成功（判定窗前 0.12s 内弹到）");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Event_Dodge_Perfect,		"Event.Dodge.Perfect",		"完美闪避（无敌帧前段被命中）");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Event_Dodge_Normal,		"Event.Dodge.Normal",		"普通闪避（无敌帧内但不在完美窗）");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Event_Clash,			"Event.Clash",			"拼刀（双方同窗挥刀 → 稳心：取消硬直 + 回架势）");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Event_Boss_PhaseChanged,	"Event.Boss.PhaseChanged",	"BOSS 阶段切换");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Event_Boss_UltimateStart,	"Event.Boss.UltimateStart",	"BOSS 大招开始（HUD 提示用）");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Event_Boss_Landed,			"Event.Boss.Landed",			"BOSS 物理落地（交给蓝图做扬尘/震屏）");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Event_Ability_ComboInput,"Event.Ability.ComboInput",	"连段输入（在窗口内按下攻击键）");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Event_Ability_ParryRewindow,"Event.Ability.ParryRewindow","连弹：技能已激活时再按右键，重开弹刀判定窗");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Event_Montage_ComboBegin,	"Event.Montage.ComboBegin",	"连段窗口开启");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Event_Montage_ComboEnd,		"Event.Montage.ComboEnd",	"连段窗口关闭");

	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Data_Damage,			"Data.Damage",		"SetByCaller：伤害数值");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Data_PoiseDamage,	"Data.PoiseDamage",	"SetByCaller：削韧数值");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Data_AnomalyImpact,	"Data.AnomalyImpact",	"SetByCaller：冲击异常积累（橙条）");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Data_AnomalyElement,	"Data.AnomalyElement",	"SetByCaller：元素异常积累（青条）");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Data_Posture,		"Data.Posture",		"SetByCaller：架势变化量（负=格挡消耗，正=返还）");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Data_StaminaCost,	"Data.StaminaCost",	"SetByCaller：体力消耗量（传正数=扣多少）");

	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Cooldown_Attack_Skill,	"Cooldown.Attack.Skill",	"技能冷却");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Cooldown_Dodge,			"Cooldown.Movement.Dodge",	"闪避冷却");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Cooldown_Parry,			"Cooldown.Defense.Parry",	"弹反冷却");
}
