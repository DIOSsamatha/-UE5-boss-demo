// 战斗平衡总表 —— 一场 BOSS 战的时长、双方硬度、每一招的伤害都从这里出。
//
// ============================ 为什么要有这个文件 ============================
// 数值原来散在三处（AttributeSet 构造函数、BossCharacterBase 的 Default* 属性、
// BossBrainComponent::EnsureDefaultPhases 的招式参数），改一个要翻三个文件、
// 而且玩家侧压根没有默认属性兜底（血量是 AttributeSet 里的 100）——
// 于是出现"BOSS 一击打玩家 51 点、两下就死"这种事故。
//
// 现在统一在这里定义，.cpp 里引用；改数值只改这一处，推导过程也写在同一处，
// 下次调完能立刻按下面的表复核一遍。
//
// ============================ 目标：战斗 4~6 分钟 ============================
//
// 【一、BOSS 血量 —— 决定"打多久"】
//   玩家侧的有效输出（按普攻 4 段默认伤害 28/34/42/56 算）：
//     一轮 4 段合计伤害 ................. 160
//     一轮耗时（含 ComboSpeedupPerStage=0.12 的连段加速）≈ 2.40s
//     理论 DPS .......................... 160 / 2.40 ≈ 66.7
//     实战要走位、躲弹幕、被打断 → 有效输出占空比取 0.50
//     普攻折算 DPS ...................... 33.3
//     技能攻击（CD 5s / 伤害 120）折合 .... +7.0  ← 若玩家不按技能则只有 33.3
//     ----------------------------------------------------------
//     净 DPS ≈ 40
//   目标时长 250~300s（落在 4~6 分钟区间）
//     → BOSS 血量 ≈ 40 × 250 = 10000
//   再算上破防期 1.5 倍易伤（每 ~18s 有 2.5s）与二阶段玩家更谨慎，
//   实测一般在 4.2 ~ 5.5 分钟之间。想更长/更短就直接等比调 BossMaxHealth。
//
// 【二、玩家能挨几下 —— 决定"难不难"】
//   期望：会闪避/弹刀的玩家能撑满全场；只会站桩乱砍的 1 分钟内被打死。
//   按"全场约挨 30 下"倒推允许的单下伤害：
//     2000 / 30 ≈ 67
//   玩家 DefensePower = 25 → 减伤 25/(25+100) = 20%
//     → BOSS 近战基础伤害需要 ≈ 67 / 0.8 ≈ 84，取 88（一击实伤 70.4）
//   于是"挨 28 下死"，配合闪避/弹刀有充足容错，但完全不防会很快暴毙。
//
// 【三、韧性 —— 决定破防节奏】
//   BOSS：MaxPoise 320，玩家普攻每段削韧 14 → 约 23 段破防一次（≈18s）
//   玩家：MaxPoise 260，BOSS 每下削韧 18~35     → 约 9~14 下破防
//   破防后在 DemoDamageExecution 里享受 1.5 倍伤害（State.Broken 分支）。
//
// 【四、弹幕 —— 为什么要"单发不疼、糊脸才疼"】
//   单发 54，扇形 5 发全中 = 54 × 0.8 × 5 = 216（血量的 10.8%）。
//   单发不致命才让"走位躲弹幕"有意义；站着不动被整片糊到就是重伤。
//
// ============================ 改数值的注意事项 ============================
// · AttackPower 与 DefensePower 有意都设成很小的值：
//   伤害公式是 FinalDamage = (招式基础值 + 攻击方 AttackPower) × (1 - 防御方Def/(Def+100))。
//   把 AttackPower 压到 0、DefensePower 只留一点点，招式表上的数字就等于"实际打出的伤害"，
//   调参时不用心算，一眼就能对上。
// · 这些常量只在 .cpp 里用（不写进 UPROPERTY 默认值）——
//   UHT 对属性默认值表达式的解析有限，引用跨文件常量容易在生成代码里翻车；
//   统一在 BeginPlay 的 EnsureDefaultAttributes() 里赋值最稳。
#pragma once

#include "CoreMinimal.h"

namespace DemoBalance
{
	// ---------------------------------------------------------------- 玩家
	// 血量：约挨 BOSS 近战 28 下
	inline constexpr float PlayerMaxHealth = 2000.f;
	// 耐力：闪避一次 -20 → 约 10 次；格挡一次 -12
	inline constexpr float PlayerMaxStamina = 200.f;
	// 韧性：BOSS 每下削韧 18~35 → 约 9~14 下破防
	inline constexpr float PlayerMaxPoise = 260.f;
	// 玩家的伤害全部写在招式基础值里（普攻 ComboDamages / 技能 Damage），
	// 这里保持 0，让"招式表上的数字 = 实际伤害"
	inline constexpr float PlayerAttackPower = 0.f;
	// 减伤 25/(25+100) = 20%
	inline constexpr float PlayerDefensePower = 25.f;

