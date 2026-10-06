// UE 5.7: 抑制 FInheritedTagContainer 废弃 API 警告（详见 DemoGameplayEffectHelpers.cpp 注释）
#pragma warning(push)
#pragma warning(disable: 4996)

#include "Abilities/PlayerAbilities.h"
#include "Core/DemoCharacterBase.h"
// 必须完整包含属性集：这里要用 UDemoAttributeSet::GetStaminaAttribute() 之类的静态访问器。
// DemoCharacterBase.h 里它只是前向声明，少了这一行就是 C2027「使用了未定义类型」+ C3861。
#include "Core/DemoAttributeSet.h"
#include "Core/DemoCombatTuning.h"
#include "Core/DemoCommon.h"
#include "Core/DemoGameplayTags.h"
#include "Core/DemoAbilitySystemLibrary.h"
#include "Effects/DemoGameplayEffects.h"
#include "Player/FightPlayerCharacter.h"
#include "UI/CombatHUDWidget.h"

#include "Abilities/Tasks/AbilityTask_PlayMontageAndWait.h"
#include "Abilities/Tasks/AbilityTask_WaitGameplayEvent.h"
#include "Abilities/Tasks/AbilityTask_WaitDelay.h"
#include "Abilities/Tasks/AbilityTask_ApplyRootMotionConstantForce.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "UObject/UnrealType.h"

// 默认蒙太奇路径：当蓝图子类没在编辑器里配 ComboMontages / DirectionalMontages 时，
// 直接从这些已知路径加载，保证技能一定能播动画（避免“激活即 EndAbility、毫无反应”）。
static UAnimMontage* LoadMontage(const TCHAR* Path)
{
	return ::LoadObject<UAnimMontage>(nullptr, Path);
}

// 闪避方向槽位：0=前(+X)  1=后(-X)  2=左(-Y)  3=右(+Y)
// 与角色本地坐标一致：X=前，Y=右
enum class EDodgeSlot : int32
{
	Forward = 0,
	Backward = 1,
	Left = 2,
	Right = 3,
	Count = 4
};

// 把“本地方向”量化成 4 个方向之一（取分量绝对值大的那个轴）。
// LocalDir 已经是“镜头坐标系”下的方向：+X=前 / -X=后 / -Y=左 / +Y=右；
// 零向量 = 没按方向键 → 默认向前闪。
static EDodgeSlot QuantizeLocalDir(const FVector& LocalDir)
{
	const FVector Dir = LocalDir.IsNearlyZero() ? FVector(1.f, 0.f, 0.f) : LocalDir.GetSafeNormal2D();

	if (FMath::Abs(Dir.X) >= FMath::Abs(Dir.Y))
	{
		return Dir.X >= 0.f ? EDodgeSlot::Forward : EDodgeSlot::Backward;
	}
	return Dir.Y >= 0.f ? EDodgeSlot::Right : EDodgeSlot::Left;
}

// 从资产名里认方向：Dodge_F / Dodge_B / Dodge_L / Dodge_R，
// 或 ..._Forward / ..._Back / ..._Left / ..._Right。
//
// 为什么按名字而不是只按数组下标：
//   数组顺序属于“资产配置”，很容易被脚本或手滑写反 —— 历史上 DirectionalMontages
//   就被填成了 [R, L, B, F]，结果按 W 向右闪、按 D 向前闪、闪避距离还因为动画自带根位移变长。
//   按名字识别后，无论数组怎么排，选出来的动画都是对的（下标顺序退化为兜底）。
static bool GuessDodgeSlotFromName(const FString& AssetName, EDodgeSlot& OutSlot)
{
	const FString Name = AssetName.ToUpper();

	// 先长后短：否则 "_LEFT" 会被 "_L" 抢先命中
	if (Name.Contains(TEXT("_FORWARD"))) { OutSlot = EDodgeSlot::Forward;  return true; }
	if (Name.Contains(TEXT("_BACK")))    { OutSlot = EDodgeSlot::Backward; return true; }
	if (Name.Contains(TEXT("_LEFT")))    { OutSlot = EDodgeSlot::Left;     return true; }
	if (Name.Contains(TEXT("_RIGHT")))   { OutSlot = EDodgeSlot::Right;    return true; }
	if (Name.Contains(TEXT("_F")))       { OutSlot = EDodgeSlot::Forward;  return true; }
	if (Name.Contains(TEXT("_B")))       { OutSlot = EDodgeSlot::Backward; return true; }
	if (Name.Contains(TEXT("_L")))       { OutSlot = EDodgeSlot::Left;     return true; }
	if (Name.Contains(TEXT("_R")))       { OutSlot = EDodgeSlot::Right;    return true; }

	return false;
}

// 按方向取蒙太奇：先按资产名识别（抗数组顺序错乱），4 段都识别得出才用它；
// 否则退回“数组下标 = 方向枚举”的约定顺序。
static UAnimMontage* PickDodgeMontageBySlot(const TArray<UAnimMontage*>& Montages, EDodgeSlot Slot, bool& bOutMatchedByName)
{
	bOutMatchedByName = false;

	UAnimMontage* BySlot[static_cast<int32>(EDodgeSlot::Count)] = { nullptr, nullptr, nullptr, nullptr };
	int32 NumNamed = 0;

	for (UAnimMontage* M : Montages)
	{
		if (!M)
		{
			continue;
		}

		EDodgeSlot S;
		if (GuessDodgeSlotFromName(M->GetName(), S))
		{
			const int32 Idx = static_cast<int32>(S);
			if (!BySlot[Idx])
			{
				BySlot[Idx] = M;
				++NumNamed;
			}
		}
	}

	if (NumNamed == static_cast<int32>(EDodgeSlot::Count))
	{
		bOutMatchedByName = true;
		return BySlot[static_cast<int32>(Slot)];
	}

	if (Montages.Num() >= static_cast<int32>(EDodgeSlot::Count))
	{
		return Montages[static_cast<int32>(Slot)];
	}

	return nullptr;
}

// ================================================================ 连段普攻
UGA_PlayerNormalAttack::UGA_PlayerNormalAttack()
{
	FGameplayTagContainer DefaultTags;
	DefaultTags.AddTag(DemoTag::Ability_Attack_Normal);
	SetAssetTags(DefaultTags);
	ActivationOwnedTags.AddTag(DemoTag::State_Attacking);

	// 出招时不允许中途换技能（连段靠内部推进）。
	//
	// ⚠ 这里【故意不】阻挡闪避（曾经有一行 BlockAbilitiesWithTag.AddTag(Ability_Movement_Dodge)）。
	//   BlockAbilitiesWithTag 的语义是"本技能激活期间，带这些 tag 的技能不能被激活" ——
	//   加了它就意味着"一旦开始砍，Shift 就彻底没反应"，
	//   玩家完全没有"看招 → 闪避取消后摇"的操作空间，手感非常僵硬，
	//   而闪避取消恰恰是这类动作游戏最核心的节奏来源。
	//   现在改成单向关系：闪避可以随时插进来取消普攻
	//   （见 UGA_PlayerDodge 构造函数里的 CancelAbilitiesWithTag）。
	BlockAbilitiesWithTag.AddTag(DemoTag::Ability_Attack_Skill);

	// Cancel 窗口表的一环：普攻可以取消【弹刀】。
	// 完美弹反成功后弹刀技能还挂着一段"收势"（ParrySuccessLockTime = 0.14s），
	// 没有这一行的话，玩家弹完想立刻接一刀会石沉大海 ——
	// 只狼式"弹开就反击"的节奏就没了，弹反的回报也会少一大截。
	CancelAbilitiesWithTag.AddTag(DemoTag::Ability_Defense_Parry);

	// ---------------- 连段默认伤害 ----------------
	// 以前这里是空的 → 播放时走 `ComboDamages.Num() > 0 ? ... : 10.f` 的兜底，
	// 于是玩家每一段只打 10 点、BOSS 几十秒就被砍死。
	// 现在把平衡表里的值直接写进 CDO：蓝图不配也是对的，配了则以蓝图为准。
	// 数值推导（为什么要 28/34/42/56）见 Core/DemoCombatTuning.h 顶部。
	ComboDamages = {
		DemoBalance::PlayerComboDamage1,
		DemoBalance::PlayerComboDamage2,
		DemoBalance::PlayerComboDamage3,
		DemoBalance::PlayerComboDamage4
	};

	// BOSS 韧性 320，每段削韧 14 → 约 23 段破防一次（≈18s 一个输出窗口）
	PoiseDamage = DemoBalance::PlayerComboPoiseDamage;
}

void UGA_PlayerNormalAttack::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	Super::ActivateAbility(Handle, ActorInfo, ActivationInfo, TriggerEventData);

	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	ComboIndex = 0;
	bComboWindowOpen = false;
	bBufferedNext = false;
	bAdvancing = false;
	// 每次重新起手都重置"本技能持有几层锁 / 是否压过移速"。
	// 上一轮结束时的计数必须归零（EndAbility 里 RestoreMovement 会做），
	// 这里再兜一次底，防止异常路径带着上一轮的残留锁开始新的一轮。
	MovementLockHeld = 0;
	bSpeedReduced = false;
	bOvercommitted = false;   // 每次重新起手都当作没贪刀：惩罚只针对"这一套打完还接着按"

	// 记下攻击前角色的移速（仅作兜底）。压低移速 + 锁 WASD 由 PlayCurrentCombo 开头的
	// LockMovement() 完成 —— 放在那里是为了让【每一段】都重新锁一次（上一段的收招窗口会提前解锁）。
	if (ACharacter* AvatarChar = GetAvatarCharacter())
	{
		if (UCharacterMovementComponent* MoveComp = AvatarChar->GetCharacterMovement())
		{
			SavedMaxWalkSpeed = MoveComp->MaxWalkSpeed;
		}
	}

	if (ADemoCharacterBase* Character = Cast<ADemoCharacterBase>(GetAvatarCharacter()))
	{
		Character->SetMeleeHitWindowEnabled(false);
	}

	// 只监听“连段输入”事件（鼠标左键在连段窗口内按下时由控制器发送）。
	// 连段窗口 / 命中窗口改由 PlayCurrentCombo 里的计时器驱动，
	// 不再依赖蒙太奇里是否放了 ComboWindow 通知 —— 这样即使蒙太奇是空的，连段也能跑。
	if (UAbilityTask_WaitGameplayEvent* InputTask =
		UAbilityTask_WaitGameplayEvent::WaitGameplayEvent(this, DemoTag::Event_Ability_ComboInput, nullptr, false, true))
	{
		InputTask->EventReceived.AddDynamic(this, &UGA_PlayerNormalAttack::OnComboInput);
		InputTask->ReadyForActivation();
	}

	// 出招前把根位移模式掰到“会提取根位移”的状态。
	// 只有提取根位移，蒙太奇的根骨骼才会在姿势里被重置（动画不会推着 Mesh 滑出去），
	// 位移也才会真正作用到胶囊体上 —— 否则看到的“位移”只是 Mesh 在滑，播完就弹回。
	if (AFightPlayerCharacter* Player = Cast<AFightPlayerCharacter>(GetAvatarCharacter()))
	{
		Player->EnsureRootMotionExtraction(TEXT("普攻"));
		// 记录位移起点 + 位移期间让镜头与角色刚性同步（视角跟着角色）
		Player->BeginDisplacementAssist();
	}

	PlayCurrentCombo();
}

