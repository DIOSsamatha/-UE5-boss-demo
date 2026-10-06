#include "Core/DemoCharacterBase.h"
#include "Core/DemoAttributeSet.h"
#include "Core/DemoAbilitySystemLibrary.h"
#include "Core/DemoCombatTuning.h"
#include "Core/DemoEffectContext.h"
#include "Core/DemoGameplayTags.h"
#include "Effects/DemoGameplayEffects.h"
#include "Abilities/DemoGameplayAbility.h"
#include "UI/DemoNameplateWidget.h"

#include "AbilitySystemComponent.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "Components/StaticMeshComponent.h"
#include "Components/WidgetComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/OverlapResult.h"
#include "TimerManager.h"

ADemoCharacterBase::ADemoCharacterBase()
{
	PrimaryActorTick.bCanEverTick = true;

	AbilitySystemComponent = CreateDefaultSubobject<UAbilitySystemComponent>(TEXT("AbilitySystemComponent"));
	AbilitySystemComponent->SetIsReplicated(false);
	AbilitySystemComponent->SetReplicationMode(EGameplayEffectReplicationMode::Minimal);

	AttributeSet = CreateDefaultSubobject<UDemoAttributeSet>(TEXT("AttributeSet"));

	// ---------- 头顶血条 ----------
	// 屏幕空间（Screen）而不是世界空间：远处的小怪血条不会变成一条线，读起来始终清晰度一致。
	HealthNameplateComponent = CreateDefaultSubobject<UWidgetComponent>(TEXT("HealthNameplate"));
	HealthNameplateComponent->SetupAttachment(GetRootComponent());
	HealthNameplateComponent->SetWidgetSpace(EWidgetSpace::Screen);
	HealthNameplateComponent->SetWidgetClass(UDemoNameplateWidget::StaticClass());
	HealthNameplateComponent->SetDrawSize(FVector2D(190.f, 34.f));
	// 轴心放在底边中点 → 条的底部贴着挂点，不会随文字行数上下跳
	HealthNameplateComponent->SetPivot(FVector2D(0.5f, 1.f));
	HealthNameplateComponent->SetTwoSided(true);
	HealthNameplateComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	HealthNameplateComponent->SetGenerateOverlapEvents(false);
	HealthNameplateComponent->SetVisibility(false);

	// ---------- 角色之间互相穿透时"温柔分开" ----------
	// UE 默认 MaxDepenetrationWithPawn = 32cm，而且是【一帧内瞬间位移】：
	// 一帧 32cm ≈ 1900cm/s，玩家眼里就是"我撞上去把对方弹飞 / 我被对方弹飞"。
	// 压到 4cm 后，重叠会在好几帧里慢慢分开，看起来只是"贴在一起挤了一下"，
	// 不会再出现任何一方被"打飞"。
	if (UCharacterMovementComponent* MoveComp = GetCharacterMovement())
	{
		MoveComp->MaxDepenetrationWithPawn = 4.f;
	}

	// ---------- 角色之间彻底不互相"硬推开"（根治"BOSS 被我推着走 / 打飞 / 瞬移"）----------
	// 根因：Pawn↔Pawn 默认通道响应是 Block，玩家身体撞上 BOSS 的胶囊，
	//   物理求解器每帧把双方互相顶开，单帧能推出 30~40cm（日志实锤：
	//   "[Boss][位移闸] 单帧位移 38.0cm"），位移闸再把它拉回，观感就是"瞬移 / 打飞"。
	// 改成 Overlap 后双方不再产生互推力，物理互推这个根因被消除。
	// ⚠ 命中检测不受影响：玩家砍 BOSS、BOSS 范围冲击都走 OverlapMultiByChannel(ECC_Pawn)，
	//   通道响应为 Overlap 仍会被该查询命中（Ignore 才跳过），所以"能砍到 BOSS"照常工作。
	if (UCapsuleComponent* Capsule = GetCapsuleComponent())
	{
		Capsule->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	}
}

UAbilitySystemComponent* ADemoCharacterBase::GetAbilitySystemComponent() const
{
	return AbilitySystemComponent;
}