	// ---- 回血道具"酒" ----
	// 单次回复 30% 最大生命；冷却 1.5s；初始携带 3 瓶、上限 3 瓶。
	// 蒙太奇路径：从 SwordAnimsetPro 的占位序列生成的喝酒动作（见 Tools/SetupHealDrinkMontage.py）。
	inline constexpr float HealPercent = 0.30f;                         // 单次回复比例
	inline constexpr float HealCooldown = 1.5f;                        // 喝酒冷却（秒）
	inline constexpr int32 HealItemMaxCount = 3;                       // 携带上限
	inline constexpr int32 HealItemInitialCount = 3;                   // 开局携带数量
	inline constexpr const TCHAR* HealMontagePath = TEXT("/Game/FightPlayer/Animation/Heal_Drink_Montage.Heal_Drink_Montage");

	// 普攻连段单段伤害（4 段递增）。填在 UGA_PlayerNormalAttack 构造函数里，
	// 保证"没配蓝图的 ComboDamages 也有正确伤害"，不再退回 10 点的兜底值。
	// 合计 160 / 一轮约 2.4s —— 见文件顶部的 DPS 推导。
	inline constexpr float PlayerComboDamage1 = 28.f;
	inline constexpr float PlayerComboDamage2 = 34.f;
	inline constexpr float PlayerComboDamage3 = 42.f;
	inline constexpr float PlayerComboDamage4 = 56.f;

	// 普攻每段削韧：BOSS 320 韧 → 约 23 段破防
	inline constexpr float PlayerComboPoiseDamage = 14.f;

	// 技能攻击（鼠标中键 / IA_AttackSkill）：CD 5s，一击有分量但不至于喧宾夺主
	inline constexpr float PlayerSkillDamage = 120.f;
	inline constexpr float PlayerSkillPoiseDamage = 50.f;
	inline constexpr float PlayerSkillStaminaCost = 25.f;

	// ---------------------------------------------------------------- BOSS
	// 血量：决定整场时长，见文件顶部推导
	inline constexpr float BossMaxHealth = 10000.f;
	inline constexpr float BossMaxStamina = 999.f;
	// 玩家普攻每段削韧 14。
	//
	// ⚠ 320 是"太容易破防"的元凶之一（2026-09-30 上调）：破防是玩家最爽的奖励，
	//   但 320 / 14 ≈ 23 段，一套四段连打 6 轮就能白嫖一次处决窗口，BOSS 全程在挨打。
	//   提到 520 → 约 37 段（≈30s 的持续输出），破防才重新变成"攒出来的"。
	//
	// ⚠ 2026-10-04 再上调 520 → 720：用户反馈"破防阈值再高一点"。
	//   720 / 14 ≈ 51 段普攻，或技能（50 削韧）14 次。
	//   破防是"打空一整条韧性槽"级别的奖励，频率上应该和 BOSS 的阶段切换（约 30~40s）对齐，
	//   而不是每半分钟白送一次。BOSS 的血量是 10000，破防只能靠输出效率换，
	//   阈值太低会连锁成"破防 → 易伤 1.5x → 更快破防"的滚雪球，玩家感觉是在打一个会自己崩的木桩。
	//   ⚠ 这是 constexpr（不是 UPROPERTY），所以不存在"蓝图 CDO 压过 C++ 默认值"的问题，
	//     改完直接生效；但改它要连带检查 BossPoiseBreakWeakDuration 是否还合理。
	inline constexpr float BossMaxPoise = 720.f;
	inline constexpr float BossAttackPower = 0.f;
	// 0 = 玩家伤害不打折，招式表数字即实伤
	inline constexpr float BossDefensePower = 0.f;
	inline constexpr float BossMoveSpeed = 380.f;

	// ---- BOSS 招式基础伤害（一阶段；二阶段按招式上的 Phase2DamageScale 自动强化）----
	// 近战三段递增：第三段是重击，起手更长、伤害明显更高
	inline constexpr float BossSlash1Damage = 88.f;
	inline constexpr float BossSlash2Damage = 108.f;
	inline constexpr float BossSlash3Damage = 160.f;

	// 弹幕单发。扇形 5 发全中 ≈ 216（血量的 10.8%）—— 单发不疼、糊脸才疼
	inline constexpr float BossBarrageDamage = 54.f;

	// 突进斩：三段都有独立伤害 —— 蓄势不算，突进拖刀 + 收尾重斩各算一次。
	// 设计上"拖刀不疼、收尾重"，这样玩家有明确的分段读招：
	// 第一段可以硬吃（换输出），第二段必须躲。
	inline constexpr float BossChargeDamage = 96.f;          // 招式本体（未分段的兜底值）
	inline constexpr float BossChargeDashDamage = 52.f;      // 突进途中拖到
	inline constexpr float BossChargeImpactDamage = 132.f;   // 收尾重斩
	inline constexpr float BossChargeShockwaveDamage = 10.f; // 二阶段收尾的震荡波单发

	// 大招收尾环形弹幕：单发伤害低于普通弹幕（圈数多、覆盖面广，靠量取胜）
	inline constexpr float BossUltimateBarrageDamage = 64.f;

	// ---- BOSS 招式削韧（打玩家的韧性）----
	inline constexpr float BossSlash1Poise = 18.f;
	inline constexpr float BossSlash2Poise = 22.f;
	inline constexpr float BossSlash3Poise = 30.f;
	inline constexpr float BossBarragePoise = 12.f;
	inline constexpr float BossChargePoise = 26.f;
	inline constexpr float BossChargeDashPoise = 14.f;
	inline constexpr float BossChargeImpactPoise = 32.f;
	inline constexpr float BossChargeShockwavePoise = 6.f;
	inline constexpr float BossUltimatePoise = 14.f;