void UGA_PlayerNormalAttack::EndAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	bool bReplicateEndAbility, bool bWasCancelled)
{
	ComboIndex = 0;
	bComboWindowOpen = false;
	bBufferedNext = false;
	bAdvancing = false;
	bOvercommitted = false;

	// 恢复移动（解锁 WASD + 还原移速）。收招窗口可能已经还原过，
	// RestoreMovement 用 MovementLockHeld 计数天然幂等（归零后直接返回）。
	RestoreMovement();

	if (ADemoCharacterBase* Character = Cast<ADemoCharacterBase>(GetAvatarCharacter()))
	{
		Character->SetMeleeHitWindowEnabled(false);
	}

	// 结束位移辅助：还原镜头，并量出本轮普攻的【真实位移】。
	// 这个数字就是判据：>0 = 动画根位移真的推动了胶囊体（真移动）；
	//                 ≈0 = 位移只发生在 Mesh 上（会被拉回），需要查根位移模式。
	if (AFightPlayerCharacter* Player = Cast<AFightPlayerCharacter>(GetAvatarCharacter()))
	{
		const float MoveDistance = Player->EndDisplacementAssist();
		UE_LOG(LogTemp, Log,
			TEXT("[Combo] 本轮普攻实际位移 = %.0f cm（>0 = 真实位移；≈0 = 位移只在 Mesh 上，会拉回；"
				 "根位移模式=%d，1 才是期望值）"),
			MoveDistance, Player->GetAnimRootMotionMode());
	}

	// 还原根位移缩放，避免影响后续其它蒙太奇
	if (ACharacter* AvatarChar = GetAvatarCharacter())
	{
		AvatarChar->SetAnimRootMotionTranslationScale(1.f);
	}

	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}

void UGA_PlayerNormalAttack::PlayCurrentCombo()
{
	ACharacter* Avatar = GetAvatarCharacter();

	// 本段蒙太奇：优先用蓝图配的 ComboMontages，没配则按索引加载 4 段默认蒙太奇
	UAnimMontage* Montage = ComboMontages.IsValidIndex(ComboIndex) ? ComboMontages[ComboIndex] : nullptr;
	if (!Montage)
	{
		static const TCHAR* FallbackPaths[] = {
			TEXT("/Game/FightPlayer/Animation/Attack_Combo_01_Montage.Attack_Combo_01_Montage"),
			TEXT("/Game/FightPlayer/Animation/Attack_Combo_02_Montage.Attack_Combo_02_Montage"),
			TEXT("/Game/FightPlayer/Animation/Attack_Combo_03_Montage.Attack_Combo_03_Montage"),
			TEXT("/Game/FightPlayer/Animation/Attack_Combo_04_Montage.Attack_Combo_04_Montage"),
		};
		if (ComboIndex >= 0 && ComboIndex < UE_ARRAY_COUNT(FallbackPaths))
		{
			Montage = LoadMontage(FallbackPaths[ComboIndex]);
		}
	}

	if (!Avatar || !Montage)
	{
		UE_LOG(LogTemp, Error, TEXT("[Combo] 第 %d 段普攻蒙太奇缺失，无法播放"), ComboIndex);
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
		return;
	}

	if (Montage->GetPlayLength() <= 0.f)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Combo] 普攻蒙太奇 %s 时长为 0（空壳资产）"), *GetNameSafe(Montage));
	}

	// —— 根运动检查 / 缩放 ——
	//
	// 说明（踩过的坑）：Enable Root Motion 是【AnimSequence】上的属性，Montage 上并没有这个字段，
	// 所以以前对着 Montage 反射找 bEnableRootMotion 的那段兜底其实从未生效（永远找不到属性）。
	// 蒙太奇能不能提取根位移，取决于它引用的源动画，用 Montage->HasRootMotion() 就能看出来；
	// 而这个值又决定了姿势里的根骨骼是否被重置（见 FightPlayerCharacter::EnsureRootMotionExtraction 的注释）——
	// 换言之：HasRootMotion=false 时，动画的位移只会推着 Mesh 滑，播完弹回，胶囊体不动（“拉回”）。
	// 这里只做两件事：打印判据 + 把 AttackRootMotionScale 真正作用到角色身上。
	{
		const bool bHasRootMotion = Montage->HasRootMotion();
		if (!bHasRootMotion)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[Combo] %s 没有根位移数据（引用的源动画未勾 Enable Root Motion）→ 位移只会留在 Mesh 上。"
					 "请跑 Tools/SetupAttackComboMontages.py 修资产。"),
				*GetNameSafe(Montage));
		}

		// 真正作用于角色的根位移缩放：1 = 完整前冲，0.5 = 减半，0 = 完全原地（不位移也不回弹）
		Avatar->SetAnimRootMotionTranslationScale(AttackRootMotionScale);

		UE_LOG(LogTemp, Log, TEXT("[Combo] 第 %d 段：HasRootMotion=%d 根位移缩放=%.2f"),
			ComboIndex, bHasRootMotion ? 1 : 0, AttackRootMotionScale);
	}

	if (ADemoCharacterBase* DemoCharacter = Cast<ADemoCharacterBase>(Avatar))
	{
		// 伤害取数（防御性，避免"打中却 0 伤害、血条永远不动"的历史事故）：
		//   ① 优先用蓝图配的 ComboDamages（设计师可在蓝图里微调各段伤害）；
		//   ② 但只要蓝图把它【留空】或【填了 0】，就退回平衡表的权威值
		//      —— 因为项目里反复踩过"蓝图 CDO 序列化覆盖了 C++ 默认值"的坑，
		//      一旦蓝图里残留旧的空数组，伤害就会悄无声息地变 0。
		//   ③ 两种来源都不合法时，兜底给 10 点，保证一定造成伤害。
		static const float BalanceDamages[4] = {
			DemoBalance::PlayerComboDamage1,
			DemoBalance::PlayerComboDamage2,
			DemoBalance::PlayerComboDamage3,
			DemoBalance::PlayerComboDamage4
		};
		const float RawDamage = (ComboDamages.IsValidIndex(ComboIndex) && ComboDamages[ComboIndex] > 0.f)
			? ComboDamages[ComboIndex]
			: (ComboIndex >= 0 && ComboIndex < 4 ? BalanceDamages[ComboIndex] : 10.f);

		// 两个"打折"来源叠乘，而不是二选一：
		//   · 体力耗尽（0.6）：语义是"挥得动但没力气"
		//   · 贪刀（0.7）：语义是"这一刀出得太急、没吃住劲"
		// 同时发生时该更亏，这正是"又没体力又不收手"应得的反馈。
		UAbilitySystemComponent* ASC = UDemoAbilitySystemLibrary::GetAbilitySystemComponent(Avatar);
		const bool bExhausted = ASC && ASC->HasMatchingGameplayTag(DemoTag::State_StaminaExhausted);

		float DamageScale = 1.f;
		if (bExhausted)
		{
			DamageScale *= DemoBalance::StaminaExhaustedDamageScale;
		}
		if (bOvercommitted)
		{
			DamageScale *= DemoBalance::ComboOvercommitDamageScale;
		}
		const float Damage = RawDamage * DamageScale;

		DemoCharacter->SetCurrentAttackParams(Damage, PoiseDamage, false, true);

		// 本段体力消耗：连段越往后越贵（8 / 10 / 12 / 16）。
		// 这是"体力真正参与决策"的关键改动 —— 原本普攻【零消耗】，
		// 玩家永远打不出体力见底这个状态，"要不要收手"也就根本不是一个决策。
		const float ComboStaminaCost = (ComboIndex >= 0 && ComboIndex < 4)
			? DemoBalance::PlayerComboStaminaCost[ComboIndex]
			: DemoBalance::PlayerComboStaminaCost[3];
		UDemoAbilitySystemLibrary::ApplyStaminaCost(Avatar, ComboStaminaCost);
	}

	// 每段开始时重新锁住 WASD（上一段的收招窗口可能已提前解锁过移动）
	// ⚠⚠⚠ 2026-10-03 关键修复：必须【在 EndTask(ActiveRecover) 之前】先把上一段残留的锁还掉。
	//
	// 泄漏原理（实测锁计数一路涨到 15 层 = 彻底动不了）：
	//   段N 的收招窗口任务 ActiveRecover 还没触发 → 玩家已经输入下一段
	 //   → 这里 LockMovement() 又锁一次（MovementLockHeld++）
	 //   → 紧接着 EndTask(ActiveRecover) 把"本该解锁"的那个任务杀了
	 //   → 收招窗口的回调【永远不会执行】→ 那一次 LockMovement 永远没有配对的 Unlock
	 //   连打越多次，泄漏的锁层数越多 → 最后彻底动不了。
	//
	// 正确做法：换段时先 RestoreMovement() 把上一段的锁还清，再为新段 LockMovement()。
	// RestoreMovement 用 MovementLockHeld 计数、归零即返回，天然幂等。
	RestoreMovement();

	// 每段开始时重新锁住 WASD（上一段的收招窗口可能已提前解锁过移动）
	LockMovement();

	// —— 先结束上一段遗留的任务，避免旧的连段/命中窗口计时器串到当前段 ——
	auto EndTask = [](UAbilityTask* T) { if (T) { T->EndTask(); } };
	EndTask(ActiveHitOn);   ActiveHitOn = nullptr;
	EndTask(ActiveHitOff);  ActiveHitOff = nullptr;
	EndTask(ActiveWinOpen); ActiveWinOpen = nullptr;
	EndTask(ActiveWinClose); ActiveWinClose = nullptr;
	EndTask(ActiveRecover); ActiveRecover = nullptr;
	EndTask(ActiveMontageTask); ActiveMontageTask = nullptr;

	// 根运动位移缩放（CreatePlayMontageAndWaitProxy 的第 7 个参数就是 AnimRootMotionTranslationScale）：
	//   过去这里写死 0（想“原地挥砍”），但由于攻击蒙太奇没勾 Enable Root Motion，
	//   动画的根骨位移根本没走进根运动通道，而是直接推着 Mesh 走 → 播完弹回原位（“拉回”）。
	//   现在改用攻击蒙太奇自带的真实根运动（AttackRootMotionScale 默认 1.0），
	//   位移是真正作用到角色身上的，所以不会再回弹；想减少前冲把它调小即可（0 = 完全原地）。
	// 连段加速：越往后段播放越快，连续点击时连段更紧凑流畅
	const float PlayRate = FMath::Max(0.1f, 1.f + ComboSpeedupPerStage * (float)ComboIndex);
	UAbilityTask_PlayMontageAndWait* Task = UAbilityTask_PlayMontageAndWait::CreatePlayMontageAndWaitProxy(
		this, TEXT("ComboMontage"), Montage, PlayRate, NAME_None, true, FMath::Max(0.f, AttackRootMotionScale));

	if (!Task)
	{
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
		return;
	}
	ActiveMontageTask = Task;

	// —— C++ 计时驱动“连段窗口”和“命中窗口”，不依赖蒙太奇通知 ——
	// 因为上面按 PlayRate 加速播放，实际在屏时长是 Len/PlayRate，
	// 所以窗口计时要用“有效时长” EffectiveLen，否则窗口会出现在动画放完之后。
	const float Len = Montage->GetPlayLength();
	const float EffectiveLen = Len / PlayRate;
	if (EffectiveLen > 0.1f)
	{
		// 命中窗口：从挥砍开始到收招前（开启近战判定）
		if (UAbilityTask_WaitDelay* HitOn = UAbilityTask_WaitDelay::WaitDelay(this, EffectiveLen * 0.18f))
		{
			ActiveHitOn = HitOn;
			HitOn->OnFinish.AddDynamic(this, &UGA_PlayerNormalAttack::OnMeleeWindowOpen);
			HitOn->ReadyForActivation();
		}
		if (UAbilityTask_WaitDelay* HitOff = UAbilityTask_WaitDelay::WaitDelay(this, EffectiveLen * 0.78f))
		{
			ActiveHitOff = HitOff;
			HitOff->OnFinish.AddDynamic(this, &UGA_PlayerNormalAttack::OnMeleeWindowClose);
			HitOff->ReadyForActivation();
		}
		// 连段窗口：提前开放（ComboWindowOpenRatio），允许鼠标左键接下一段 → 连段更跟手
		const float WinOpenTime = EffectiveLen * FMath::Clamp(ComboWindowOpenRatio, 0.05f, 0.6f);
		if (UAbilityTask_WaitDelay* WinOpen = UAbilityTask_WaitDelay::WaitDelay(this, WinOpenTime))
		{
			ActiveWinOpen = WinOpen;
			WinOpen->OnFinish.AddDynamic(this, &UGA_PlayerNormalAttack::OnComboWindowOpen);
			WinOpen->ReadyForActivation();
		}
		// 连段窗口关闭：开放后再过 ComboResetDelay 秒仍没点左键 → 关闭输入接收，
		// 本段播完即回退至第1段（满足“超过时间没点击就回退”）。
		// 把关闭时刻钳制在本段蒙太奇结束前（≤95%有效长度），避免上一招的关闭计时器
		// 串到下一招、误关新招的连段窗口。
		if (ComboResetDelay > 0.f)
		{
			const float CloseTime = FMath::Min(WinOpenTime + ComboResetDelay, EffectiveLen * 0.95f);
			if (CloseTime > WinOpenTime)
			{
				if (UAbilityTask_WaitDelay* WinClose = UAbilityTask_WaitDelay::WaitDelay(this, CloseTime))
				{
					ActiveWinClose = WinClose;
					WinClose->OnFinish.AddDynamic(this, &UGA_PlayerNormalAttack::OnComboWindowClose);
					WinClose->ReadyForActivation();
				}
			}
		}

		// 收招窗口：播到 ComboRecoverRatio（默认 0.70）就把移动权还给玩家 ——
		// 最后 30% 的收招动作期间可以立刻走/跑，不用等整段播完，解决“打完愣一下才动”的滞涩感。
		// 同时也解锁了角色，所以这段期间的 WASD 输入会被正常处理。
		// 贪刀状态额外延长收招：连点打满一轮后还想接着按，就得忍受更长的收招空档。
		// （若加完已接近整段时长，下面就不会创建收招任务 —— 移动权一直锁到技能结束，
		//   这本身就是"贪刀 = 更久动不了"的正确体感。）
		const float RecoverTime = EffectiveLen * FMath::Clamp(ComboRecoverRatio, 0.3f, 1.f)
			+ (bOvercommitted ? DemoBalance::ComboOvercommitRecoveryBonus : 0.f);
		if (RecoverTime < EffectiveLen * 0.99f)
		{
			if (UAbilityTask_WaitDelay* Recover = UAbilityTask_WaitDelay::WaitDelay(this, RecoverTime))
			{
				ActiveRecover = Recover;
				Recover->OnFinish.AddDynamic(this, &UGA_PlayerNormalAttack::OnRecoveryWindowOpen);
				Recover->ReadyForActivation();
			}
		}
	}

	Task->OnCompleted.AddDynamic(this, &UGA_PlayerNormalAttack::OnMontageCompleted);
	Task->OnCancelled.AddDynamic(this, &UGA_PlayerNormalAttack::OnMontageCancelled);
	Task->OnInterrupted.AddDynamic(this, &UGA_PlayerNormalAttack::OnMontageCancelled);
	Task->ReadyForActivation();

	UE_LOG(LogTemp, Log, TEXT("[Combo] 播放第 %d 段普攻"), ComboIndex);
}