void ADemoCharacterBase::EnsureDefaultAttributes()
{
	if (!AttributeSet || !AbilitySystemComponent)
	{
		return;
	}

	// ---- "有没有人配过属性"的判据 ----
	// UDemoAttributeSet 的构造函数给了一套哨兵值（见 DemoAttributeSet.cpp 顶部注释）。
	// 只要 MaxHealth 还是哨兵值，就说明蓝图 / 数据资产都没配过 → 由平衡表接管。
	// 这样既不会覆盖美术策划在 BP 里精心配过的值，也不会再出现"兜底从未执行"。
	// ---- 双异常条上限 ----
	// 只有 BOSS（bHasAnomalyBars）用得到，但统一在这里写：
	// 以后给精英怪开异常条时不会忘记配上限，否则槽会因为"当前值 >= 上限(0)"而永远判定为满。
	if (bHasAnomalyBars)
	{
		AttributeSet->SetMaxAnomalyImpact(DemoBalance::AnomalyImpactMax);
		AttributeSet->SetMaxAnomalyElement(DemoBalance::AnomalyElementMax);
		AttributeSet->SetAnomalyImpact(0.f);
		AttributeSet->SetAnomalyElement(0.f);
	}

	// ---- 架势条 ----
	// 所有角色都有。BOSS 容量单独给（它是主要被攻击方，容量相同会显得太脆）。
	const float PostureCap = bHasAnomalyBars ? DemoBalance::BossPostureMax : DemoBalance::PostureMax;
	AttributeSet->SetMaxPosture(PostureCap);
	if (AttributeSet->GetPosture() <= 0.f || AttributeSet->GetPosture() > PostureCap)
	{
		AttributeSet->SetPosture(PostureCap);
	}

	constexpr float UnconfiguredHealth = 100.f;
	if (AttributeSet->GetMaxHealth() > UnconfiguredHealth + KINDA_SMALL_NUMBER)
	{
		// 已经有真实数值了：只补那些明显是空的项（攻击力 / 防御力 / 移速）
		if (AttributeSet->GetAttackPower() <= 0.f && BalanceAttackPower > 0.f)
		{
			AttributeSet->SetAttackPower(BalanceAttackPower);
		}
		if (AttributeSet->GetDefensePower() <= 0.f && BalanceDefensePower > 0.f)
		{
			AttributeSet->SetDefensePower(BalanceDefensePower);
		}
		if (BalanceMoveSpeed > 0.f && AttributeSet->GetMoveSpeed() <= 0.f)
		{
			AttributeSet->SetMoveSpeed(BalanceMoveSpeed);
		}
		return;
	}

	// ---- 走平衡表 ----
	// 顺序有讲究：先设 Max* 再设当前值，否则当前值会被 PreAttributeChange 夹到旧的 Max 上。
	AttributeSet->SetMaxHealth(BalanceMaxHealth);
	AttributeSet->SetHealth(BalanceMaxHealth);

	AttributeSet->SetMaxStamina(BalanceMaxStamina);
	AttributeSet->SetStamina(BalanceMaxStamina);

	AttributeSet->SetMaxPoise(BalancePoiseCap);
	AttributeSet->SetPoise(BalancePoiseCap);

	AttributeSet->SetAttackPower(BalanceAttackPower);
	AttributeSet->SetDefensePower(BalanceDefensePower);

	if (BalanceMoveSpeed > 0.f)
	{
		AttributeSet->SetMoveSpeed(BalanceMoveSpeed);
	}

	// 这条是"数值到底生效没有"的第一现场证据 —— 排查"改了 C++ 但游戏里没变"先看它。
	UE_LOG(LogDemoCombat, Warning,
		TEXT("[Balance] %s 使用平衡表数值：血量=%.0f 韧性=%.0f 耐力=%.0f 攻=%.0f 防=%.0f 移速=%.0f"
			 "（来源 Core/DemoCombatTuning.h；想覆盖就在 BP 的 Demo|Balance 上改，或给 AttributeSet 配数据）"),
		*GetName(), BalanceMaxHealth, BalancePoiseCap, BalanceMaxStamina,
		BalanceAttackPower, BalanceDefensePower, AttributeSet->GetMoveSpeed());
}

void ADemoCharacterBase::BeginPlay()
{
	// ⚠ 必须在最前面：下面会用 MoveSpeed 去设 MaxWalkSpeed，
	// 而"未配置"的 AttributeSet 里 MoveSpeed 还是构造默认值。
	EnsureDefaultAttributes();

	Super::BeginPlay();

	if (AbilitySystemComponent)
	{
		AbilitySystemComponent->InitAbilityActorInfo(this, this);

		for (const TSubclassOf<UDemoGameplayAbility>& AbilityClass : DefaultAbilities)
		{
			if (AbilityClass)
			{
				AbilitySystemComponent->GiveAbility(FGameplayAbilitySpec(AbilityClass, 1, INDEX_NONE, this));
			}
		}

		UDemoAbilitySystemLibrary::ApplyEffectToSelf(this, UGE_StaminaRegenEffect::StaticClass());

		AbilitySystemComponent->GetGameplayAttributeValueChangeDelegate(UDemoAttributeSet::GetMoveSpeedAttribute())
			.AddUObject(this, &ADemoCharacterBase::OnMoveSpeedChanged);

		if (UCharacterMovementComponent* MoveComp = GetCharacterMovement())
		{
			MoveComp->MaxWalkSpeed = AttributeSet ? AttributeSet->GetMoveSpeed() : 600.f;
		}

		// 监听被命中事件，驱动受击表现
		AbilitySystemComponent->GenericGameplayEventCallbacks.FindOrAdd(DemoTag::Event_Hit_Dealt)
			.AddUObject(this, &ADemoCharacterBase::OnDamagedEventInternal);
	}

	if (AttributeSet)
	{
		AttributeSet->OnPoiseBroken.AddUObject(this, &ADemoCharacterBase::HandlePoiseBroken);
		AttributeSet->OnDeath.AddUObject(this, &ADemoCharacterBase::HandleDeath);

		// 双异常条：冲击槽满 → 失衡；失衡期间元素槽满 → 击破
		AttributeSet->OnAnomalyUnstable.AddUObject(this, &ADemoCharacterBase::HandleAnomalyUnstable);
		AttributeSet->OnAnomalyBroken.AddUObject(this, &ADemoCharacterBase::HandleAnomalyBroken);
		AttributeSet->OnPostureBroken.AddUObject(this, &ADemoCharacterBase::HandlePostureBroken);

		// 真实伤害结算回调（含格挡/破防暴击等判定结果）→ 转发给 OnDamagedDetailed
		AttributeSet->OnDamageTaken.AddUObject(this, &ADemoCharacterBase::HandleDamageTakenInternal);

		// 血量变化 → 头顶血条
		AttributeSet->OnHealthChanged.AddUObject(this, &ADemoCharacterBase::HandleHealthChangedForNameplate);
	}

	// ---------- 头顶血条 ----------
	if (HealthNameplateComponent)
	{
		HealthNameplateComponent->SetRelativeLocation(NameplateOffset);
		HealthNameplateComponent->SetDrawSize(FVector2D(190.f, 34.f));

		if (bShowHealthNameplate)
		{
			// 走到这里 Actor 已经 BeginPlay，WidgetComponent 的 UserWidget 一般已就绪；
			// InitWidget() 本身是幂等的，多调一次也安全。
			HealthNameplateComponent->InitWidget();

			if (UDemoNameplateWidget* Nameplate = Cast<UDemoNameplateWidget>(HealthNameplateComponent->GetUserWidgetObject()))
			{
				Nameplate->BindToCharacter(this);
			}
			HealthNameplateComponent->SetVisibility(true);
		}
		else
		{
			HealthNameplateComponent->SetVisibility(false);
		}
	}
}