	// ---- 击退硬上限（"别再把玩家打飞 / 推着走"）----
	// 为什么要有"硬上限"：招式表里每一招都能填击退，谁填大了玩家就飞。
	// 与其指望每次改数值都记得收着点，不如在结算处夹一刀 ——
	// 无论招式表怎么填，玩家最多被"震开"一点点，永远不会被掀到空中。
	inline constexpr float BossMaxKnockbackSpeed = 110.f;   // 水平击退速度上限（cm/s）
	inline constexpr float BossMaxKnockbackUpward = 0.f;    // 上抛上限（0 = 永不把玩家打离地面）

	// ---- 突进斩的速度上限 ----
	// 之前"固定 0.28s 冲完"的写法会让速度随距离暴涨：
	// 玩家站在 961cm 外时速度 = 961/0.28 ≈ 3431cm/s（34 米/秒）——
	// BOSS 是一堵墙一样撞过来的，玩家被撞飞 / 被顶走就是这么来的。
	// 现在改成"按速度算时长"：距离越远冲得越久，但速度恒定、肉眼可读。
	inline constexpr float BossChargeSpeed = 1100.f;         // 突进速度（cm/s ≈ 11 米/秒）
	inline constexpr float BossChargeMinDuration = 0.22f;    // 再近也别快过这个（太短会像瞬移）
	inline constexpr float BossChargeMaxDuration = 0.70f;    // 再远也别慢过这个（站太久会失去压迫感）
	inline constexpr float BossChargeMaxDistance = 900.f;    // 单次突进最远距离（超过就分两次走位）

	// ---- 起手预警红光 ----
	// 闪烁频率随预警进度从慢到快：玩家不看预警条也能凭"闪得越来越急"判断还有多久出招。
	inline constexpr float BossTelegraphFlashHzStart = 3.5f;
	inline constexpr float BossTelegraphFlashHzEnd = 10.f;

	// ---- 新增 3 套近战（丰富 BOSS 近战变化；Phase2 起加入池子）----
	// 伤害都落在"玩家 2000 血、挨 28 下死"的基准附近（见本文件顶部推导），
	// 不会因加招而让 BOSS 突然变难。
	//   Slash4 横扫：覆盖面广、伤害中等，逼玩家保持距离别贴脸
	//   Slash5 突进挑斩：贴脸高伤，给玩家压迫；起手短、收招快
	//   Slash6 重砸：慢起手的大硬直窗口、最高伤，是玩家的"读完招上去输出"窗口
	inline constexpr float BossSlash4Damage = 96.f;
	inline constexpr float BossSlash5Damage = 122.f;
	inline constexpr float BossSlash6Damage = 150.f;

	inline constexpr float BossSlash4Poise = 16.f;
	inline constexpr float BossSlash5Poise = 24.f;
	inline constexpr float BossSlash6Poise = 30.f;

	// ---- 重砸三连（Melee_A/B/C_Slow）：「慢、重、好读」的那一类招 ----
	//
	// 为什么还要再补一组"慢招"：原来池子里全是 0.3~0.85s 起手的快招，
	// 玩家应对手段永远是"反应式弹/闪"，读不到"这一招很重、必须处理"的分量。
	// Slow 变体的前摇明显更长（0.80~0.95s）、动作幅度更大 ——
	// 于是招式表第一次有了【节奏差】：快招逼反应，慢招逼判断。
	// 代价是伤害/削韧都要明显更高，否则玩家会发现"慢招可以直接无视"。
	//
	// 素材：Khaimera 的 Melee_A/B/C 各有 Fast / Slow 两个变体，
	//       01~03 用的是标准版、04~06 用的是 Fast 版，这组用剩下的 Slow 版正好补齐三档速度。
	//
	// 取材动画里 Melee_*_Slow 摆手极大（抬起整个上半身再砸下），
	// ⚠ 因此判定窗口要往后挪（0.34~0.62 而不是快招的 0.22~0.50），
	//    否则会出现"刀还没落下来就已经判定完了"。
	inline constexpr float BossHeavy1Damage = 150.f;
	inline constexpr float BossHeavy2Damage = 168.f;
	inline constexpr float BossHeavy3Damage = 200.f;   // 玩家 2000 血的 10%：全场最重的单发近战

	// 削韧也要配得上"重"：这三招是玩家最该弹、也最容易弹的（前摇长）
	inline constexpr float BossHeavy1Poise = 34.f;
	inline constexpr float BossHeavy2Poise = 38.f;
	inline constexpr float BossHeavy3Poise = 44.f;

