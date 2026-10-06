// 全局 GameplayTag 定义（原生注册，无需在编辑器里手动添加）
#pragma once

#include "CoreMinimal.h"
#include "NativeGameplayTags.h"
#include "DemoCommon.h"

namespace DemoTag
{
	// ---------- 技能 ----------
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Attack_Normal);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Attack_Skill);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Defense_Block);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Defense_Parry);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Movement_Dodge);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Movement_Jump);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Boss_Attack);

	// ---------- 道具（回血"酒"）----------
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_UseItem_Heal);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Cooldown_UseItem_Heal);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Heal);

	// ---------- 角色状态 ----------
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Blocking);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Parrying);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Invincible);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Attacking);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Stagger);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Dead);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Broken);

	// 失衡：异常条（冲击槽）打满的中间态。
	// 与 State_Broken 的区别（对标参考 Demo 的 UNSTABLE / BROKEN 两级）：
	//   UNSTABLE = 还能行动，但绿色轮廓 + 受伤加成 → "再压两刀就能击破"的可见进度
	//   BROKEN   = 击破：失去格挡 + 进虚弱窗口 → 明确的高额输出窗口
	// 只挂 tag 不改血量，是为了让 UI 和材质两条通道都能直接读它（状态信号规范的双通道）。
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Unstable);

	// ---------- 完美判定窗（"绝区零手感"的核心）----------
	// 这两个 tag 只存在极短时间（闪避 0.22s / 弹刀 0.12s），
	// ApplyHit 结算时【优先】检查它们：
	//   有它 = 玩家在正确时机按下了键 → 完美判定（强顿帧 + 子弹时间 + 额外奖励）
	//   没它但仍有无敌 / 招架状态 = 普通判定（仍然免伤 / 弹开，但表现弱一档）
	//
	// 为什么不用"时间戳差值"来判断：ApplyHit 是纯结算函数，
	// 让它去问技能"你刚才什么时候按的键"会把耦合搞得很脏；
	// 用 GE 挂 tag 则是声明式的 —— 谁开的窗、开多久，都在 Effect 里一眼看得见。
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_PerfectDodgeWindow);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_PerfectParryWindow);
	// 完美闪避派生窗：完美闪避后短暂存在，期间按攻击可取消闪避后摇直接接普攻/技能（"闪A"）
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_DodgePerfect);

	// 体力偏低：玩家普攻只能打前两段（第 3/4 段被锁），逼玩家收手而不是无脑连点
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_StaminaLow);
	// 体力耗尽：无法弹反、普攻伤害打折，但【不被硬控】——仍可自由走位（软惩罚）
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_StaminaExhausted);

	// 玩家受击僵直：被打中后短暂无法出招（普攻/技能被封禁并中断），但【保留闪避】，
	// 所以不会变成"被连击锁死"——玩家永远有翻滚脱身这一条路。
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Hitstun);

	// BOSS 二阶段标记：动画蓝图 / 材质 / 音效可以据此切表现
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Boss_Phase2);
	// BOSS 大招蓄力中：HUD 可以据此打提示，玩家知道该跑位了
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Boss_Charging);

	// ---------- 负面状态效果（后续扩展直接在这里加） ----------
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Debuff_Burn);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Debuff_Poison);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Debuff_Bleed);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Debuff_Vulnerable);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Debuff_Slow);

	// ---------- 增益状态效果 ----------
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Buff_AttackUp);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Buff_DefenseUp);

	// ---------- 事件（用于 Ability 之间 / 与角色通信） ----------
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Hit_Dealt);
	// 普通弹反（判定窗后半段弹到）：免伤 + 攻击方短硬直
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Parry_Success);
	// 完美弹反（判定窗前 0.12s 内弹到）：额外削韧 + 攻击方长硬直 + 玩家回耐力
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Parry_Perfect);
	// 完美闪避（无敌帧前 0.22s 内被命中）：子弹时间 + 清空闪避冷却
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Dodge_Perfect);
	// 普通闪避（无敌帧内但不在完美窗）：只免伤，不给子弹时间
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Dodge_Normal);
	// 拼刀（双方同一窗口内都在挥刀）：稳心 —— 取消硬直 + 回架势
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Clash);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Boss_PhaseChanged);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Boss_UltimateStart);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Boss_Landed);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Ability_ComboInput);
	// 连弹：右键在"弹刀技能还在跑"的时候再按一次。
	//
	// 为什么需要它：技能的实例化策略是 InstancedPerActor，已经激活的技能再
	// TryActivateAbility 会直接失败 —— 于是弹刀成功后的收势期间按第二下右键会石沉大海，
	// BOSS 的三段连击只能弹掉第一段，剩下的全吃。
	// 现在由控制器在"激活失败"时补发这个事件，技能收到就把判定窗重新推开一次。
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Ability_ParryRewindow);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Montage_ComboBegin);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Montage_ComboEnd);

	// ---------- SetByCaller 数据键 ----------
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Damage);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_PoiseDamage);
	// 双异常条积累量（UGE_AnomalyGainEffect 的两个 SetByCaller 通道）
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_AnomalyImpact);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_AnomalyElement);
	// 架势条变化量（UGE_PostureChangeEffect 的 SetByCaller 通道；负数=扣，正数=回）
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Posture);
	// 体力消耗量（UGE_StaminaCostEffect 的 SetByCaller 通道；传正数=扣多少）
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_StaminaCost);

	// ---------- 冷却 ----------
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Cooldown_Attack_Skill);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Cooldown_Dodge);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Cooldown_Parry);
}