void ADemoCharacterBase::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (bMeleeHitWindowOpen)
	{
		PerformMeleeTrace();
	}
}

// ------------------------------------------------------------------ 命中检测
void ADemoCharacterBase::SetMeleeHitWindowEnabled(bool bEnabled)
{
	bMeleeHitWindowOpen = bEnabled;

	if (bEnabled)
	{
		HitActorsThisSwing.Reset();
		bHasPrevSocketLocations = false;
	}
}

// 解析一个武器插槽的世界位置。
//
// ⚠ 为什么要遍历所有网格组件，而不是只用 GetMesh()：
//   武器在蓝图里是挂在角色身上的 StaticMesh 组件，WeaponStart / WeaponEnd 这两个插槽
//   定义在【武器资产】上（SM_Sword_08），角色骨骼网格上并没有同名插槽。
//   只用 GetMesh()->DoesSocketExist() 找的话永远返回 false → 每帧都退回
//   "身前 40~120cm" 那条粗判线，命中判定和实际刀锋位置脱节：
//   看着砍中了可能不判，看着没碰到反而判 —— 这就是"砍 BOSS 不掉血"的常见来源。
static bool ResolveWeaponSocket(const AActor* Owner, const USkeletalMeshComponent* MeshComp,
	FName SocketName, FVector& OutLocation)
{
	if (SocketName.IsNone())
	{
		return false;
	}

	// 1) 角色骨骼网格（有些项目会把插槽直接加在骨骼上）
	if (MeshComp && MeshComp->DoesSocketExist(SocketName))
	{
		OutLocation = MeshComp->GetSocketLocation(SocketName);
		return true;
	}

	// 2) 所有静态网格组件（武器最常见）。先找【可见】的 ——
	//    背在背上 / 收起状态的备用武器不该参与命中判定。
	if (Owner)
	{
		TArray<UStaticMeshComponent*> MeshComps;
		Owner->GetComponents<UStaticMeshComponent>(MeshComps);

		for (const UStaticMeshComponent* Comp : MeshComps)
		{
			if (Comp && Comp->IsVisible() && Comp->DoesSocketExist(SocketName))
			{
				OutLocation = Comp->GetSocketLocation(SocketName);
				return true;
			}
		}
		for (const UStaticMeshComponent* Comp : MeshComps)
		{
			if (Comp && Comp->DoesSocketExist(SocketName))
			{
				OutLocation = Comp->GetSocketLocation(SocketName);
				return true;
			}
		}

		// ⚠⚠⚠ 3) 【骨骼网格武器】也要找（2026-10-02 第二十轮修复）
		//
		// 实测日志：`[Melee] BP_Boss_C_... 武器插槽解析：WeaponStart=缺失，WeaponEnd=缺失`
		//   → BOSS 的武器是带插槽的骨骼网格组件，而这里只遍历了 StaticMeshComponent
		//   → 两个插槽都找不到 → 判定退化成"角色身前一整段"（40~120cm 的粗判线）
		//   → 表现为"BOSS 明明砍中了却不掉血 / 玩家看着砍到了也没反馈"。
		// 玩家自己的武器是 StaticMesh 所以能找到（所以只有 BOSS 那边出问题），
		// 这正是"有些攻击没伤害"的直接原因。
		//
		// 遍历【除主身体 Mesh 之外】的所有骨骼网格组件（武器/挂件），先找可见的。
		TArray<USkeletalMeshComponent*> SkelMeshComps;
		Owner->GetComponents<USkeletalMeshComponent>(SkelMeshComps);
		const USkeletalMeshComponent* BodyMesh = Owner->GetComponentByClass<USkeletalMeshComponent>();
		for (const USkeletalMeshComponent* Comp : SkelMeshComps)
		{
			// 跳过角色身体本身（身体网格上的同名插槽不是"武器刀锋"，且已在上��� 1) 查过）
			if (!Comp || Comp == BodyMesh)
			{
				continue;
			}
			if (Comp->IsVisible() && Comp->DoesSocketExist(SocketName))
			{
				OutLocation = Comp->GetSocketLocation(SocketName);
				return true;
			}
		}
		for (const USkeletalMeshComponent* Comp : SkelMeshComps)
		{
			if (!Comp || Comp == BodyMesh)
			{
				continue;
			}
			if (Comp->DoesSocketExist(SocketName))
			{
				OutLocation = Comp->GetSocketLocation(SocketName);
				return true;
			}
		}
	}

	return false;
}