	// ---- 回旋横扫（Spin_Bwd2Fwd）+ 大幅挥出（E_Ability_Throw_MSA）----
	// 2026-09-30 新增，目的是真正的【招式多样化】而不是"换速度档"：
	// Melee_A / Melee_A_Fast / Melee_A_Slow 本质是同一个挥击的三个速度版本，
	// 玩家眼里 Slash_01、Slash_04、Heavy_01 是同一个动作。Spin 是转体一圈，
	// Throw 是大开大合的挥出，两者和任何直线挥砍的观感都拉开了。
	// 数值定位：Spin 覆盖面广（范围大、伤害中等、前后摇都偏长 → 好读也好罚），
	//           Throw 打中远距（伤害偏低但收招慢，逼玩家别贪刀）。
	inline constexpr float BossSpin1Damage = 105.f;
	inline constexpr float BossSpin2Damage = 130.f;   // 二阶段反向版，更痛
	inline constexpr float BossThrow1Damage = 88.f;

	inline constexpr float BossSpin1Poise = 30.f;
	inline constexpr float BossSpin2Poise = 36.f;
	inline constexpr float BossThrow1Poise = 22.f;

	// ---- 腾空扑击（Melee_Air_Attack + Bound）：中距离的"贴脸答案" ----
	//
	// 补的是招式表里的一个洞：突进斩（ChargeSlash）要从 640cm 外起手，
	// 中距离（400~500cm）时它会嫌远、近战又会嫌近，BOSS 只能干走位。
	// 扑击就填这个带：起跳 → 扑过来 → 落地砸，比突进斩短、但更快、更突然。
	// 和突进斩的区分：突进斩是"长距离冲刺 + 两段判定"，扑击是"中距离一点 + 落地一段"。
	inline constexpr float BossPounceDashDamage = 62.f;      // 扑击途中蹭到（比突进拖刀略高：动作更凶）
	inline constexpr float BossPounceDashPoise = 16.f;
	inline constexpr float BossPounceImpactDamage = 145.f;   // 落地砸（略低于突进斩收尾：距离短、更容易吃到）
	inline constexpr float BossPounceImpactPoise = 34.f;

	// 扑击的位移：比突进斩【短】但【快】—— 短是为了不和突进斩抢距离带，
	// 快是为了让"腾空扑"有别于"贴地冲"（观感上必须是"扑"不是"跑"）。
	inline constexpr float BossPounceChargeDistance = 420.f;
	inline constexpr float BossPounceChargeSpeed = 1250.f;   // 突进斩 1100 → 扑击 1250
	inline constexpr float BossPounceStopDistance = 160.f;
	inline constexpr float BossPounceMaxChargeDistance = 620.f;

	// ---- BOSS 普通受击短硬直（非霸体、非破防时被命中触发）----
	// 时长刻意很短：既让 BOSS"被打会有反应"（僵直一下），又不至于被无限连控。
	// 配合 Brain 的 AttackInterval（僵直结束后还要等出招间隔）实现"僵直 CD 后才攻击"。
	inline constexpr float BossHitstunTime = 0.28f;

	// 玩家受击僵直时长（秒）：被打中后这段时间无法出招（普攻/技能被封禁并中断）。
	// ⚠ 只封进攻，不封闪避/弹反/格挡 —— 玩家永远能翻滚脱身，避免连击锁死。
	// 0.3s 是动作游戏的常规档位：足够让玩家"感觉到被打断了"，
	// 又不会长到让 BOSS 的一套连击变成无法应对的处刑。
	inline constexpr float PlayerHitstunTime = 0.30f;

	// ---------------------------------------------------------------- 手感窗口
	// 完美判定的窗口。这两个值是"绝区零手感"的核心：
	//   玩家按下去之后，只有【窗口内】被命中才算完美；窗口外被命中算普通判定。
	// 太小 = 玩家永远打不出完美，太大会变成无脑连按。
	//
	// 闪避：0.22s 是一段"刚好需要预判、但不是拼手速"的宽容区间
	inline constexpr float PerfectDodgeWindow = 0.22f;
	// 闪避全程无敌时长。比闪避动作略短一点，避免"闪完还站桩无敌"
	inline constexpr float DodgeInvincibleDuration = 0.45f;
	// 完美闪避派生窗：完美闪避后这段时间内按攻击，可取消闪避后摇直接接招（"闪A"）。
	// 比完美判定窗长，给玩家"闪完立刻按攻击"的反应余量；但也不能太长，否则变成无限派生。
	inline constexpr float PerfectDodgeFollowupWindow = 0.45f;

	// 弹反：完美窗放宽到 0.22s（之前 0.15s），容错更友好，但不至于无脑连按。
	inline constexpr float PerfectParryWindow = 0.22f;
	// 弹反总窗口放宽到 0.55s（之前 0.38s）：点一下右键就有约 0.55s 容错，
	// 配合 HUD 的金色起手预警条，普通弹反非常容易打出。
	// ⚠ 必须与 UGA_PlayerParry::WindowDuration 保持一致（见 PlayerAbilities.h）。
	inline constexpr float ParryWindowDuration = 0.55f;

	// 完美闪避触发时的子弹时间：时长 / 时缓倍率
	inline constexpr float PerfectDodgeSlowMoTime = 0.75f;
	inline constexpr float PerfectDodgeSlowMoScale = 0.18f;