void UGA_PlayerNormalAttack::AdvanceToNextCombo()
{
	if (bAdvancing)
	{
		return;
	}
	bAdvancing = true;
	bBufferedNext = false;
	bComboWindowOpen = false;

	// 顺序循环：1→2→3→4→1…（索引对段数取模，第4段之后回到第1段）
	const int32 StageCount = ComboMontages.Num() > 0 ? ComboMontages.Num() : 4;
	const int32 NextIndex = (ComboIndex + 1) % StageCount;

	// 体力不足 → 打不出第 3/4 段（索引 >= 2）：连段到这里强制收招。
	// 这是"体力参与决策"最直观的一条：没力气了就只能打两下，
	// 玩家得自己判断"这两下够不够、要不要先拉开喘口气"，而不是永远无脑连点。
	if (NextIndex >= 2)
	{
		if (UAbilitySystemComponent* ASC = UDemoAbilitySystemLibrary::GetAbilitySystemComponent(GetAvatarCharacter()))
		{
			const float MaxStamina = ASC->GetNumericAttribute(UDemoAttributeSet::GetMaxStaminaAttribute());
			const float CurStamina = ASC->GetNumericAttribute(UDemoAttributeSet::GetStaminaAttribute());
			if (MaxStamina > KINDA_SMALL_NUMBER
				&& (CurStamina / MaxStamina) < DemoBalance::StaminaLowRatio)
			{
				bAdvancing = false;
				bBufferedNext = false;
				// 不推进段数：当前这段自然放完，由 OnMontageCompleted 正常收招。
				// 这里不强制 EndAbility —— 硬打断会让第 2 段像被吞掉一样突兀。
				return;
			}
		}
	}

	// 段数绕回第 1 段 = 玩家打满一轮（第4段）后还继续按 → 进入贪刀状态。
	// 之后每一刀都后摇更长、伤害更低（在 PlayCurrentCombo 里生效），
	// 直到本次连段技能结束才解除。
	if (NextIndex == 0)
	{
		bOvercommitted = true;
	}

	ComboIndex = NextIndex;

	// 先解绑旧蒙太奇任务的回调、再结束它：这样旧蒙太奇真正停止时
	// 不会再回调到 OnMontageCancelled/OnMontageCompleted 误杀技能（它会在下一帧才真正停）。
	// 然后无缝接下一段 —— “点下左键”当下就跳到下一段，而不是等当前整段放完，连点非常跟手。
	if (ActiveMontageTask)
	{
		ActiveMontageTask->OnCompleted.RemoveDynamic(this, &UGA_PlayerNormalAttack::OnMontageCompleted);
		ActiveMontageTask->OnCancelled.RemoveDynamic(this, &UGA_PlayerNormalAttack::OnMontageCancelled);
		ActiveMontageTask->OnInterrupted.RemoveDynamic(this, &UGA_PlayerNormalAttack::OnMontageCancelled);
		ActiveMontageTask->EndTask();
		ActiveMontageTask = nullptr;
	}

	PlayCurrentCombo();

	bAdvancing = false;
}

void UGA_PlayerNormalAttack::OnMontageCompleted()
{
	// 这是“即时跳段”时旧蒙太奇被结束触发的回调，直接忽略，由新一段接管
	if (bAdvancing)
	{
		return;
	}

	// 本段自然播完且窗口内没收到续段输入（超过 ComboResetDelay 未点左键）→
	// 连段结束，索引归零，下次左键从第1段开始
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
}

void UGA_PlayerNormalAttack::OnMontageCancelled()
{
	// 同上：自己主动跳段触发的停止，忽略
	if (bAdvancing)
	{
		return;
	}
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, true);
}

void UGA_PlayerNormalAttack::OnComboWindowClose()
{
	// 窗口超时：关闭输入接收，之后即使点击也不会续段（本段结束后回退第1段）
	bComboWindowOpen = false;
}

void UGA_PlayerNormalAttack::LockMovement()
{
	// 两件事一起做：
	//   1) 锁 WASD 输入（HandleMove 直接 return）—— 比只改 MaxWalkSpeed 更彻底，
	//      因为 MaxWalkSpeed 只管速度上限，残余速度仍会让角色滑出去；
	//   2) 压低移速上限 —— 用 AttackMoveSpeedScale（默认 0.3）而不是 0，
	//      这样起手瞬间是“平滑刹车”而不是被瞬间钳到 0 的“钉在原地”。
	//
	// ⚠⚠⚠ 2026-10-03 修复：这里【每段连招都会调一次】（PlayCurrentCombo 开头，段数最多 4），
	//   而移动锁是【引用计数】。所以必须每次 Lock 并把次数记进 MovementLockHeld，
	//   由 RestoreMovement 一次性减掉同样的次数。
	//   之前这里配的是单个 bool bMovementRestored → "锁 4 次只减 1 次"
	//   → 计数永久多 3 层 → **攻击后角色彻底动不了**（尤其是连打之后）。
	//   bool 时代重复 Lock 无害，计数时代会累积 —— 换数据结构时必须重审所有"重复加锁"的调用点。
	if (AFightPlayerCharacter* Player = Cast<AFightPlayerCharacter>(GetAvatarCharacter()))
	{
		Player->LockMoveInput();
		++MovementLockHeld;
	}

	// ⚠ 移速只压一次：旧代码每段都压，连打四段会叠成 0.3^4 ≈ 0.008% —— 同样是"动不了"。
	//   基值固定用 SavedMaxWalkSpeed（ActivateAbility 记的"起手前移速"），可重复且幂等。
	if (!bSpeedReduced)
	{
		if (ACharacter* AvatarChar = GetAvatarCharacter())
		{
			if (UCharacterMovementComponent* MoveComp = AvatarChar->GetCharacterMovement())
			{
				const float Base = SavedMaxWalkSpeed > 0.f ? SavedMaxWalkSpeed : MoveComp->MaxWalkSpeed;
				MoveComp->MaxWalkSpeed = Base * FMath::Clamp(AttackMoveSpeedScale, 0.f, 1.f);
				bSpeedReduced = true;
			}
		}
	}
}

void UGA_PlayerNormalAttack::RestoreMovement()
{
	// 幂等：本函数会被【收招窗口】和【EndAbility】各调一次，也可能连调多次。
	// MovementLockHeld 归零 = 已经全解开，直接返回。
	// （用计数而不是 bool 天然幂等，不需要额外的 bMovementRestored 标志。）
	if (MovementLockHeld <= 0)
	{
		return;
	}

	if (AFightPlayerCharacter* Player = Cast<AFightPlayerCharacter>(GetAvatarCharacter()))
	{
		// 把本技能累计加的锁【全部】减掉。少减 = 永久僵住。
		for (int32 i = 0; i < MovementLockHeld; ++i)
		{
			Player->UnlockMoveInput();
		}
		MovementLockHeld = 0;

		// ⚠ 只有"所有锁都解除了"才还原移速 —— 否则会抢在别的技能（闪避/弹反）之前
		//   把它们压低的移速顶回去，造成"那边刹车、这边加速"的打架。
		if (!Player->IsMoveInputLocked())
		{
			if (UCharacterMovementComponent* MoveComp = Player->GetCharacterMovement())
			{
				MoveComp->MaxWalkSpeed = Player->GetDesiredMaxWalkSpeed();
			}
		}
		return;
	}
	MovementLockHeld = 0;

	// 兜底（非玩家角色）：还原攻击前记录的移速
	if (ACharacter* AvatarChar = GetAvatarCharacter())
	{
		if (UCharacterMovementComponent* MoveComp = AvatarChar->GetCharacterMovement())
		{
			if (SavedMaxWalkSpeed > 0.f)
			{
				MoveComp->MaxWalkSpeed = SavedMaxWalkSpeed;
			}
		}
	}
}

void UGA_PlayerNormalAttack::OnRecoveryWindowOpen()
{
	// 收招窗口（ComboRecoverRatio，默认 0.70）：提前把移动权还给玩家，
	// 打完不用等整段动画播完就能立刻走/跑 —— 消除“打完愣一下才能动”的滞涩感。
	RestoreMovement();
}

void UGA_PlayerNormalAttack::OnMeleeWindowOpen()
{
	if (ADemoCharacterBase* Character = Cast<ADemoCharacterBase>(GetAvatarCharacter()))
	{
		Character->SetMeleeHitWindowEnabled(true);
	}
}

void UGA_PlayerNormalAttack::OnMeleeWindowClose()
{
	if (ADemoCharacterBase* Character = Cast<ADemoCharacterBase>(GetAvatarCharacter()))
	{
		Character->SetMeleeHitWindowEnabled(false);
	}
}

void UGA_PlayerNormalAttack::OnComboInput(FGameplayEventData Payload)
{
	// 连段窗口已开 → 立即跳到下一段（即时取消接段，最跟手）
	if (bComboWindowOpen)
	{
		AdvanceToNextCombo();
	}
	else
	{
		// 窗口还没开（比如刚出第一刀的前摇阶段就点了）→ 先缓冲，
		// 等 OnComboWindowOpen 一开就立刻续段，手感更跟手、不丢输入。
		// 记下按下时刻：缓冲不是无限期的，过期输入会在窗口打开时被丢弃（防"幽灵自动接段"）。
		bBufferedNext = true;
		BufferedInputTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	}
}