void ADemoCharacterBase::PerformMeleeTrace()
{
	USkeletalMeshComponent* MeshComp = GetMesh();
	if (!MeshComp)
	{
		return;
	}

	// 拼刀判定用："我这把刀现在真的在挥"。
	// 必须打在扫掠之前：扫掠被调用 = 攻击判定窗正在生效，这正是"出手"的精确定义。
	MarkSwing();

	// ---- 1) 采样点：优先用武器插槽的真实刀锋位置 ----
	FVector Start = FVector::ZeroVector;
	FVector End = FVector::ZeroVector;

	const bool bHasStartSocket = ResolveWeaponSocket(this, MeshComp, WeaponStartSocket, Start);
	const bool bHasEndSocket = ResolveWeaponSocket(this, MeshComp, WeaponEndSocket, End);

	// 只解出一个端点时，用角色前方 80cm 把另一端补齐
	//（比整段都走兜底线好：至少有一端是真的刀锋）
	const FVector Fwd = MeshComp->GetForwardVector();
	const FVector BodyLoc = MeshComp->GetComponentLocation();

	// ⚠⚠⚠ 插槽全缺时的兜底：【用持武器的手骨 + 手臂骨】当刀锋（2026-10-02 第二十一轮决定性修复）
	//
	// 实测日志（无头对局，取证）：
	//   [Melee] BP_Boss_C_... 武器插槽解析：WeaponStart=缺失，WeaponEnd=缺失
	//   [Melee] BP_FightPlayer_C_0 武器插槽解析：WeaponStart=找到，WeaponEnd=找到
	// 玩家能找到、**BOSS 两个都找不到** → BOSS 的刀锋判定退化成
	//   "身体前方 40~120cm 的一条固定线段"（与朝向无关的粗判线）——
	//   而 BOSS 的 Mesh 带 Yaw = -90° 相对旋转、身体中心离刀锋很远，
	//   于是判定线段和实际刀锋完全脱节：
	//     · 看着砍中了却查不到人 → 玩家觉得"攻击没伤害、也不能交互"；
	//     · 看着没碰到反而判中。
	// 这是"有些攻击了却没伤害"的**唯一根因**（命中窗口本身已在前几轮修好）。
	//
	// 正解：插槽找不到时，退到【骨骼】上取点 ——
	//   刀柄 = 持刀手骨（hand_r / hand_l，Khaimera 是右手持刀）
	//   刀尖 = 同侧前臂/上臂延伸出去（forearm_r / lowerarm_r / upperarm_r）
	// 骨骼名做多候选 + 模糊匹配，兼容不同骨架。
	if (!bHasStartSocket && !bHasEndSocket)
	{
		// 持刀手
		static const FName HandBones[] = {
			TEXT("hand_r"), TEXT("hand_l"), TEXT("wrist_r"), TEXT("wrist_l") };
		// 刀尖方向：优先前臂（离手最近、方向大致等于刀锋）
		static const FName BladeBones[] = {
			TEXT("forearm_r"), TEXT("lowerarm_r"), TEXT("upperarm_r"),
			TEXT("forearm_l"), TEXT("lowerarm_l"), TEXT("upperarm_l") };

		const auto FindFirstBone = [MeshComp](const FName* Names, int32 Count) -> FName
		{
			for (int32 i = 0; i < Count; ++i)
			{
				if (MeshComp->GetBoneIndex(Names[i]) != INDEX_NONE)
				{
					return Names[i];
				}
			}
			return NAME_None;
		};

		const FName HandBone = FindFirstBone(HandBones, UE_ARRAY_COUNT(HandBones));
		const FName BladeBone = FindFirstBone(BladeBones, UE_ARRAY_COUNT(BladeBones));

		if (HandBone != NAME_None)
		{
			Start = MeshComp->GetBoneLocation(HandBone, EBoneSpaces::WorldSpace);
			// ⚠⚠⚠ 刀锋必须【随手臂挥动】，"手臂骨"不能当固定点（2026-10-02 第二十一轮决定性修复）
			//
			// 实测（无头日志，取证）：
			//   [Melee] BP_Boss_C_... WeaponStart=缺失，WeaponEnd=缺失
			//          刀柄(-374,-259,176) 刀尖(-240,-342,176) 长度=157cm 半径=45
			// 这两个点是**挥砍时抓某一帧算出来的**。之前每帧都用"手骨 + 外推"重算，
			// 但外推方向来自 forearm→hand 的【瞬时骨向量】—— 手臂在挥砍中途会【弯折/回抽】，
			// 一旦那一帧手臂方向和刀锋实际朝向差 90°，判定球就飞到身体另一侧去了。
			// 玩家看到的现象就是【刀明明砍在身上，却查不到人、也没法交互】。
			//
			// 正解：沿【手臂骨链 hand → forearm → upperarm】取【多个采样点】，
			//   再从最后一个点沿"肩→手"方向外推刀长 —— 外推方向取自【稳定的肩/胸骨】，
			//   不受挥砍瞬时姿态影响，判定球因此始终贴在真实刀锋上。
			FVector BladeDir = Fwd;
			float BestLen = 0.f;
			const FName BladeBones2[] = {
				TEXT("forearm_r"), TEXT("lowerarm_r"), TEXT("upperarm_r"),
				TEXT("forearm_l"), TEXT("lowerarm_l"), TEXT("upperarm_l") };
			for (const FName Cand : BladeBones2)
			{
				if (MeshComp->GetBoneIndex(Cand) == INDEX_NONE)
				{
					continue;
				}
				const FVector CandPos = MeshComp->GetBoneLocation(Cand, EBoneSpaces::WorldSpace);
				const FVector D = (CandPos - Start);
				if (D.SizeSquared2D() > BestLen * BestLen)
				{
					BestLen = D.Size2D();
					BladeDir = D.GetSafeNormal2D();
				}
			}
			if (BladeDir.IsNearlyZero())
			{
				BladeDir = Fwd;
			}

			// 刀长：优先用一个可配置值；缺省按【躯干高度】推算（BOSS 体型越大刀越长），
			// 至少保证判定半径能覆盖到贴身的目标。
			const FBoxSphereBounds BodyB = MeshComp->Bounds;
			const float ScaleRef = FMath::Max(BodyB.BoxExtent.Z, 1.f);   // 约等于半身高
			const float BladeLen = FMath::Max(WeaponTraceRadius * 3.5f, ScaleRef * 1.15f);

			// ⚠⚠⚠ 沿【手臂骨链】布多个判定点，而不是只取"手 + 外推一点"（第二十一轮）
			//
			// 为什么：单点判定在挥砍中会"甩掉"目标。手臂从举刀到劈下，骨链长度不变，
			// 但【手骨到身体的距离】变化很大 —— 举刀时手在头顶（离躯干远）、
			// 劈下时手在腰侧（离躯干近）。只用手骨一个点，就会出现
			// "劈到玩家胸口的那一刻，手骨其实在玩家头顶偏后" → 查不到人。
			// 沿骨链 + 刀长方向布 5 个点，能覆盖整条挥砍轨迹。
			ExtraTracePoints.Reset();
			{
				const FName ChainBones[] = {
					TEXT("forearm_r"), TEXT("lowerarm_r"), TEXT("upperarm_r"), TEXT("clavicle_r"),
					TEXT("forearm_l"), TEXT("lowerarm_l"), TEXT("upperarm_l"), TEXT("clavicle_l") };
				for (const FName B : ChainBones)
				{
					if (MeshComp->GetBoneIndex(B) != INDEX_NONE)
					{
						ExtraTracePoints.Add(MeshComp->GetBoneLocation(B, EBoneSpaces::WorldSpace));
					}
				}
				// 刀身中段也要覆盖：从手骨沿刀锋方向等距取样
				for (int32 i = 1; i <= 3; ++i)
				{
					ExtraTracePoints.Add(Start + BladeDir * (BladeLen * (float(i) / 3.f)));
				}
			}
			End = Start + BladeDir * BladeLen;
		}
		else
		{
			// 极端兜底：连手骨都没有（换骨架了）→ 保留原来的粗判线
			Start = BodyLoc + Fwd * 40.f;
			End = BodyLoc + Fwd * 120.f;
		}
	}
	else if (!bHasStartSocket)
	{
		Start = End - Fwd * 80.f;
	}
	else if (!bHasEndSocket)
	{
		End = Start + Fwd * 80.f;
	}

	// 第一次挥砍打一行：这是"判定位置和刀锋对不上"的第一现场证据
	if (!bLoggedWeaponSockets)
	{
		bLoggedWeaponSockets = true;
		UE_LOG(LogDemoCombat, Log,
			TEXT("[Melee] %s 武器插槽解析：%s=%s，%s=%s → 判定线段 刀柄(%.0f,%.0f,%.0f) 刀尖(%.0f,%.0f,%.0f) 长度=%.0fcm 半径=%.0f"),
			*GetName(),
			*WeaponStartSocket.ToString(), bHasStartSocket ? TEXT("找到") : TEXT("缺失"),
			*WeaponEndSocket.ToString(), bHasEndSocket ? TEXT("找到") : TEXT("缺失"),
			Start.X, Start.Y, Start.Z, End.X, End.Y, End.Z,
			FVector::Dist(Start, End), WeaponTraceRadius);
	}

	// ---- 2) 连续扫掠：用"重叠检测"把挥砍轨迹上多个采样点都查一遍 ----
	// ⚠ 关键修正（玩家砍不到 BOSS 的根因）：
	//   角色胶囊对 ECC_Pawn 通道是【Overlap】而非【Block】（项目约定，
	//   DemoProjectile / 锁定 / 范围冲击全用 OverlapMultiByChannel）。
	//   SweepMultiByChannel 只返回 Blocking 命中，用它查 Pawn 通道会什么都查不到，
	//   于是"刀明明砍在 BOSS 身上却一次都打不中"——玩家因此感觉"攻击不到 / BOSS 不掉血"。
	//   这里改用与 BOSS 范围冲击相同的 OverlapMultiByChannel，命中立即恢复。
	TArray<FOverlapResult> Overlaps;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(DemoMeleeTrace), false, this);
	const FCollisionShape Shape = FCollisionShape::MakeSphere(WeaponTraceRadius);

	// 在"上一帧刀尖/刀柄"和"这一帧刀尖/刀柄"四个点各做一次球形重叠，
	// 覆盖挥砍过程中刀刃扫过的整条带，避免快速挥砍从目标身上"跳过"（低帧率时尤其明显）。
	TArray<FVector> SamplePoints;
	if (bHasPrevSocketLocations)
	{
		SamplePoints.Append({ PrevStartLocation, PrevEndLocation,
			(PrevStartLocation + Start) * 0.5f, (PrevEndLocation + End) * 0.5f });
	}
	SamplePoints.Append({ Start, End });
	// ⚠⚠ 插槽缺失时（BOSS 就是这种情况）额外布上【手臂骨链 + 刀身中段】采样点，
	//   否则只有"手骨 + 外推端点"两个点，挥砍中会甩掉贴身的玩家。
	//   （见上面插槽全缺分支的注释：这就是"砍了没伤害、也不能交互"的根因。）
	SamplePoints.Append(ExtraTracePoints);

	for (const FVector& Point : SamplePoints)
	{
		Overlaps.Reset();
		GetWorld()->OverlapMultiByChannel(Overlaps, Point, FQuat::Identity, ECC_Pawn, Shape, Params);
		for (const FOverlapResult& Overlap : Overlaps)
		{
			AActor* HitActor = Overlap.GetActor();
			if (!HitActor || HitActor == this)
			{
				continue;
			}
			if (HitActorsThisSwing.Contains(HitActor))
			{
				continue;
			}
			if (!IsHostileTo(HitActor))
			{
				continue;
			}

			HitActorsThisSwing.Add(HitActor);

			const EDemoHitResultType Result = UDemoAbilitySystemLibrary::ApplyHit(this, HitActor,
				CurrentAttackDamage, CurrentPoiseDamage,
				bCurrentAttackCanBeParried, bCurrentAttackCanBeBlocked, GetActorLocation(),
				true, EDemoDamageSource::Melee);

			OnHitLanded.Broadcast(HitActor, CurrentAttackDamage, Result, HitActor->GetActorLocation());
		}
	}

	// 每次挥砍都打一行"这次砍到谁 / 没砍到谁"——
	// "攻击了却没伤害"必须能从日志一眼定性，而不是靠猜。
	// 只在【开着判定窗】时打（也就是真的在攻击中），并做节流。
	{
		static int32 TraceLogCounter = 0;
		const bool bAnyHostile = [&Overlaps, this]() {
			for (const FOverlapResult& O : Overlaps)
			{
				AActor* A = O.GetActor();
				if (A && A != this && !HitActorsThisSwing.Contains(A) && IsHostileTo(A))
				{
					return true;
				}
			}
			return false;
		}();

		UE_LOG(LogDemoCombat, Log,
			TEXT("[MeleeTrace] %s 攻击中：采样点=%d 查到的Pawn=%d 可命中=%d 本次新命中=%d 已命中过=%d 刀柄(%.0f,%.0f,%.0f) 刀尖(%.0f,%.0f,%.0f) 伤害=%.0f"),
			*GetName(), SamplePoints.Num(), Overlaps.Num(), bAnyHostile ? 1 : 0,
			HitActorsThisSwing.Num() > 0 ? 1 : 0, HitActorsThisSwing.Num(),
			Start.X, Start.Y, Start.Z, End.X, End.Y, End.Z, CurrentAttackDamage);
		++TraceLogCounter;
	}

	PrevStartLocation = Start;
	PrevEndLocation = End;
	bHasPrevSocketLocations = true;
}