	// ---------------------------------------------------------------- 弹刀（只狼式）
	//
	// 【为什么把慢动作砍到几乎没有 —— 这是本轮最重要的手感决策】
	//   只狼的弹刀是"打铁"：叮、叮、叮。每一下都是
	//     一簇火花 + 一声脆响 + 极短的定帧，然后立刻可以按下一刀。
	//   节奏是【连着】的，玩家才有能力去弹连招的第二下、第三下。
	//
	//   而 0.45s / 0.25 倍的子弹时间会把节奏生生截断：
	//   弹中一下 → 世界慢下来半秒 → 玩家失去"下一刀什么时候到"的时间感 →
	//   连弹第二下必失手 → 于是 BOSS 的三段连击永远只能弹掉第一段。
	//
	//   慢动作是"单次高光"，连弹是"连续节奏"，两者互斥。这里选了后者：
	//   顿帧压到 0.07s（撞击的分量还在），只留 0.08s 的 0.5 倍微时缓
	//   当作"弹到了"的确认感 —— 世界基本不减速。
	inline constexpr float PerfectParryHitStop = 0.09f;
	inline constexpr float PerfectParryHitStopScale = 0.015f;   // 冻得更死，但只冻一瞬
	inline constexpr float PerfectParrySlowMoTime = 0.08f;
	inline constexpr float PerfectParrySlowMoScale = 0.50f;

	// 普通弹反（判定窗后半段弹到）：只给顿帧，不给时缓
	inline constexpr float NormalParryHitStop = 0.10f;
	inline constexpr float NormalParryHitStopScale = 0.03f;

	// 弹刀火花：位置在两人之间、弹刀者的胸口高度。
	// 火花是"弹到了"最直观的证据 —— 只狼里玩家主要靠它确认自己弹对了。
	inline constexpr float ParrySparkRadius = 70.f;
	inline constexpr float ParrySparkDuration = 0.20f;

	// 弹刀镜头冲击：必须很轻。轻到玩家感觉得到、但不会影响下一次读招。
	inline constexpr float ParryCameraPunchLength = 10.f;
	inline constexpr float ParryCameraPunchDuration = 0.14f;
	inline constexpr float ParryPerfectCameraPunchLength = 16.f;

	// 弹刀成功后的"收势"时长：这段时间技能还挂着（不会立刻回到待机），
	// 但只要玩家再按右键，判定窗会立刻重开（连弹）。
	// 别调大 —— 值越大，"没按第二下时"越晚才回到待机状态。
	inline constexpr float ParrySuccessLockTime = 0.14f;

	// 弹反成功后攻击方的硬直：完美弹反明显更长，玩家够时间接一套反击
	inline constexpr float PerfectParryStaggerTime = 1.30f;
	inline constexpr float NormalParryStaggerTime = 0.60f;

	// 完美弹反额外削掉攻击方多少韧性（鼓励玩家主动去弹）
	inline constexpr float PerfectParryBonusPoiseDamage = 45.f;

	// 完美闪避 / 完美弹反给玩家的耐力返还（奖励"用对操作"而不是"挨打"）
	inline constexpr float PerfectDodgeStaminaRefund = 20.f;
	inline constexpr float PerfectParryStaminaRefund = 25.f;

	// ---------------------------------------------------------------- 体力：让它真正参与决策
	// 缺口背景：普攻原本【零消耗】、技能 25 消耗只声明未施加 —— 体力实际是装饰品，
	// 玩家永远打不出"体力见底"这个状态，也就无从谈起"要不要收手"的决策。
	// 这里把"每个动作值多少体力"摊开，并配门控阈值：低体力要真的改变"你能做什么"。

	// 普攻连段四段的体力消耗：越往后越贵。配合贪刀惩罚，让"见好就收"成为真实决策
	inline constexpr float PlayerComboStaminaCost[4] = { 8.f, 10.f, 12.f, 16.f };

	// 空弹（举了刃但全程没弹到任何东西）的惩罚：防止玩家无脑长按右键
	inline constexpr float PlayerEmptyParryStaminaCost = 5.f;

	// 门控阈值（占体力上限的比例）
	inline constexpr float StaminaLowRatio = 0.20f;        // <20%：普攻第 3/4 段锁死，只能打 1-2 段
	inline constexpr float StaminaDodgeWeakRatio = 0.15f;  // <15%：闪避降级（无敌帧变短、距离变短）

	// 体力耗尽的【软】惩罚：持续这段时间，期间恢复减半（净 +15/s），攻防都变弱，
	// 但玩家【不被硬控】——仍可自由走位。硬控会让失误变成坐牢，这里只要"变弱"。
	inline constexpr float StaminaExhaustedTime = 2.5f;
	inline constexpr float StaminaExhaustedRegenPerTick = -1.5f;  // 与恢复 GE(+3/0.1s) 抵消 → 净 +15/s
	inline constexpr float StaminaExhaustedDamageScale = 0.6f;    // 耗尽时普攻伤害

	// 低体力时的闪避降级参数（正常无敌帧 0.45s / 距离系数 1.0）
	inline constexpr float DodgeWeakInvincibleTime = 0.28f;
	inline constexpr float DodgeWeakDistanceScale = 0.7f;

	// ---------------------------------------------------------------- 贪刀惩罚
	// 设计原则：连按不该有安全收益。惩罚必须"看得见但不致命" ——
	// 玩家要能感觉到自己贪了，而不是被罚到不敢出手。