void UGA_PlayerNormalAttack::OnComboWindowOpen()
{
	bComboWindowOpen = true;
	// 消费缓冲：前一刀前摇阶段点下的左键，现在立刻续上下一段。
	// 但只消费"新鲜"的缓冲：按下超过 ComboBufferExpiryTime（0.45s）的输入已经和玩家
	// 当前意图脱节 —— 那是起手前摇时误触/过早点的那一下，替他自动接段反而显得"自己出刀"。
	if (bBufferedNext)
	{
		const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
		const bool bFresh = (Now - BufferedInputTime) <= DemoBalance::ComboBufferExpiryTime;
		if (bFresh)
		{
			AdvanceToNextCombo();
		}
		else
		{
			bBufferedNext = false;
			UE_LOG(LogDemoCombat, Log, TEXT("[Combo] 丢弃过期缓冲输入（按下后 %.2fs 才开窗 > %.2fs）"),
				Now - BufferedInputTime, DemoBalance::ComboBufferExpiryTime);
		}
	}
}

// ================================================================ 技能攻击
UGA_PlayerAttackSkill::UGA_PlayerAttackSkill()
{
	FGameplayTagContainer DefaultTags;
	DefaultTags.AddTag(DemoTag::Ability_Attack_Skill);
	SetAssetTags(DefaultTags);
	ActivationOwnedTags.AddTag(DemoTag::State_Attacking);

	CooldownGameplayEffectClass = UGE_SkillCooldownEffect::StaticClass();

	// 同样不阻挡闪避：技能动作更长，更需要能被闪避取消（否则放完技能就是活靶子）
	BlockAbilitiesWithTag.AddTag(DemoTag::Ability_Attack_Normal);

	// Cancel 窗口表：技能可以取消【普攻】和【弹刀】。
	//   普攻命中后想接技能，必须能打断普攻后摇，否则"普攻 → 技能"这条连法根本不存在；
	//   弹反成功后想接技能同理（否则弹完只能干等着收势结束）。
	CancelAbilitiesWithTag.AddTag(DemoTag::Ability_Attack_Normal);
	CancelAbilitiesWithTag.AddTag(DemoTag::Ability_Defense_Parry);
}

void UGA_PlayerAttackSkill::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	Super::ActivateAbility(Handle, ActorInfo, ActivationInfo, TriggerEventData);

	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	ACharacter* Avatar = GetAvatarCharacter();
	if (!Avatar)
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	// 技能体力消耗：StaminaCost(25) 以前【只声明、从未施加】，放技能等于白嫖体力，
	// 体力条在放技能时纹丝不动 —— 这正是"体力像装饰品"的主因之一。这里把它真正接上。
	UDemoAbilitySystemLibrary::ApplyStaminaCost(Avatar, StaminaCost);

	if (ADemoCharacterBase* DemoCharacter = Cast<ADemoCharacterBase>(Avatar))
	{
		DemoCharacter->SetCurrentAttackParams(Damage, PoiseDamage, false, false);
	}

	if (!SkillMontage)
	{
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
		return;
	}

	UAbilityTask_PlayMontageAndWait* Task = UAbilityTask_PlayMontageAndWait::CreatePlayMontageAndWaitProxy(
		this, TEXT("SkillMontage"), SkillMontage, 1.f, NAME_None, true, 0.f);

	if (!Task)
	{
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
		return;
	}

	Task->OnCompleted.AddDynamic(this, &UGA_PlayerAttackSkill::OnMontageCompleted);
	Task->OnCancelled.AddDynamic(this, &UGA_PlayerAttackSkill::OnMontageCancelled);
	Task->OnInterrupted.AddDynamic(this, &UGA_PlayerAttackSkill::OnMontageCancelled);
	Task->ReadyForActivation();
}

void UGA_PlayerAttackSkill::OnMontageCompleted()
{
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
}

void UGA_PlayerAttackSkill::OnMontageCancelled()
{
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, true);
}

// ================================================================ 格挡
UGA_PlayerDefense::UGA_PlayerDefense()
{
	FGameplayTagContainer DefaultTags;
	DefaultTags.AddTag(DemoTag::Ability_Defense_Block);
	SetAssetTags(DefaultTags);
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;
}

void UGA_PlayerDefense::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	Super::ActivateAbility(Handle, ActorInfo, ActivationInfo, TriggerEventData);

	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	BlockingEffectHandle = ApplyEffectToSelf(UGE_BlockingEffect::StaticClass());
}

void UGA_PlayerDefense::EndAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	bool bReplicateEndAbility, bool bWasCancelled)
{
	if (BlockingEffectHandle.IsValid())
	{
		UDemoAbilitySystemLibrary::RemoveActiveEffect(GetAvatarCharacter(), BlockingEffectHandle);
		BlockingEffectHandle.Invalidate();
	}

	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}

// ================================================================ 弹刀（鼠标右键）
UGA_PlayerParry::UGA_PlayerParry()
{
	FGameplayTagContainer DefaultTags;
	DefaultTags.AddTag(DemoTag::Ability_Defense_Parry);
	SetAssetTags(DefaultTags);
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;

	// 弹刀期间不能重新起手攻击（否则会一边招架一边挥砍）
	BlockAbilitiesWithTag.AddTag(DemoTag::Ability_Attack_Normal);
	BlockAbilitiesWithTag.AddTag(DemoTag::Ability_Attack_Skill);

	// 但可以【取消】正在跑的攻击 —— 和闪避同理，弹刀也必须是随时可用的"取消键"。
	// 敌人起手了才有意义去按键，如果这时还被自己的普攻锁着，弹反就永远打不出来。
	CancelAbilitiesWithTag.AddTag(DemoTag::Ability_Attack_Normal);
	CancelAbilitiesWithTag.AddTag(DemoTag::Ability_Attack_Skill);
}

UAnimMontage* UGA_PlayerParry::ResolveMontage(const TObjectPtr<UAnimMontage>& Configured, const TCHAR* FallbackPath)
{
	if (Configured)
	{
		return Configured;
	}
	return FallbackPath ? LoadMontage(FallbackPath) : nullptr;
}

void UGA_PlayerParry::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	Super::ActivateAbility(Handle, ActorInfo, ActivationInfo, TriggerEventData);

	// 体力耗尽 → 举不起刃（软惩罚的一环）。
	// 注意是【拒绝这一次弹反】而不是把玩家硬控住：他照样能走位、能拉距离喘口气，
	// 只是这一下弹不了 —— 硬控会把"打空体力"直接变成等死。
	if (UAbilitySystemComponent* ASC = UDemoAbilitySystemLibrary::GetAbilitySystemComponent(GetAvatarCharacter()))
	{
		if (ASC->HasMatchingGameplayTag(DemoTag::State_StaminaExhausted))
		{
			EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
			return;
		}
	}

	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	Phase = EParryPhase::Enter;
	bWindowOpen = true;
	bParrySucceeded = false;
	// 真正起手了 → 武装空弹惩罚（结束时若没弹到东西就扣一点体力）
	bEmptyParryCostArmed = true;
	bLockedMoveByParry = false;

	// 弹刀期间锁住 WASD，避免"招架动作 + 跑步"叠加成滑步。
	//
	// ⚠⚠ 移动锁是【引用计数】（FightPlayerCharacter::LockMoveInput）。
	//   "加锁"和"解锁"必须严格一一配对，所以这里【无条件加锁】并记账 bLockedMoveByParry=true，
	//   结束时会无条件减一次。
	//   （旧写法是"当前没被锁才加锁、否则不记账"，那在 bool 时代是对的，
	//     换成计数后就会漏减/多减。）
	//   典型场景：普攻锁着(1) → 弹反取消普攻 → 弹反再锁(2) → 普攻 EndAbility 减(1) → 弹反结束减(0)。
	if (bLockMoveDuringParry)
	{
		if (AFightPlayerCharacter* Player = Cast<AFightPlayerCharacter>(GetAvatarCharacter()))
		{
			Player->LockMoveInput();
			bLockedMoveByParry = true;
		}
	}

	// 监听"连弹"事件：技能已经在跑的时候再按右键，控制器会补发这个事件。
	// OnlyTriggerOnce=false —— 一段连招里有几下，就要能重开几次窗。
	if (UAbilityTask_WaitGameplayEvent* Rewindow =
		UAbilityTask_WaitGameplayEvent::WaitGameplayEvent(this, DemoTag::Event_Ability_ParryRewindow, nullptr, false, false))
	{
		ParryRewindowListener = Rewindow;
		Rewindow->EventReceived.AddDynamic(this, &UGA_PlayerParry::OnRewindowEvent);
		Rewindow->ReadyForActivation();
	}

	// 起手动作：按下右键马上抬剑，玩家立刻有反馈（不用等判定结果）
	PlayParryMontage(
		ResolveMontage(ParryEnterMontage, TEXT("/Game/FightPlayer/Animation/Parry_Enter_Montage.Parry_Enter_Montage")),
		EParryPhase::Enter);

	// 开判定窗（挂窗口 GE + 挂成功监听 + 起窗口计时）
	OpenParryWindow();

	// 看门狗：到点还没结束就强制结束。
	// 弹反有窗口计时 / 收势计时 / 蒙太奇完成 / 连弹重开等多条收尾路径，
	// 任何一条失联都会让技能永久挂着 = 玩家失去操作。这条定时器独立兜底。
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(ParrySafetyTimerHandle, this,
			&UGA_PlayerParry::OnParrySafetyTimeout, ParrySafetyTimeout, /*bLoop=*/false);
	}

	UE_LOG(LogTemp, Log, TEXT("[Parry] 弹刀窗口开启 %.2fs（右键；窗内再按一次 = 连弹重开）"), WindowDuration);
}

void UGA_PlayerParry::OnParrySafetyTimeout()
{
	// 能走到这里说明正常的收尾路径某处失联了 —— 打 Warning 让人一眼看出是兜底生效，
	// 而不是误以为"弹反就是这个手感"。
	UE_LOG(LogTemp, Warning,
		TEXT("[Parry] ⚠看门狗触发：技能激活超过 %.1fs 仍未结束（正常收尾路径失联）→ 强制结束并解锁移动。")
		TEXT(" 窗口=%d 已弹到=%d 窗口计时=%d 收势计时=%d 蒙太奇=%d"),
		ParrySafetyTimeout, bWindowOpen ? 1 : 0, bParrySucceeded ? 1 : 0,
		ActiveWindowTask ? 1 : 0, SuccessLockTask ? 1 : 0, ActiveParryMontage ? 1 : 0);

	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
}

void UGA_PlayerParry::OpenParryWindow()
{
	bParrySucceeded = false;
	bWindowOpen = true;
	Phase = EParryPhase::Enter;

	// 先把上一轮的窗口 GE 收掉：不然连弹时 tag 会叠层，
	// 技能结束只摘掉最后一次的 handle，前面几次的 State.Parrying 会永远留在身上。
	CloseWindowEffects();

	// 判定窗分两段：
	//   State.Parrying            全程 0.30s，这段时间内被打到就能弹开
	//   State.PerfectParryWindow  只有开场 0.12s —— 这段时间内弹到算【完美弹反】
	// 窄窗 + 高回报才有"练出来"的曲线；只有一个宽窗的话连按右键就能一直弹，没有成就感。
	ParryWindowHandle = ApplyEffectToSelf(UGE_ParryWindowEffect::StaticClass());
	PerfectParryWindowHandle = ApplyEffectToSelf(UGE_PerfectParryWindowEffect::StaticClass());

	// 成功监听：OnlyTriggerOnce=true，所以【每开一次窗都要重新挂】。
	// 这正是连弹的关键 —— 第一次弹到会把监听消耗掉，不重挂的话第二下即使弹到了
	// 也不会切阶段 / 不会收窗，玩家会觉得"第二下弹了但没反应"。
	if (ParrySuccessListener)
	{
		ParrySuccessListener->EventReceived.RemoveDynamic(this, &UGA_PlayerParry::OnParrySuccessEvent);
		ParrySuccessListener->EndTask();
		ParrySuccessListener = nullptr;
	}
	if (UAbilityTask_WaitGameplayEvent* WaitEvent =
		UAbilityTask_WaitGameplayEvent::WaitGameplayEvent(this, DemoTag::Event_Parry_Success, nullptr, true, false))
	{
		ParrySuccessListener = WaitEvent;
		WaitEvent->EventReceived.AddDynamic(this, &UGA_PlayerParry::OnParrySuccessEvent);
		WaitEvent->ReadyForActivation();
	}

	// 窗口计时：旧的先停掉（连弹时上一轮的计时器还挂着，不停会出现
	// "新窗刚开就被旧计时器关掉"）
	if (ActiveWindowTask)
	{
		ActiveWindowTask->OnFinish.RemoveDynamic(this, &UGA_PlayerParry::OnWindowFinished);
		ActiveWindowTask->EndTask();
		ActiveWindowTask = nullptr;
	}
	if (UAbilityTask_WaitDelay* DelayTask = UAbilityTask_WaitDelay::WaitDelay(this, FMath::Max(0.02f, WindowDuration)))
	{
		ActiveWindowTask = DelayTask;
		DelayTask->OnFinish.AddDynamic(this, &UGA_PlayerParry::OnWindowFinished);
		DelayTask->ReadyForActivation();
	}
	else
	{
		OnWindowFinished();
	}

	// 连弹会取消掉上一发的收势计时（否则技能刚重开窗就被收掉）
	if (SuccessLockTask)
	{
		SuccessLockTask->OnFinish.RemoveDynamic(this, &UGA_PlayerParry::OnSuccessLockFinished);
		SuccessLockTask->EndTask();
		SuccessLockTask = nullptr;
	}
}