void ADemoCharacterBase::SetComboWindowEnabled(bool bEnabled)
{
	bComboWindowOpen = bEnabled;

	FGameplayEventData Payload;
	Payload.Instigator = this;
	Payload.Target = this;
	UAbilitySystemBlueprintLibrary::SendGameplayEventToActor(this,
		bEnabled ? DemoTag::Event_Montage_ComboBegin : DemoTag::Event_Montage_ComboEnd, Payload);
}

void ADemoCharacterBase::SetLastMoveInput(const FVector& InMoveInput)
{
	LastMoveInput = InMoveInput;

	if (const UWorld* World = GetWorld())
	{
		LastMoveInputTime = World->GetTimeSeconds();
	}
}

void ADemoCharacterBase::ClearLastMoveInput()
{
	LastMoveInput = FVector::ZeroVector;

	// 把时间戳也打回“很久以前”，这样 HasMoveInput() 立刻返回 false
	LastMoveInputTime = -1000.f;
}

bool ADemoCharacterBase::HasMoveInput(float MaxAge) const
{
	if (LastMoveInput.SizeSquared2D() < 0.01f)
	{
		return false;
	}

	const UWorld* World = GetWorld();
	if (!World || MaxAge <= 0.f)
	{
		return true;
	}

	return (World->GetTimeSeconds() - LastMoveInputTime) <= MaxAge;
}