	// 连段打满一轮后还继续按（段数绕回第 1 段）→ 进入"贪刀"状态：
	// 后摇延长、伤害打折。这是"见好就收"成为真实决策的关键。
	inline constexpr float ComboOvercommitRecoveryBonus = 0.15f;   // 后摇 +0.15s
	inline constexpr float ComboOvercommitDamageScale = 0.7f;      // 伤害 ×0.7

	// 闪避后这么短的时间内再闪 → 第二次扑短（距离打折）。压掉"疯狂点 Shift"的保底打法。
	inline constexpr float DodgeSpamWindow = 0.2f;
	inline constexpr float DodgeSpamDistanceScale = 0.6f;

	// 误弹重招：对不可被打断的重型招式（突进斩 / 大招）用普通弹反去接 → 硬直 + 扣体力。
	// 与"完美弹反重型招式只免伤不打断"配套：读错招就要付代价，逼玩家改走闪避。
	inline constexpr float MisParryHeavyStunTime = 0.4f;
	inline constexpr float MisParryHeavyStaminaCost = 15.f;

	// ---------------------------------------------------------------- 打击感：镜头反馈
	// 这些只服务于"手感"，不参与数值结算，所以单独放在这里方便集中调。
	//
	// 受击 FOV 脉冲：BOSS 突进 / 收尾重击（HeavyImpact）命中玩家瞬间，短暂把 FOV 扩开再弹回，
	// 制造"被砸脸"的镜头冲击（Spec 要求 +3~5°、0.1s 回弹）。普通近战不扩，避免每次挨打都晃。
	inline constexpr float BossHeavyHitFOVPulse = 4.f;          // 扩开度数（+4°）
	inline constexpr float BossHeavyHitFOVPulseDuration = 0.12f;

	// 命中旋转微震（玩家砍中 BOSS）：强度随伤害缩放，给"砸实了"的镜头回馈。
	//   轻击(28) ≈ 1.4°、重击(132) 触顶 3.0°。夹住上下限，重击不会甩太凶。
	inline constexpr float HitImpactShakeDamageScale = 0.05f;   // 每点伤害换算的震动度数
	inline constexpr float HitImpactShakeMinDeg = 0.6f;         // 最小震动（轻击也有反馈）
	inline constexpr float HitImpactShakeMaxDeg = 3.0f;         // 最大震动（封顶）

	// ---- 玩家砍中敌人的三通道反馈（"有没有手感"几乎全靠这一组）----
	//
	// 历史坑：以前命中只给 6cm / 0.07s 的【臂长冲击】，而臂长冲击在锁定时被屏蔽
	// （锁定要求视角绝对稳定，用户明确提过）—— 而打 BOSS 时玩家基本全程锁定，
	// 于是锁定状态下砍中 BOSS 几乎没有任何镜头反馈，表现就是"血在掉但像砍在棉花上"。
	//
	// 现在拆成三条通道，按"锁定是否安全"分工：
	//   · 臂长冲击 ApplyCameraPunch   —— 只改弹簧臂，非锁定时才生效（保留原策略）
	//   · FOV 脉冲 PulseCameraFOV     —— 只改视野宽度，不碰臂长也不碰朝向 → 锁定安全
	//   · 旋转微震 PlayHitImpactShake —— 只改相机的相对旋转，自衰减归零 → 锁定安全
	// 锁定时靠后两条给出"砸实了"的分量，非锁定时三条叠加。
	inline constexpr float PlayerHitPunchLength = 12.f;         // 普通命中：拉近臂长
	inline constexpr float PlayerHitPunchDuration = 0.12f;
	inline constexpr float PlayerHitCritPunchLength = 26.f;     // 破防/暴击：明显更重的一击
	inline constexpr float PlayerHitCritPunchDuration = 0.24f;

	inline constexpr float PlayerHitFOVPulse = 2.5f;            // 普通命中扩 FOV（度）
	inline constexpr float PlayerHitCritFOVPulse = 5.f;         // 破防/暴击
	inline constexpr float PlayerHitFOVPulseDuration = 0.11f;

	inline constexpr float PlayerHitShakeDuration = 0.09f;      // 微震时长（强度仍按伤害缩放）
	inline constexpr float PlayerHitCritShakeDuration = 0.15f;

	// 被格挡：手感要"闷"—— 短、钝、没有回响，和"砍进肉里"形成对比，
	// 玩家才能【靠手感】分辨"这一下到底吃进去没有"，从而决定要不要继续压。
	inline constexpr float PlayerHitBlockedPunchLength = 7.f;
	inline constexpr float PlayerHitBlockedPunchDuration = 0.09f;
	inline constexpr float PlayerHitBlockedShakeDuration = 0.05f;

	// ---- 打击感参数包（2026-10-01 定案，对标"顿帧+输入缓冲+特效+震屏+硬直"五件套）----
	//
	// ① 输入缓冲过期：连段窗口未开时提前按下的左键会被缓冲，但不能无限期有效 ——
	//    否则"这一段刚起手时随手点的那一下"会在 1 秒后窗口打开时突然自动接出下一段，
	//    玩家根本不知道自己为什么出刀。0.45s = "我刚才是想接段的"的记忆长度，
	//    比人类的"预输入"节奏（约 2~3 帧反应 + 出手意念）略宽，又不至于把误触存太久。
	inline constexpr float ComboBufferExpiryTime = 0.45f;