void UGA_PlayerParry::CloseWindowEffects()
{
	if (ParryWindowHandle.IsValid())
	{
		UDemoAbilitySystemLibrary::RemoveActiveEffect(GetAvatarCharacter(), ParryWindowHandle);
		ParryWindowHandle.Invalidate();
	}
	if (PerfectParryWindowHandle.IsValid())
	{
		UDemoAbilitySystemLibrary::RemoveActiveEffect(GetAvatarCharacter(), PerfectParryWindowHandle);
		PerfectParryWindowHandle.Invalidate();
	}
}

void UGA_PlayerParry::OnRewindowEvent(FGameplayEventData Payload)
{
	// 连弹：只重开判定窗。
	// 不重播起手动作（动作会抖），不动移动锁（本来就没解锁），
	// 也不重置 bLockedMoveByParry —— 那是 EndAbility 的账。
	OpenParryWindow();

	UE_LOG(LogTemp, Log, TEXT("[Parry] ★连弹★ 判定窗重开 %.2fs"), WindowDuration);
}

void UGA_PlayerParry::OnSuccessLockFinished()
{
	// ⚠⚠⚠ 这里【不能】因为"还有蒙太奇在播"就直接 return（2026-10-03 决定性修复）
	//
	// 实测症状：弹反成功后角色【愣住 4.0 秒】，日志显示
	//   「看门狗触发：技能激活超过 4.0s 仍未结束 … 窗口=0 已弹到=1 窗口计时=0 收势计时=1 蒙太奇=0」
	//   同时 [Fight] 尝试激活 GA_NormalAttack → 失败（标签阻挡/已在激活中）
	// 也就是说：收势计时已经跑完，但技能没结束 → 普攻被 Parry 的
	// BlockAbilitiesWithTag 挡住 → 玩家按什么都没反应 = "愣神"。
	//
	// 旧代码写成 `if (bWindowOpen || ActiveParryMontage) return;`，
	// 只要还有【任何】蒙太奇在播就永不结束。而 Parry_Hold_Montage 是
	// 【持剑架势】——它的时长可能很长、或被后续事件重开，
	// 于是收势计时一遍遍白跑，技能无限期挂着。
	//
	// 正确语义：收势计时是【兜底收尾】，到点就该结束。
	// 若真的还想继续（连弹），玩家会重新按右键 → 走 OnRewindowEvent 重开窗口，
	// 那条路径会自己重建收势计时，不需要这里等蒙太奇。
	// 唯一保留的守卫是"判定窗还开着"（说明玩家正在连弹）。
	if (bWindowOpen)
	{
		return;
	}

	// 有蒙太奇在播也不等了：先把技能收掉（会顺带停掉蒙太奇），
	// 保证 BlockAbilitiesWithTag / 移动锁立刻释放，玩家马上能操作。
	StopParryMontage();
	ActiveParryMontage = nullptr;

	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
}

void UGA_PlayerParry::OnParrySuccessEvent(FGameplayEventData Payload)
{
	if (bParrySucceeded)
	{
		return;
	}
	bParrySucceeded = true;
	// 弹到了 → 解除空弹惩罚（这一下是有效操作，不该被罚）
	bEmptyParryCostArmed = false;

	// 弹到了 → 立刻关掉判定窗：否则同一瞬间的第二下攻击还会被"弹"一次（白嫖无敌帧）。
	// ⚠ 必须【在】ApplyHit 判定完之后才收 —— ApplyHit 正是靠 State.PerfectParryWindow
	//   才知道刚才那一下算不算完美，而这个事件就是从那里发出来的，走到这里判定早已完成。
	CloseWindowEffects();

	// ⚠⚠ 必须在这里把 bWindowOpen 一起关掉，并撤掉窗口计时器 —— 否则本技能【永远结束不了】：
	//   · 收势计时 OnSuccessLockFinished 看到 bWindowOpen==true 会直接 return
	//     （它以为玩家又按了一次右键重开了窗，要交给新窗收尾）
	//   · 窗口计时 OnWindowFinished 看到 bParrySucceeded==true 也直接 return
	//   → 两条收尾路径互相让路 → EndAbility 永不执行 →
	//     移动锁不释放 + 攻击被本技能的 BlockAbilities 挡住 = 玩家"弹反完就卡住"（2026-09-29 修复）
	bWindowOpen = false;
	if (ActiveWindowTask)
	{
		ActiveWindowTask->OnFinish.RemoveDynamic(this, &UGA_PlayerParry::OnWindowFinished);
		ActiveWindowTask->EndTask();
		ActiveWindowTask = nullptr;
	}

	UE_LOG(LogTemp, Log, TEXT("[Parry] 弹刀成功（%.2fs 内再按右键可立刻重开窗 → 连弹）"),
		DemoBalance::ParrySuccessLockTime);

	// 只狼式：弹到就是弹到，不播额外动作。
	// 弹完立刻能按下一刀去弹连招的第二段，节奏才是连着的。
	// 想看"弹开之后反手打回去"的表演就把 bPlayCounterOnParry 勾上（代价是节奏变拖）。
	if (bPlayCounterOnParry)
	{
		if (UAnimMontage* Counter = ResolveMontage(ParryMontage,
			TEXT("/Game/FightPlayer/Animation/Parry_Montage.Parry_Montage")))
		{
			PlayParryMontage(Counter, EParryPhase::Success);
		}
	}

	// 收势计时：到点收技能（玩家在这期间再按右键会重开窗并取消这个计时）。
	// 不这么做的话技能会一直挂在激活状态 —— 虽然连弹能重开窗，
	// 但"没按第二下"的时候玩家会处于"招架姿势"下不来，手感会发黏。
	if (SuccessLockTask)
	{
		SuccessLockTask->OnFinish.RemoveDynamic(this, &UGA_PlayerParry::OnSuccessLockFinished);
		SuccessLockTask->EndTask();
		SuccessLockTask = nullptr;
	}
	if (UAbilityTask_WaitDelay* Lock = UAbilityTask_WaitDelay::WaitDelay(
		this, FMath::Max(0.02f, DemoBalance::ParrySuccessLockTime)))
	{
		SuccessLockTask = Lock;
		Lock->OnFinish.AddDynamic(this, &UGA_PlayerParry::OnSuccessLockFinished);
		Lock->ReadyForActivation();
	}
	else
	{
		OnSuccessLockFinished();
	}
}

void UGA_PlayerParry::OnWindowFinished()
{
	bWindowOpen = false;

	if (bParrySucceeded)
	{
		// 已经弹到了：正常由收势计时（SuccessLockTask）收尾。
		// 双保险：计时器因为某种原因没建起来 / 已经跑完被清掉了，
		// 这里不能把技能挂死（挂死 = 移动锁不解 + 攻击被挡 = 卡住），直接结束。
		if (!SuccessLockTask)
		{
			EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
		}
		return;
	}

	// 没弹到 → 播放收招动作（有动作就播完再结束，没动作就直接结束）
	if (UAnimMontage* Exit = ResolveMontage(ParryExitMontage,
		TEXT("/Game/FightPlayer/Animation/Parry_Exit_Montage.Parry_Exit_Montage")))
	{
		PlayParryMontage(Exit, EParryPhase::Exit);
		return;
	}

	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
}

void UGA_PlayerParry::PlayParryMontage(UAnimMontage* Montage, EParryPhase InPhase)
{
	Phase = InPhase;

	if (!Montage)
	{
		return;
	}

	// 先停掉上一个动作（解绑后再 EndTask），否则旧任务的延迟回调会串到新阶段
	StopParryMontage();

	// 第 7 个参数是 AnimRootMotionTranslationScale：招架动作都在 In-Place 目录（本来就无根位移），
	// 保持 1.0 即可，不会有位移被拉回的问题。
	UAbilityTask_PlayMontageAndWait* Task = UAbilityTask_PlayMontageAndWait::CreatePlayMontageAndWaitProxy(
		this, TEXT("ParryMontage"), Montage, 1.f, NAME_None, true, 1.f);
	if (!Task)
	{
		return;
	}

	ActiveParryMontage = Task;
	Task->OnCompleted.AddDynamic(this, &UGA_PlayerParry::OnParryMontageFinished);
	Task->OnCancelled.AddDynamic(this, &UGA_PlayerParry::OnParryMontageCancelled);
	Task->OnInterrupted.AddDynamic(this, &UGA_PlayerParry::OnParryMontageCancelled);
	Task->ReadyForActivation();
}

void UGA_PlayerParry::UnbindParryMontage()
{
	if (!ActiveParryMontage)
	{
		return;
	}

	ActiveParryMontage->OnCompleted.RemoveDynamic(this, &UGA_PlayerParry::OnParryMontageFinished);
	ActiveParryMontage->OnCancelled.RemoveDynamic(this, &UGA_PlayerParry::OnParryMontageCancelled);
	ActiveParryMontage->OnInterrupted.RemoveDynamic(this, &UGA_PlayerParry::OnParryMontageCancelled);
	ActiveParryMontage = nullptr;
}

void UGA_PlayerParry::StopParryMontage()
{
	if (!ActiveParryMontage)
	{
		return;
	}

	UAbilityTask_PlayMontageAndWait* Task = ActiveParryMontage;
	// 先解绑再 EndTask：EndTask 会中止蒙太奇，回调若还挂着可能延迟触发，
	// 在下一个动作已经开播后才误把技能结束掉（普攻连段踩过同一个坑）。
	UnbindParryMontage();
	Task->EndTask();
}

void UGA_PlayerParry::OnParryMontageFinished()
{
	ActiveParryMontage = nullptr;

	if (Phase == EParryPhase::Enter)
	{
		// 起手播完、判定窗还开着 → 接“持剑架势”（没配就静候判定窗结束）
		if (bWindowOpen && !bParrySucceeded)
		{
			if (UAnimMontage* Hold = ResolveMontage(ParryHoldMontage,
				TEXT("/Game/FightPlayer/Animation/Parry_Hold_Montage.Parry_Hold_Montage")))
			{
				PlayParryMontage(Hold, EParryPhase::Hold);
			}
		}
		return;
	}

	if (Phase == EParryPhase::Hold)
	{
		// 架势播完，静候判定窗结束 / 弹反成功
		return;
	}

	// Success / Exit 播完 → 本次弹刀结束。
	// ⚠ 但"连弹重开的判定窗还活着"时不能结束：EndAbility 会把窗口 GE 一起摘掉，
	//   玩家刚重开的第二下就白按了。这时交给 OnWindowFinished / 收势计时去收尾。
	if (bWindowOpen)
	{
		return;
	}

	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
}