FVector ADemoCharacterBase::GetMoveInputOrZero(float MaxAge) const
{
	return HasMoveInput(MaxAge) ? LastMoveInput : FVector::ZeroVector;
}

void ADemoCharacterBase::SetCurrentAttackParams(float Damage, float PoiseDamage, bool bCanBeParried, bool bCanBeBlocked)
{
	CurrentAttackDamage = Damage;
	CurrentPoiseDamage = PoiseDamage;
	bCurrentAttackCanBeParried = bCanBeParried;
	bCurrentAttackCanBeBlocked = bCanBeBlocked;
}

bool ADemoCharacterBase::IsHostileTo(const AActor* Other) const
{
	const ADemoCharacterBase* OtherCharacter = Cast<ADemoCharacterBase>(Other);
	if (!OtherCharacter)
	{
		return false;
	}
	return OtherCharacter->TeamId != TeamId;
}

// ------------------------------------------------------------------ 属性
float ADemoCharacterBase::GetHealth() const { return AttributeSet ? AttributeSet->GetHealth() : 0.f; }
float ADemoCharacterBase::GetMaxHealth() const { return AttributeSet ? AttributeSet->GetMaxHealth() : 0.f; }
float ADemoCharacterBase::GetStamina() const { return AttributeSet ? AttributeSet->GetStamina() : 0.f; }
float ADemoCharacterBase::GetMaxStamina() const { return AttributeSet ? AttributeSet->GetMaxStamina() : 0.f; }
float ADemoCharacterBase::GetPoise() const { return AttributeSet ? AttributeSet->GetPoise() : 0.f; }
float ADemoCharacterBase::GetMaxPoise() const { return AttributeSet ? AttributeSet->GetMaxPoise() : 0.f; }