	// ② Screen Shake（真·镜头震动，走 PlayerCameraManager 的 ULegacyCameraShake）：
	//    和既有三通道（臂长冲击 / FOV 脉冲 / 旋转微震）叠加 —— 那三道是"镜头被推了一下"，
	//    这道是"整个世界在手里震了一下"，四道一起构成完整的命中冲击。
	//    强度用 Scale（0~1+）传给 UDemoHitCameraShake，波形参数写在它的构造函数里。
	inline constexpr float HitShakeNormalIntensity = 0.55f;    // 普通命中：可感知但不吵
	inline constexpr float HitShakeCritIntensity = 1.0f;       // 破防/暴击：满额
	inline constexpr float HitShakeBlockedIntensity = 0.30f;   // 被格挡：更"闷"的一下
	//    伤害附加：越重的刀震得越狠，给连段一个"逐段变沉"的渐进。
	//    普攻四段 28/34/42/56 → 附加 0.11/0.14/0.17/0.22，第四段正好接近 +0.25 封顶。
	inline constexpr float HitShakeDamagePerPoint = 0.004f;
	inline constexpr float HitShakeDamageBonusMax = 0.25f;
	//    震动总时长（写进 UDemoHitCameraShake 的波形）：0.18s 比单帧长、比一个攻击节奏短，
	//    连段每刀都震但不叠加成持续眩晕。
	inline constexpr float HitShakeDuration = 0.18f;

	// ③ 攻速与位移（既有链定版，集中列出便于整体把握）：
	//    · 攻速：段基础 PlayRate 1.0，逐段 +0.12 → 1.00/1.12/1.24/1.36，
	//      连点时连段肉眼可见地越打越快；命中/连段窗口全部用 EffectiveLen(=Len/PlayRate) 计时，
	//      加速不会让窗口漂到动画外。
	//    · 连段节奏：窗口在每段 20% 处开放、0.5s 没接段就回退第 1 段、收招 70% 处归还移动权。
	//    · 位移：AttackRootMotionScale 保持 1.0 —— 位移必须与动画严格一致，
	//      缩放它会让"脚的动作"和"身体位移"对不上（和 BOSS 滑步同一个道理）。
	//      真要收敛前冲距离，改 Tools/SetupAttackComboMontages.py 里源动画的根位移，别缩放运行时。

	// 普通命中顿帧（玩家砍中 BOSS / 玩家被普通攻击命中）：全局时间膨胀，制造"砸实了"的定帧。
	// 之前 ApplyHit 里写死 0.08s@0.02（50× 慢放几乎无感），所以"砍中了但没感觉"。
	// 改 0.09s@0.15：连段里能读出"每一刀都卡一下"，又不至于把节奏拖死。
	// 子系统自带"已有更长冻结不覆盖"守卫，连段不会把冻结无限叠加，而是保持一道稳定的定帧。
	inline constexpr float NormalHitHitStop = 0.10f;
	inline constexpr float NormalHitHitStopScale = 0.12f;
	// 易伤(破防)/暴击命中的顿帧更重一点：
	// 破防是玩家打出来的高光时刻，必须"世界停一下"，否则和普攻第 1 段没区别。
	inline constexpr float CriticalHitHitStop = 0.16f;
	inline constexpr float CriticalHitHitStopScale = 0.07f;

	// ================================================================ 双异常条（反馈链核心）
	// 对标参考 Demo：Boss 血条下方两条独立积累槽（橙=冲击 / 青=元素）。
	// 两条槽的"容量"刻意做成一样（100），因为玩家要比较的是"我这套连招推进得快不快"，
	// 而不是"哪条更长"。真正区分两条的是下面的积累效率表。
	inline constexpr float AnomalyImpactMax = 100.f;
	inline constexpr float AnomalyElementMax = 100.f;

	// ---- 积累效率：不同手段喂养不同槽 ----
	// 设计意图：
	//   普攻连段 → 冲击槽（纯输出就能推进，保证"一直砍"有收益）
	//   弹反     → 元素槽（奖励"读招应对"，而且是单次高量，鼓励主动去弹）
	//   技能     → 元素槽为主（长 CD 换高积累，形成"攒一波放大招"的节奏）
	// 于是"只砍不弹"和"只弹不砍"都推不动第二条，玩家必须两条手段都用 —— 这就是耦合。
	inline constexpr float ComboAnomalyImpact[4] = { 6.f, 7.f, 8.f, 12.f };  // 普攻 1~4 段
	inline constexpr float ComboAnomalyElement[4] = { 0.f, 0.f, 3.f, 5.f };  // 连段后段才带一点元素
	inline constexpr float SkillAnomalyElement = 18.f;                       // 技能
	inline constexpr float SkillAnomalyImpact = 10.f;
	inline constexpr float ParryAnomalyElement = 22.f;                       // 普通弹反
	inline constexpr float PerfectParryAnomalyElement = 30.f;                // 完美弹反（窗口前段）