void UGA_PlayerParry::OnParryMontageCancelled()
{
	ActiveParryMontage = nullptr;

	// 只有“收尾动作”（弹刀反击 / 收招）被打断才结束技能；
	// 起手/架势被新动作顶掉时走的是 StopParryMontage（回调已解绑），不会误触这里。
	// 连弹期间判定窗还开着的话同样不能结束（理由同 OnParryMontageFinished）。
	if ((Phase == EParryPhase::Success || Phase == EParryPhase::Exit) && !bWindowOpen)
	{
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, true);
	}
}

void UGA_PlayerParry::EndAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	bool bReplicateEndAbility, bool bWasCancelled)
{
	// 蒙太奇交给 bStopWhenAbilityEnds 停，这里只需解绑回调防止回调反过来再调 EndAbility
	UnbindParryMontage();

	// 看门狗收工：正常结束路径已经走到，别让它在几秒后再触发一次多余的 EndAbility
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ParrySafetyTimerHandle);
	}

	// 空弹惩罚：举了刃起手、但全程没弹到任何东西 → 扣一点体力。
	// 目的是压掉"无脑长按右键、把弹反当常驻护盾"这个保底策略 ——
	// 弹反应当是读招之后的主动选择，而不是随时能白嫖的保险。
	if (bEmptyParryCostArmed)
	{
		bEmptyParryCostArmed = false;
		UDemoAbilitySystemLibrary::ApplyStaminaCost(
			GetAvatarCharacter(), DemoBalance::PlayerEmptyParryStaminaCost);
	}

	if (ParrySuccessListener)
	{
		ParrySuccessListener->EventReceived.RemoveDynamic(this, &UGA_PlayerParry::OnParrySuccessEvent);
		ParrySuccessListener->EndTask();
		ParrySuccessListener = nullptr;
	}

	// 连弹监听 + 两个计时任务也要收干净：
	// 技能结束时它们还挂着的话，下一轮起手会拿到两套计时器互相打架
	//（典型症状是"刚起手窗就关了"）。
	if (ParryRewindowListener)
	{
		ParryRewindowListener->EventReceived.RemoveDynamic(this, &UGA_PlayerParry::OnRewindowEvent);
		ParryRewindowListener->EndTask();
		ParryRewindowListener = nullptr;
	}
	if (ActiveWindowTask)
	{
		ActiveWindowTask->OnFinish.RemoveDynamic(this, &UGA_PlayerParry::OnWindowFinished);
		ActiveWindowTask->EndTask();
		ActiveWindowTask = nullptr;
	}
	if (SuccessLockTask)
	{
		SuccessLockTask->OnFinish.RemoveDynamic(this, &UGA_PlayerParry::OnSuccessLockFinished);
		SuccessLockTask->EndTask();
		SuccessLockTask = nullptr;
	}

	// 判定窗还没过期 → 主动移除（技能结束了就不该还留着“可弹反”状态）
	CloseWindowEffects();

	// 只释放本技能加的那次移动锁，并还原“玩家此刻想要的速度”（按着 Shift 就是跑速）
	if (bLockedMoveByParry)
	{
		if (AFightPlayerCharacter* Player = Cast<AFightPlayerCharacter>(GetAvatarCharacter()))
		{
			// ⚠ 引用计数配对减一次。若此时计数仍 >0（普攻/闪避还锁着），
			//   就【不要】顺手把 MaxWalkSpeed 改回去 —— 那些技能自己会还原。
			//   这里只有"锁已经全部解除"才恢复移速，避免和别的技能抢着改同一个值。
			Player->UnlockMoveInput();
			if (!Player->IsMoveInputLocked())
			{
				if (UCharacterMovementComponent* MoveComp = Player->GetCharacterMovement())
				{
					MoveComp->MaxWalkSpeed = Player->GetDesiredMaxWalkSpeed();
				}
			}
		}
		bLockedMoveByParry = false;
	}

	bWindowOpen = false;

	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}

// ================================================================ 闪避
UGA_PlayerDodge::UGA_PlayerDodge()
{
	FGameplayTagContainer DefaultTags;
	DefaultTags.AddTag(DemoTag::Ability_Movement_Dodge);
	SetAssetTags(DefaultTags);

	CooldownGameplayEffectClass = UGE_DodgeCooldownEffect::StaticClass();

	// 闪避期间不能再出招（否则会在闪避位移里插一段挥砍，方向全乱）
	BlockAbilitiesWithTag.AddTag(DemoTag::Ability_Attack_Normal);
	BlockAbilitiesWithTag.AddTag(DemoTag::Ability_Attack_Skill);
	// 喝酒（道具）期间也不许闪：闪避是逃脱手段，喝血是"committed"动作，二者互斥，
	// 避免"边喝边闪"把两套位移/锁输入搅在一起。
	BlockAbilitiesWithTag.AddTag(DemoTag::Ability_UseItem_Heal);

	// ---------------- 闪避取消（这是"手感"的一半）----------------
	// BlockAbilitiesWithTag 只负责"闪避期间不许重新起手"；
	// 真正把正在跑的普攻/技能【打断】的是 CancelAbilitiesWithTag。
	//
	// 以前只写了 Block 没写 Cancel，导致：普攻挥到一半按 Shift，闪避确实位移出去了，
	// 但普攻技能还在后台跑（锁着 WASD、占着 State.Attacking），
	// 表现就是"闪了但动作没断" —— 玩家感觉按键没被响应。
	// 补上 Cancel 之后，闪避键等于一个随时可用的"取消"，才有动作游戏的节奏感。
	//
	// Cancel 窗口表补全：闪避也能取消【弹刀】与【喝血】。
	// 弹反失手 / 弹完想脱离时，玩家必须能立刻跑 —— 弹刀不是单向门；
	// 喝血中途发现 BOSS 要出招，也得能立刻闪出去，不能卡在喝酒动作里当活靶。
	CancelAbilitiesWithTag.AddTag(DemoTag::Ability_Attack_Normal);
	CancelAbilitiesWithTag.AddTag(DemoTag::Ability_Attack_Skill);
	CancelAbilitiesWithTag.AddTag(DemoTag::Ability_Defense_Parry);
	CancelAbilitiesWithTag.AddTag(DemoTag::Ability_UseItem_Heal);
}