bool ADemoCharacterBase::IsDead() const
{
	return AbilitySystemComponent && AbilitySystemComponent->HasMatchingGameplayTag(DemoTag::State_Dead);
}

// ------------------------------------------------------------------ 内部回调
void ADemoCharacterBase::OnMoveSpeedChanged(const FOnAttributeChangeData& Data)
{
	if (UCharacterMovementComponent* MoveComp = GetCharacterMovement())
	{
		MoveComp->MaxWalkSpeed = Data.NewValue;
	}
}

void ADemoCharacterBase::HandlePoiseBroken()
{
	UDemoAbilitySystemLibrary::ApplyStagger(this, true);
	OnPoiseBroken.Broadcast();

	GetWorldTimerManager().SetTimer(BreakRecoverTimerHandle, this, &ADemoCharacterBase::RecoverFromBreak, BreakDuration, false);
}

void ADemoCharacterBase::RecoverFromBreak()
{
	if (AbilitySystemComponent)
	{
		AbilitySystemComponent->RemoveLooseGameplayTag(DemoTag::State_Broken);
	}
	UDemoAbilitySystemLibrary::ApplyEffectToSelf(this, UGE_PoiseRecoveryEffect::StaticClass());
}

void ADemoCharacterBase::OnBlockedIncomingHit_Implementation(AActor* Attacker)
{
	// 基类默认什么都不做。BOSS 会重写它：累计"被压着打"的次数并在阈值时反制。
}

void ADemoCharacterBase::MarkSwing()
{
	if (const UWorld* World = GetWorld())
	{
		LastSwingTime = World->GetTimeSeconds();
	}
}

bool ADemoCharacterBase::HasRecentSwing(float Window) const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	return (World->GetTimeSeconds() - LastSwingTime) <= Window;
}

bool ADemoCharacterBase::HasRecentClash(float Window) const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	return (World->GetTimeSeconds() - LastClashTime) <= Window;
}

void ADemoCharacterBase::MarkClash()
{
	if (const UWorld* World = GetWorld())
	{
		LastClashTime = World->GetTimeSeconds();
	}
}

float ADemoCharacterBase::GetPosture() const
{
	return AttributeSet ? AttributeSet->GetPosture() : 0.f;
}

float ADemoCharacterBase::GetMaxPosture() const
{
	return AttributeSet ? AttributeSet->GetMaxPosture() : 0.f;
}

float ADemoCharacterBase::GetAnomalyImpact() const
{
	return AttributeSet ? AttributeSet->GetAnomalyImpact() : 0.f;
}

float ADemoCharacterBase::GetMaxAnomalyImpact() const
{
	return AttributeSet ? AttributeSet->GetMaxAnomalyImpact() : 0.f;
}

float ADemoCharacterBase::GetAnomalyElement() const
{
	return AttributeSet ? AttributeSet->GetAnomalyElement() : 0.f;
}

float ADemoCharacterBase::GetMaxAnomalyElement() const
{
	return AttributeSet ? AttributeSet->GetMaxAnomalyElement() : 0.f;
}

void ADemoCharacterBase::ClearAnomalyBars()
{
	if (!AbilitySystemComponent || !AttributeSet)
	{
		return;
	}

	AbilitySystemComponent->SetNumericAttributeBase(UDemoAttributeSet::GetAnomalyImpactAttribute(), 0.f);
	AbilitySystemComponent->SetNumericAttributeBase(UDemoAttributeSet::GetAnomalyElementAttribute(), 0.f);
}

void ADemoCharacterBase::HandlePostureBroken()
{
	if (!AbilitySystemComponent)
	{
		return;
	}

	// 挂 State_Broken 有两个用途：
	//   ① ApplyHit 里靠它判断"这一下格挡是不是被打破了"，从而补上"部分伤害"的差额；
	//   ② DemoDamageExecution 里破防状态受伤 ×1.5 —— 破防后的几刀会很疼，
	//      这是"架势打空"这件事真正的代价，玩家下一次才会更慎重地选择"挡还是弹"。
	AbilitySystemComponent->AddLooseGameplayTag(DemoTag::State_Broken);

	// 硬直：破防期间不能再动作（这就是参考 Demo 里"进入硬直"那一段）
	UDemoAbilitySystemLibrary::ApplyStagger(this, true);

	// 取消格挡本身：盾已经被打飞了，没道理还举着。
	// 少了这一步会出现"明明破防了，玩家松开再按住又能立刻挡住"的手感 bug。
	FGameplayTagContainer CancelTags;
	CancelTags.AddTag(DemoTag::Ability_Defense_Block);
	AbilitySystemComponent->CancelAbilities(&CancelTags);

	UE_LOG(LogDemoCombat, Warning,
		TEXT("[架势] %s 架势被打空 → 被破防（硬直 %.1fs，破防期间受伤加成）"),
		*GetName(), DemoBalance::GuardBreakStaggerTime);

	GetWorldTimerManager().SetTimer(PostureRecoverTimerHandle, this,
		&ADemoCharacterBase::RecoverFromPostureBreak, DemoBalance::GuardBreakStaggerTime, false);
}

void ADemoCharacterBase::RecoverFromPostureBreak()
{
	if (AbilitySystemComponent)
	{
		AbilitySystemComponent->RemoveLooseGameplayTag(DemoTag::State_Broken);

		// 架势只回一半：破防的代价要延续到起身之后，
		// 但又不至于让玩家从零开始、下一波直接再被破一次（那会变成死亡螺旋）。
		const float RecoverTo = AttributeSet
			? AttributeSet->GetMaxPosture() * DemoBalance::GuardBreakPostureRecoverRatio
			: 0.f;
		AbilitySystemComponent->SetNumericAttributeBase(UDemoAttributeSet::GetPostureAttribute(), RecoverTo);
	}
}