	// ================================================================ 架势条（玩家）
	// 参考 Demo："架势值过低时格挡会被 Boss 破防，进入硬直且受到部分伤害"。
	// 它解决的是防御手段在反馈链里"零产出"的问题：
	//   没有架势条 → 举盾是免费的，最优解永远是按住不放，弹反和闪避就没人用；
	//   有了架势条 → 挡是"借"，必须靠完美闪避 / 完美弹反（稳心）还回去，
	//   于是【挡 / 弹 / 闪】三种防御手段第一次有了相互约束的关系。
	inline constexpr float PostureMax = 100.f;
	// BOSS 的架势容量单独给：它是主要被攻击方，容量和玩家一样的话，
	// 玩家站桩猛攻 ~7 刀就能把它的架势打空（每刀约扣 15），BOSS 会显得太脆。
	// ⚠ 180 是"太容易破防"的真正元凶（2026-09-30 上调）：
	//   格挡一次扣 = 8 + 0.25×伤害，玩家普攻伤害 108 → 单次约扣 35，
	//   180 / 35 ≈ **5 次格挡**就把 BOSS 架势打空 → 破防跟喝水一样。
	//   提到 460 → 约 13 次格挡才破一次，BOSS 才有"顶得住"的分量，
	//   破防也重新变成需要玩家刻意去攒（用重击 / 技能而不是无脑平砍）的奖励。
	inline constexpr float BossPostureMax = 460.f;

	// ---- BOSS 默认格挡（P1-4）----
	// BOSS 连续挡下玩家 N 次后强制还手一次（RequestRevengeAttack）。
	// 3 次 = 玩家砍三轮就被怼回来，逼他停止"站桩输出"去找真正的窗口；
	// 太小会变成"砍两刀就被打断"的挫败感，太大则格挡形同虚设。
	inline constexpr int32 BossCounterAfterBlocks = 3;

	// 格挡消耗：基础值 + 按伤害 scaling。
	// 为什么要带伤害项：只扣固定值的话，挨小招和挨大招的代价一样，
	// 玩家学不到"这一下值不值得挡" —— 带 scaling 才能把"重招别硬挡"教出来。
	inline constexpr float BlockPostureCostBase = 8.f;
	inline constexpr float BlockPostureCostPerDamage = 0.25f;

	// 返还（"还债"）：完美闪避给得最多，因为它的窗口最窄、风险最高
	inline constexpr float PerfectDodgePostureRefund = 25.f;
	// 完美弹反 = 稳心：挡回去的那一刻心是稳的，架势回一截
	inline constexpr float PerfectParryPostureRefund = 18.f;

	// ---- 拼刀（双方同一窗口内都在挥刀）----
	// 0.15s：略宽于完美弹反窗（0.22s）的一半。
	// 太宽会把"我先砍到、对方才起手"也算成拼刀（判定发水），
	// 太窄则实战几乎撞不出来（双方判定窗本来就不对齐）。
	inline constexpr float ClashWindow = 0.15f;
	// 稳心：拼刀抵消掉双方身上的硬直，并回一截架势。
	// 给 15（比完美闪避 25 少、比完美弹反 18 略少）——
	// 拼刀是"撞上了"而不是"操作对了"，回报必须低于两种完美操作，
	// 否则玩家会去赌对撞而不是去读招。
	inline constexpr float ClashPostureRefund = 15.f;
	// 内置冷却：连段里每一刀都触发稳心的话，硬直系统就失效了
	inline constexpr float ClashCooldown = 0.6f;
	inline constexpr float ClashHitStop = 0.10f;
	inline constexpr float ClashHitStopScale = 0.10f;

	// 被破防：硬直时长 + 这一下吃多少伤害（格挡减伤从 0.25 抬到 0.6，也就是"部分伤害"）
	inline constexpr float GuardBreakStaggerTime = 1.0f;
	inline constexpr float GuardBreakDamageScale = 0.6f;
	// 破防后架势恢复到多少（0 = 空槽起身，1 = 满槽；给 0.5 让玩家还有半条命可挡）
	inline constexpr float GuardBreakPostureRecoverRatio = 0.5f;

	// ---- 失衡 / 击破 ----
	// 失衡：受伤加成 + 绿色轮廓，但 Boss 仍能行动（不是硬控）。
	// 时长到了自动退出并清空两条槽 → 它是"限时加成的输出窗口"，不是永久 debuff。
	inline constexpr float UnstableDamageBonus = 1.25f;    // 失衡期间受到的伤害倍率
	inline constexpr float UnstableDuration = 6.f;         // 失衡持续时间（秒）
	// 击破：在失衡状态下把元素槽也打满 → 失去格挡 + 虚弱窗口。
	// 3.5s 取自参考 Demo 的"3~4s 虚弱窗口"，够玩家打完一整套连段 + 位移。
	inline constexpr float AnomalyBrokenWeakDuration = 3.5f;

	// BOSS 韧性（Poise）清零 → 破防虚弱窗口。
	// 这段 BOSS 完全停手（不出招、不走位），是玩家最稳定的输出窗口。
	// 比失衡击破（3.5s）略短：韧性破防是常规手段、会反复触发，给太长会让 BOSS 全程站桩。
	inline constexpr float BossPoiseBreakWeakDuration = 2.5f;
}