void UGA_PlayerDodge::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	Super::ActivateAbility(Handle, ActorInfo, ActivationInfo, TriggerEventData);

	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	ApplyEffectToSelf(UGE_DodgeCostEffect::StaticClass());

	// ---------------- 低体力闪避降级判定 ----------------
	// 体力不足时这一次闪避会"扑短"：无敌帧变短、位移变近。
	// 这是"体力真正参与决策"的落点之一 —— 玩家得在"再闪一次保命"和
	// "留点体力打反击"之间取舍，而不是永远无脑 Shift。
	bool bWeakDodge = false;
	if (UAbilitySystemComponent* ASC = UDemoAbilitySystemLibrary::GetAbilitySystemComponent(GetAvatarCharacter()))
	{
		const float MaxStamina = ASC->GetNumericAttribute(UDemoAttributeSet::GetMaxStaminaAttribute());
		const float CurStamina = ASC->GetNumericAttribute(UDemoAttributeSet::GetStaminaAttribute());
		bWeakDodge = MaxStamina > KINDA_SMALL_NUMBER
			&& (CurStamina / MaxStamina) < DemoBalance::StaminaDodgeWeakRatio;
	}

	// 连点闪避判定：距上次闪避【结束】太近 → 这一次扑短。
	// 闪避技能结束前不能再激活，所以"连点"实际表现为两次闪避首尾相接（间隔≈0），
	// 判据用"上次结束 → 这次开始"正好能把它和"隔一会儿再闪"区分开。
	bool bSpamDodge = false;
	if (UWorld* World = GetWorld())
	{
		bSpamDodge = (World->GetTimeSeconds() - LastDodgeEndRealTime) < DemoBalance::DodgeSpamWindow;
	}

	// ---------------- 无敌帧 + 完美闪避判定窗 ----------------
	//
	// 这两条是"闪避有没有交互感"的分水岭：
	//   UGE_InvincibleEffect        → State.Invincible，全程免伤（技能结束时主动移除）
	//   UGE_PerfectDodgeWindowEffect → State.PerfectDodgeWindow，只活 0.22s
	// ApplyHit 先看有没有无敌帧（有 = 这一下打不中），
	// 再看完美窗在不在（在 = ★完美闪避★，给子弹时间 + 回耐力）。
	//
	// 以前这里什么都没有 —— 闪避只是一个"位移动作"，被攻击照样全额吃伤，
	// 所以玩家按不按 Shift 手感上没区别，自然就"没有交互感"。
	if (bGrantInvincible)
	{
		// 低体力 → 用短无敌帧那条 GE（不改成动态时长，避免影响正常闪避的节奏）
		DodgeInvincibleHandle = ApplyEffectToSelf(
			bWeakDodge ? UGE_InvincibleWeakEffect::StaticClass() : UGE_InvincibleEffect::StaticClass());
		PerfectDodgeWindowHandle = ApplyEffectToSelf(UGE_PerfectDodgeWindowEffect::StaticClass());

		UE_LOG(LogTemp, Log,
			TEXT("[Dodge] 无敌帧开启：全程 %.2fs 免伤，其中前 %.2fs 为完美闪避窗（窗内被打到触发子弹时间）"),
			DemoBalance::DodgeInvincibleDuration, DemoBalance::PerfectDodgeWindow);
	}

	ACharacter* Avatar = GetAvatarCharacter();
	if (!Avatar)
	{
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, true);
		return;
	}

	// ------------------------------------------------------------------
	// 1) 方向：只有“方向键真的按着”时才采用，松手后残留的旧方向一律不算
	//    （GetMoveInputOrZero 内部带 0.25s 时效判断）
	//
	//    参考系用【镜头（控制器）朝向】，而不是角色朝向：
	//    HandleMove 里的 LastMoveInput 就是按“镜头 yaw”换算出来的世界方向
	//    （Forward*Axis.Y + Right*Axis.X），用同一个 yaw 反算回去才严格得到
	//    (X = Axis.Y = 前, Y = Axis.X = 右) —— 与 WASD 的移动方向 100% 一致。
	//    用角色 yaw 反算会被“角色转身的瞬时滞后”带偏（横移模式下角色是插值转向的）。
	// ------------------------------------------------------------------
	FVector WorldDir = FVector::ZeroVector;
	if (ADemoCharacterBase* DemoCharacter = Cast<ADemoCharacterBase>(Avatar))
	{
		WorldDir = DemoCharacter->GetMoveInputOrZero(0.25f);
	}

	FRotator RefRotation = Avatar->GetActorRotation();
	RefRotation.Pitch = 0.f;
	RefRotation.Roll = 0.f;
	if (const APlayerController* PC = Cast<APlayerController>(Avatar->GetController()))
	{
		RefRotation.Yaw = PC->GetControlRotation().Yaw;
	}

	// 换算到“镜头坐标系”：+X=前（远离镜头）/ -X=后 / -Y=左 / +Y=右
	FVector LocalDir = RefRotation.UnrotateVector(WorldDir);
	LocalDir.Z = 0.f;
	if (LocalDir.IsNearlyZero())
	{
		LocalDir = FVector(1.f, 0.f, 0.f);   // 没按方向键 → 默认向前闪
	}
	LocalDir = LocalDir.GetSafeNormal2D();
	PendingLocalDir = LocalDir;              // 存起来，交给 PickDodgeMontage 选对应方向的动画

	// ------------------------------------------------------------------
	// 2) 闪避辅助：压制动画自带根位移 + 锁 WASD + 镜头与角色刚性同步
	//    闪避动画（Dodge_*）本身就是带位移的 Root-Motion 动画，若让它和代码给的位移
	//    同时生效，距离会变长、方向还会被动画带偏（这正是“按 W 却向右闪”的另一半成因）。
	//    压制后动画原地播，位移 100% 由 DodgeDistance 决定 —— 是真正的胶囊体位移，不会回弹。
	// ------------------------------------------------------------------
	if (AFightPlayerCharacter* Player = Cast<AFightPlayerCharacter>(Avatar))
	{
		// 压制动画根位移（动画原地播）+ 记录起点 / 镜头与角色刚性同步。
		// 蒙太奇实例级的硬屏蔽由角色的 Tick 每帧补挂（SetIgnoreAnimRootMotion 会打开那个开关）。
		Player->SetIgnoreAnimRootMotion(true);
		Player->BeginDisplacementAssist();

		// ⚠ 引用计数：闪避期间无条件加锁，EndAbility 里配对减一次（见 RestoreMovement）。
		if (bLockMoveDuringDodge)
		{
			Player->LockMoveInput();
			bLockedMoveByDodge = true;
		}
	}

	// ------------------------------------------------------------------
	// 3) 位移：匀速定距（速度 = DodgeDistance / DodgeDuration），
	//    方向 = 上面的本地方向原样转回世界 —— 和玩家按的键、和选中的动画三者严格一致。
	//    用 ApplyRootMotionConstantForce 而不是 LaunchCharacter 给初速度：
	//    初速度的实际距离会被地面摩擦力吃掉一截，还会在动作播完后继续“溜”，既不好估也不好调。
	// ------------------------------------------------------------------
	PendingWorldDir = RefRotation.RotateVector(LocalDir).GetSafeNormal2D();

	const float Duration = FMath::Max(DodgeDuration, 0.05f);

	// 实际生效的距离：夹在 [0, DodgeDistanceMax] 内。
	// 蓝图 CDO 里若残留着旧版本存的距离值（会盖掉 C++ 默认值），会被这道夹子收住 ——
	// 保证"数值调得下去"，不会出现"改了默认值但闪避距离一点没变"。
	const float BaseDistance = FMath::Clamp(DodgeDistance, 0.f, FMath::Max(DodgeDistanceMax, 1.f));

	// 两个"缩短"来源叠乘（位移是匀速定距走完的，所以这里改的就是真实位移量）：
	//   · 低体力（0.7）：没力气了，扑不远
	//   · 连点闪避（0.6）：上一跳还没站稳就又扑，自然扑短
	float DistanceScale = 1.f;
	if (bWeakDodge)
	{
		DistanceScale *= DemoBalance::DodgeWeakDistanceScale;
	}
	if (bSpamDodge)
	{
		DistanceScale *= DemoBalance::DodgeSpamDistanceScale;
	}
	const float Distance = BaseDistance * DistanceScale;
	PendingDistance = Distance;
	if (!FMath::IsNearlyEqual(Distance, DodgeDistance))
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Dodge] DodgeDistance=%.0f 被上限夹到 %.0f。"
				 "（上限 = GA_Dodge 的 Demo|Dodge|DodgeDistanceMax；蓝图 CDO 里可能存着旧值，"
				 "跑一次 Tools/FixDodgeDirection.py 或手动改 Class Defaults 即可消除这条警告）"),
			DodgeDistance, Distance);
	}

	float Speed = 0.f;

	if (bApplyLaunchImpulse)
	{
		if (Distance > 0.f)
		{
			Speed = Distance / Duration;
			DodgeForceTask = UAbilityTask_ApplyRootMotionConstantForce::ApplyRootMotionConstantForce(
				this,
				TEXT("DodgeDash"),
				PendingWorldDir,
				Speed,
				Duration,
				/*bIsAdditive=*/false,
				/*StrengthOverTime=*/nullptr,
				// 位移结束时把速度直接钳到 0：动作收招期间不会继续滑出去
				ERootMotionFinishVelocityMode::ClampVelocity,
				FVector::ZeroVector,
				/*ClampVelocityOnFinish=*/0.f,
				/*bEnableGravity=*/true);

			if (DodgeForceTask)
			{
				DodgeForceTask->ReadyForActivation();
			}
		}
		else
		{
			// 兜底通道：DodgeDistance 没配（<=0）时，退回“给初速度”的老做法
			Avatar->LaunchCharacter(PendingWorldDir * DodgeImpulse, true, true);
			UE_LOG(LogTemp, Warning,
				TEXT("[Dodge] DodgeDistance<=0，退回初速度模式：方向=(%.2f, %.2f) 力度=%.0f（距离受摩擦力影响、不易预估）"),
				PendingWorldDir.X, PendingWorldDir.Y, DodgeImpulse);
		}
	}

	UAnimMontage* Montage = PickDodgeMontage();
	if (!Montage)
	{
		UE_LOG(LogTemp, Error, TEXT("[Dodge] 闪避蒙太奇缺失，无法播放（DirectionalMontages/DodgeMontage 未配且默认路径也取不到）"));
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
		return;
	}

	if (Montage->GetPlayLength() <= 0.f)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Dodge] 闪避蒙太奇 %s 播放时长为 0（空壳资产，请用 SwordAnimsetPro 的 Dodge_F/B/L/R_Anim 重建）"),
			*GetNameSafe(Montage));
	}

	UE_LOG(LogTemp, Log,
		TEXT("[Dodge] 本地方向=(%.2f, %.2f)（+X=前/-X=后/-Y=左/+Y=右；没按键时取 +X）"
			 " 世界方向=(%.2f, %.2f) 动画=%s HasRootMotion=%d 目标距离=%.0fcm 时长=%.2fs 速度=%.0fcm/s"),
		LocalDir.X, LocalDir.Y, PendingWorldDir.X, PendingWorldDir.Y,
		*GetNameSafe(Montage), Montage->HasRootMotion() ? 1 : 0, Distance, Duration, Speed);

	UAbilityTask_PlayMontageAndWait* Task = UAbilityTask_PlayMontageAndWait::CreatePlayMontageAndWaitProxy(
		this, TEXT("DodgeMontage"), Montage, 1.f, NAME_None, true, 0.f);  // 末尾 0.f = StartTimeSeconds，与根运动无关

	if (!Task)
	{
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
		return;
	}

	Task->OnCompleted.AddDynamic(this, &UGA_PlayerDodge::OnMontageCompleted);
	Task->OnCancelled.AddDynamic(this, &UGA_PlayerDodge::OnMontageCancelled);
	Task->OnInterrupted.AddDynamic(this, &UGA_PlayerDodge::OnMontageCancelled);
	Task->ReadyForActivation();

	// 蒙太奇这一帧才开始播 → 立刻把它【自己】的根位移也关掉（蒙太奇实例级开关，
	// 比 AnimInstance 的 Root Motion Mode 更上游，ABP 也绕不过）。
	// 上面 SetIgnoreAnimRootMotion(true) 已经开了全局屏蔽 + 角色的 Tick 每帧补挂实例级屏蔽，
	// 动画就别想再插手位移了。
	if (AFightPlayerCharacter* Player = Cast<AFightPlayerCharacter>(Avatar))
	{
		Player->SuppressMontageRootMotion(Montage);
	}
}

UAnimMontage* UGA_PlayerDodge::PickDodgeMontage() const
{
	const EDodgeSlot Slot = QuantizeLocalDir(PendingLocalDir);
	static const TCHAR* SlotNames[] = { TEXT("前"), TEXT("后"), TEXT("左"), TEXT("右") };

	bool bByName = false;
	UAnimMontage* Picked = nullptr;

	// 1) 优先用 BP 里配好的 4 段方向蒙太奇。
	//    顺序约定是 [前, 后, 左, 右]，但实际以【资产名】为准（Dodge_F/B/L/R）——
	//    这样即使数组被写反，也不会再出现“按 W 向右闪”。
	if (DirectionalMontages.Num() >= 4 &&
		DirectionalMontages[0] && DirectionalMontages[1] && DirectionalMontages[2] && DirectionalMontages[3])
	{
		Picked = PickDodgeMontageBySlot(DirectionalMontages, Slot, bByName);
	}

	// 2) 兜底：从默认路径加载（[前, 后, 左, 右]），保证没配也能闪
	if (!Picked)
	{
		static TArray<UAnimMontage*> Defaults = []()
		{
			TArray<UAnimMontage*> Arr;
			const TCHAR* Paths[] = {
				TEXT("/Game/FightPlayer/Animation/Dodge_F_Montage.Dodge_F_Montage"),
				TEXT("/Game/FightPlayer/Animation/Dodge_B_Montage.Dodge_B_Montage"),
				TEXT("/Game/FightPlayer/Animation/Dodge_L_Montage.Dodge_L_Montage"),
				TEXT("/Game/FightPlayer/Animation/Dodge_R_Montage.Dodge_R_Montage"),
			};
			for (const TCHAR* P : Paths) { Arr.Add(LoadMontage(P)); }
			return Arr;
		}();

		if (Defaults.Num() == 4 && Defaults[0] && Defaults[1] && Defaults[2] && Defaults[3])
		{
			Picked = PickDodgeMontageBySlot(Defaults, Slot, bByName);
		}
	}

	// 3) 最后兜底：单个 DodgeMontage（或默认路径的 Dodge_F）
	if (!Picked)
	{
		// 注意：不能写 `DodgeMontage ? DodgeMontage : LoadMontage(...)` ——
		// TObjectPtr 与裸指针混用会让条件表达式类型有歧义（C2445）。
		if (DodgeMontage)
		{
			Picked = DodgeMontage;
		}
		else
		{
			Picked = LoadMontage(TEXT("/Game/FightPlayer/Animation/Dodge_F_Montage.Dodge_F_Montage"));
		}
		bByName = false;
	}

	UE_LOG(LogTemp, Log, TEXT("[Dodge] 方向=%s → 选用动画 %s（%s）"),
		SlotNames[static_cast<int32>(Slot)],
		*GetNameSafe(Picked),
		bByName ? TEXT("按资产名匹配") : TEXT("按数组下标顺序"));

	return Picked;
}

void UGA_PlayerDodge::OnMontageCompleted()
{
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
}

void UGA_PlayerDodge::OnMontageCancelled()
{
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, true);
}

void UGA_PlayerDodge::EndAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	bool bReplicateEndAbility, bool bWasCancelled)
{
	// 记下本次闪避结束时刻：下一次闪避用它判断是不是"连点扑"（见 ActivateAbility 的 bSpamDodge）
	if (UWorld* World = GetWorld())
	{
		LastDodgeEndRealTime = World->GetTimeSeconds();
	}

	// 先停掉定距位移任务（否则它会继续把角色推完整个 Duration）
	if (DodgeForceTask)
	{
		DodgeForceTask->EndTask();
		DodgeForceTask = nullptr;
	}

	// 无敌帧 / 完美窗在闪避结束的这一刻立刻撤掉。
	//
	// 为什么不靠 GE 自己的时长自然过期：闪避可能被更早地打断
	// （死亡、硬直、被别的技能取消）。让人已经站定了还挂着无敌，
	// 会变成"按住 Shift 就能无限免伤"的漏洞。
	if (DodgeInvincibleHandle.IsValid())
	{
		UDemoAbilitySystemLibrary::RemoveActiveEffect(GetAvatarCharacter(), DodgeInvincibleHandle);
		DodgeInvincibleHandle.Invalidate();
	}
	if (PerfectDodgeWindowHandle.IsValid())
	{
		UDemoAbilitySystemLibrary::RemoveActiveEffect(GetAvatarCharacter(), PerfectDodgeWindowHandle);
		PerfectDodgeWindowHandle.Invalidate();
	}

	float ActualDistance = 0.f;

	if (AFightPlayerCharacter* Player = Cast<AFightPlayerCharacter>(GetAvatarCharacter()))
	{
		// 结束闪避辅助：量出本次真实位移 + 还原镜头跟随。
		// 然后把动画根位移还给动画 —— 攻击等其它技能走的是“真实根位移”通道。
		ActualDistance = Player->EndDisplacementAssist();
		Player->RestoreMontageRootMotion();
		Player->SetIgnoreAnimRootMotion(false);

		if (bLockedMoveByDodge)
		{
			bLockedMoveByDodge = false;
			// ⚠ 引用计数配对减一次（不是"直接解锁"）——
			//   闪避常常紧跟在普攻/弹反之后，计数必须逐层归零。
			Player->UnlockMoveInput();

			// 解锁后把移速设成“当前应有值”（避免沿用被压过的旧值）
			if (UCharacterMovementComponent* MoveComp = Player->GetCharacterMovement())
			{
				MoveComp->MaxWalkSpeed = Player->GetDesiredMaxWalkSpeed();
			}
		}
	}

	// 削掉闪避残余速度：否则动作播完后角色还会继续“溜”一段，距离显得更远
	if (bApplyLaunchImpulse)
	{
		if (ACharacter* Avatar = GetAvatarCharacter())
		{
			if (UCharacterMovementComponent* MoveComp = Avatar->GetCharacterMovement())
			{
				MoveComp->Velocity.X *= 0.15f;
				MoveComp->Velocity.Y *= 0.15f;
			}
		}
	}

	UE_LOG(LogTemp, Log,
		TEXT("[Dodge] 本次实际位移 = %.0f cm（目标 %.0f cm，本地方向 (%.2f, %.2f)）"
			 " —— 实际≈目标 说明动画根位移已被完全压住，位移全部来自代码（真正的移动，不会回弹）"),
		ActualDistance, PendingDistance, PendingLocalDir.X, PendingLocalDir.Y);

	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}