void ADemoCharacterBase::HandleAnomalyUnstable()
{
	// 失衡不是硬控：BOSS 照样能行动，只是"更好打了"（受伤加成 + 绿色轮廓）。
	// 所以这里【不】施加硬直，只广播信号 + 起一个限时计时器。
	OnAnomalyUnstable.Broadcast();

	GetWorldTimerManager().SetTimer(UnstableRecoverTimerHandle, this,
		&ADemoCharacterBase::RecoverFromUnstable, DemoBalance::UnstableDuration, false);
}

void ADemoCharacterBase::RecoverFromUnstable()
{
	ClearAnomalyBars();

	if (AbilitySystemComponent)
	{
		AbilitySystemComponent->RemoveLooseGameplayTag(DemoTag::State_Unstable);
	}
}

void ADemoCharacterBase::HandleAnomalyBroken()
{
	// 击破 = 虚弱窗口：复用韧性清零那条成熟链路（硬直 + 广播 + 到点自动恢复）。
	// 区别在于进入方式：韧性清零是"削韧削到底"，击破是"失衡期间把元素槽也打满"。
	HandlePoiseBroken();
	OnAnomalyBroken.Broadcast();

	// 失衡在击破面前没有意义了 —— 直接退出并清空两条槽，
	// 免得虚弱结束的同一秒又弹出一次失衡，玩家会读成"状态乱跳"。
	GetWorldTimerManager().ClearTimer(UnstableRecoverTimerHandle);
	ClearAnomalyBars();

	if (AbilitySystemComponent)
	{
		AbilitySystemComponent->RemoveLooseGameplayTag(DemoTag::State_Unstable);
	}
}

void ADemoCharacterBase::HandleDeath()
{
	if (UCharacterMovementComponent* MoveComp = GetCharacterMovement())
	{
		MoveComp->DisableMovement();
	}

	AbilitySystemComponent->CancelAllAbilities();

	OnCharacterDeath.Broadcast();
	OnDeathReaction();
}

// 被命中事件：交给子类播受击表现
void ADemoCharacterBase::OnDamagedEventInternal(const FGameplayEventData* Payload)
{
	const float Damage = Payload ? Payload->EventMagnitude : 0.f;
	const FVector FromLocation = (Payload && Payload->Instigator)
		? Payload->Instigator->GetActorLocation()
		: GetActorLocation() - GetActorForwardVector() * 100.f;

	// 真正的判定结果（普通 / 被格挡 / 破防暴击）在上下文的信封里，
	// 由 ApplyHit 塞进去、我们在这里拆出来 —— 受击表现才能区分"挨打"和"挡住了"。
	EDemoHitResultType ResultType = EDemoHitResultType::Normal;
	if (Payload)
	{
		const FGameplayEffectContext* RawContext = Payload->ContextHandle.Get();
		if (RawContext && RawContext->GetScriptStruct() == FDemoGameplayEffectContext::StaticStruct())
		{
			ResultType = static_cast<const FDemoGameplayEffectContext*>(RawContext)->HitResultType;
		}
	}

	const EDemoHitDirection Dir = UDemoAbilitySystemLibrary::GetHitDirection(this, FromLocation);
	OnDamaged.Broadcast(Damage, Dir, ResultType);

	// 被格挡 / 无敌帧这类"没真挨打"的情况不播受击蒙太奇（否则格挡看起来像被打中）
	if (ResultType != EDemoHitResultType::Dodged)
	{
		OnDamagedReaction(Dir, Damage);
	}
}

// ==================================================================================
// 受击信息 → UI / 其他系统
// ==================================================================================
void ADemoCharacterBase::HandleDamageTakenInternal(float AppliedDamage, EDemoHitResultType ResultType,
	EDemoDamageSource DamageSource, FVector HitDirection)
{
	LastDamageTaken = AppliedDamage;
	LastHitResultType = ResultType;
	LastDamageSource = DamageSource;

	OnDamagedDetailed.Broadcast(AppliedDamage, ResultType, DamageSource, HitDirection);
}

void ADemoCharacterBase::HandleHealthChangedForNameplate(float NewValue, float MaxValue)
{
	if (UDemoNameplateWidget* Nameplate = GetHealthNameplate())
	{
		Nameplate->SetHealthValue(NewValue, MaxValue);
	}
}

// ==================================================================================
// 头顶血条
// ==================================================================================
UDemoNameplateWidget* ADemoCharacterBase::GetHealthNameplate() const
{
	if (!HealthNameplateComponent)
	{
		return nullptr;
	}
	return Cast<UDemoNameplateWidget>(HealthNameplateComponent->GetUserWidgetObject());
}

void ADemoCharacterBase::SetHealthNameplateVisible(bool bVisible)
{
	bShowHealthNameplate = bVisible;

	if (!HealthNameplateComponent)
	{
		return;
	}

	if (bVisible)
	{
		HealthNameplateComponent->InitWidget();

		if (UDemoNameplateWidget* Nameplate = GetHealthNameplate())
		{
			Nameplate->BindToCharacter(this);
		}
	}

	HealthNameplateComponent->SetVisibility(bVisible);
}

void ADemoCharacterBase::OnDamagedReaction(EDemoHitDirection Direction, float Damage)
{
	// 子类覆写：播放受击蒙太奇 / 材质闪白 / 特效
}

void ADemoCharacterBase::OnDeathReaction()
{
	// 子类覆写：播放死亡蒙太奇
}