// ================================================================ 道具：使用"酒"回血
UGA_PlayerUseHeal::UGA_PlayerUseHeal()
{
	FGameplayTagContainer DefaultTags;
	DefaultTags.AddTag(DemoTag::Ability_UseItem_Heal);
	SetAssetTags(DefaultTags);
	// 喝酒期间算"忙碌"：占用 State.Attacking，取消正在跑的普攻/弹刀/格挡去开喝，
	// 并锁住进攻 / 格挡 / 闪避，避免"边喝边砍"破坏动作衔接。
	ActivationOwnedTags.AddTag(DemoTag::State_Attacking);

	CooldownGameplayEffectClass = UGE_HealCooldownEffect::StaticClass();

	// 喝血时锁住其它战斗动作（闪避在 UGA_PlayerDodge 里也把本技能列入 Block/Cancel，
	// 所以"闪避可打断喝酒"的优先级更高，喝到一半能立刻闪出去保命）。
	BlockAbilitiesWithTag.AddTag(DemoTag::Ability_Attack_Normal);
	BlockAbilitiesWithTag.AddTag(DemoTag::Ability_Attack_Skill);
	BlockAbilitiesWithTag.AddTag(DemoTag::Ability_Defense_Parry);
	BlockAbilitiesWithTag.AddTag(DemoTag::Ability_Defense_Block);

	// 开喝可以打断正在挥的普攻 / 技能 / 弹刀 / 格挡：让"被打断后立刻喝一口"也顺滑。
	CancelAbilitiesWithTag.AddTag(DemoTag::Ability_Attack_Normal);
	CancelAbilitiesWithTag.AddTag(DemoTag::Ability_Attack_Skill);
	CancelAbilitiesWithTag.AddTag(DemoTag::Ability_Defense_Parry);
	CancelAbilitiesWithTag.AddTag(DemoTag::Ability_Defense_Block);
}

void UGA_PlayerUseHeal::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	Super::ActivateAbility(Handle, ActorInfo, ActivationInfo, TriggerEventData);

	bHealApplied = false;
	bMoveLockedByHeal = false;

	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	AFightPlayerCharacter* Player = Cast<AFightPlayerCharacter>(GetAvatarCharacter());
	if (!Player)
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	UAbilitySystemComponent* ASC = UDemoAbilitySystemLibrary::GetAbilitySystemComponent(Player);
	if (!ASC)
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	// 满血：不浪费酒，直接结束（给个灰色提示）
	const float CurHealth = ASC->GetNumericAttribute(UDemoAttributeSet::GetHealthAttribute());
	const float MaxHealth = ASC->GetNumericAttribute(UDemoAttributeSet::GetMaxHealthAttribute());
	if (MaxHealth - CurHealth < KINDA_SMALL_NUMBER)
	{
		if (UCombatHUDWidget* HUD = Player->GetCombatHUD())
		{
			HUD->ShowFeedbackText(FText::FromString(TEXT("生命已满")), FLinearColor::Gray, 1.0f);
		}
		EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
		return;
	}

	// 没酒了：直接结束（HUD 提示"酒已喝光"）。TryConsumeHealItem 内部再判定一次，
	// 保证"没酒"和"满血"是两个独立分支，不会多扣一瓶。
	if (!Player->TryConsumeHealItem())
	{
		if (UCombatHUDWidget* HUD = Player->GetCombatHUD())
		{
			HUD->ShowFeedbackText(FText::FromString(TEXT("酒已喝光")), FLinearColor::Gray, 1.0f);
		}
		EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
		return;
	}

	// 播放喝酒动作（没在蓝图配则按平衡表默认路径兜底）
	UAnimMontage* Montage = HealMontage ? HealMontage.Get() : LoadMontage(DemoBalance::HealMontagePath);

	// 喝到 HealApplyRatio 进度时真正灌下（表现上"酒到嘴边才回血"）
	const float Len = Montage ? Montage->GetPlayLength() : 0.f;
	const float ApplyTime = (Len > KINDA_SMALL_NUMBER) ? Len * FMath::Clamp(HealApplyRatio, 0.1f, 0.95f) : 0.f;
	if (ApplyTime > 0.f && Montage)
	{
		if (UAbilityTask_WaitDelay* Delay = UAbilityTask_WaitDelay::WaitDelay(this, ApplyTime))
		{
			HealApplyTimer = Delay;
			Delay->OnFinish.AddDynamic(this, &UGA_PlayerUseHeal::OnHealApplyPoint);
			Delay->ReadyForActivation();
		}
		else
		{
			ApplyHeal(Player, ASC, MaxHealth);
		}
	}
	else
	{
		// ⚠⚠ 走到这里说明【没拿到蒙太奇】→ 玩家看不到任何喝酒动作（用户反馈的现象）。
		//   打 Warning 并把两条候选路径都打出来，一次定位是"资产没做"还是"路径写错"。
		UE_LOG(LogTemp, Warning,
			TEXT("[Heal] ⚠ 没有喝酒蒙太奇！HealMontage=%s，兜底路径=%s")
			TEXT("（请确认该资产存在、且骨架 = 玩家 Mesh 的 Skeleton）"),
			HealMontage ? *HealMontage->GetName() : TEXT("未配置"),
			DemoBalance::HealMontagePath);

		// 没有动画也要回血 —— 酒已经扣了，不能白扣（"喝酒"必须永远有治疗效果）。
		ApplyHeal(Player, ASC, MaxHealth);
	}


	// 锁住 WASD（避免"边喝边走"把动作拖成滑步）。
	// ⚠ 引用计数：无条件加锁，EndAbility 里配对减一次。
	if (AFightPlayerCharacter* P = Cast<AFightPlayerCharacter>(GetAvatarCharacter()))
	{
		P->LockMoveInput();
		bMoveLockedByHeal = true;
	}

	if (Montage)
	{
		UAbilityTask_PlayMontageAndWait* Task = UAbilityTask_PlayMontageAndWait::CreatePlayMontageAndWaitProxy(
			this, TEXT("HealMontage"), Montage, 1.f, NAME_None, true, 0.f);
		if (!Task)
		{
			EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
			return;
		}
		ActiveMontageTask = Task;
		Task->OnCompleted.AddDynamic(this, &UGA_PlayerUseHeal::OnMontageCompleted);
		Task->OnCancelled.AddDynamic(this, &UGA_PlayerUseHeal::OnMontageCancelled);
		Task->OnInterrupted.AddDynamic(this, &UGA_PlayerUseHeal::OnMontageCancelled);

		// ⚠⚠ 诊断（2026-10-03）：用户反馈"喝酒没有蒙太奇动画"。
		//   打这一行是为了区分两种情况：
		//     ① 蒙太奇没找到（资产缺失 / 路径不对）→ 下面的路径会是"无"
		//     ② 找到了但 Montage_Play 静默失败（骨架不匹配 / Slot 没注册）
		//   蒙太奇时长也要打出来 —— 时长为 0 说明是空壳资产。
		UE_LOG(LogTemp, Log,
			TEXT("[Heal] 播放喝酒蒙太奇：%s  时长=%.2fs  来源=%s"),
			*Montage->GetName(), Montage->GetPlayLength(),
			HealMontage ? TEXT("蓝图配置") : TEXT("平衡表兜底路径"));
		Task->ReadyForActivation();
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[Heal] 喝酒蒙太奇缺失，跳过动画直接回血（请跑 Tools/SetupHealDrinkMontage.py 生成 Heal_Drink_Montage）"));
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
	}
}

void UGA_PlayerUseHeal::OnHealApplyPoint()
{
	HealApplyTimer = nullptr;
	ApplyHeal(
		Cast<AFightPlayerCharacter>(GetAvatarCharacter()),
		UDemoAbilitySystemLibrary::GetAbilitySystemComponent(GetAvatarCharacter()),
		0.f);
}

void UGA_PlayerUseHeal::ApplyHeal(AFightPlayerCharacter* Player, UAbilitySystemComponent* ASC, float InMaxHealth)
{
	if (bHealApplied)
	{
		return;
	}
	bHealApplied = true;

	if (!Player || !ASC)
	{
		return;
	}

	const float MaxHealth = (InMaxHealth > KINDA_SMALL_NUMBER)
		? InMaxHealth
		: ASC->GetNumericAttribute(UDemoAttributeSet::GetMaxHealthAttribute());
	const float HealAmount = MaxHealth * DemoBalance::HealPercent;

	// 走 GE + SetByCaller（不是 SetHealth），才能触发 PostGameplayEffectExecute 的 clamp
	// + OnHealthChanged 广播，血条 / HUD / 回血反馈才联动。
	FGameplayEffectContextHandle Ctx = ASC->MakeEffectContext();
	Ctx.AddSourceObject(this);
	const FGameplayEffectSpecHandle SpecHandle = ASC->MakeOutgoingSpec(UGE_HealEffect::StaticClass(), 1.f, Ctx);
	if (SpecHandle.IsValid())
	{
		SpecHandle.Data->SetSetByCallerMagnitude(DemoTag::Data_Heal, HealAmount);
		ASC->ApplyGameplayEffectSpecToSelf(*SpecHandle.Data.Get());
	}

	// HUD 绿色反馈：整屏绿光 + "+600 生命"飘字（复用 CombatHUDWidget 既有通道）
	if (UCombatHUDWidget* HUD = Player->GetCombatHUD())
	{
		const FText HealText = FText::Format(
			NSLOCTEXT("DemoHeal", "HealAmount", "+{0} 生命"), FMath::RoundToInt(HealAmount));
		HUD->FlashScreen(FLinearColor(0.2f, 1.f, 0.35f), 0.40f, 0.55f);
		HUD->ShowFeedbackText(HealText, FLinearColor(0.3f, 1.f, 0.45f), 1.2f);
	}

	UE_LOG(LogTemp, Log, TEXT("[Heal] 喝酒回血 +%.0f（最大生命=%.0f，比例=%.0f%%）"),
		HealAmount, MaxHealth, DemoBalance::HealPercent * 100.f);
}

void UGA_PlayerUseHeal::OnMontageCompleted()
{
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
}

void UGA_PlayerUseHeal::OnMontageCancelled()
{
	// 被打断（受击 / 被闪避取消）也保证把这一口的血结算了：酒已经扣了，不能白扣。
	ApplyHeal(
		Cast<AFightPlayerCharacter>(GetAvatarCharacter()),
		UDemoAbilitySystemLibrary::GetAbilitySystemComponent(GetAvatarCharacter()),
		0.f);
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, true);
}

void UGA_PlayerUseHeal::EndAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	bool bReplicateEndAbility, bool bWasCancelled)
{
	// 本技能锁住的 WASD 必须由它自己解开：喝酒期间调了 SetMoveInputLocked(true)，
	// 若不在这里还原，玩家喝完会"卡住动不了"，直到下次攻击/闪避才解锁（隐蔽 bug）。
	// 只在"确实是本技能锁的"前提下解锁，避免误解放普攻/闪避加的锁。
	if (bMoveLockedByHeal)
	{
		if (AFightPlayerCharacter* P = Cast<AFightPlayerCharacter>(GetAvatarCharacter()))
		{
			P->UnlockMoveInput();   // 引用计数配对减一次
		}
		bMoveLockedByHeal = false;
	}

	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}

#pragma warning(pop)
