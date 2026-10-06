// BOSS 角色本体：挂大脑组件、按招式类型分发技能、受击/破防/死亡表现、范围冲击
//
// 与玩家共用 ADemoCharacterBase：
//   · 武器扫掠命中检测（WeaponStart / WeaponEnd 插槽）→ 近战挥砍直接复用
//   · ApplyHit 统一伤害入口 → 格挡 / 弹反 / 无敌帧表现一致
// 这里额外补的是「只有 BOSS 才需要」的部分：霸体、四向受击、破防虚弱三段、落地范围伤害、
// 以及二阶段（狂化）的进入接口。
#pragma once

#include "CoreMinimal.h"
#include "Core/DemoCharacterBase.h"
#include "Abilities/GameplayAbility.h"
#include "Boss/BossCombatData.h"
#include "BossCharacterBase.generated.h"

class UBossBrainComponent;
class UAnimMontage;
class UAnimSequence;
class UAnimInstance;
class UGameplayEffect;
class UPointLightComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UGA_BossAttackBase;

// 物理落地瞬间广播（对蓝图有用：落地扬尘 / 震屏 / 音效可以绑它）
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDemoOnBossLanded, FHitResult, Hit);

// BOSS 蒙太奇"播完"回调（单节点模式下用定时器触发，避免依赖 Montage_Play 的回调）
DECLARE_DELEGATE_OneParam(FOnBossMontageFinished, UAnimMontage* /*Montage*/);

UCLASS()
class DEMO_API ABossCharacterBase : public ADemoCharacterBase
{
	GENERATED_BODY()

public:
	ABossCharacterBase();

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss")
	UBossBrainComponent* GetBrain() const { return Brain; }

	// 战斗目标：优先取大脑锁定的目标，没有则退回 0 号玩家 Pawn。
	// 技能（突进斩的冲刺方向、弹幕瞄准）统一从这里取，避免各自重复实现。
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss")
	AActor* GetCombatTarget() const;

	// 按招式类型取技能类：优先用蓝图配的槽位 → 约定路径的蓝图子类 → 原生 C++ 类。
	// 三级兜底的意义：即使忘了在 BP_Boss 里配技能、也没建蓝图子类，BOSS 照样能打。
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss")
	TSubclassOf<UGameplayAbility> GetAbilityClassForAttackType(EBossAttackType AttackType) const;

	// ---------- 招式数据交接（Brain → 技能）----------
	// 为什么挂在角色上而不是直接塞给技能实例：
	//   能力的 InstancedPerActor 实例是【首次激活时】才创建的，首次出招时 GetPrimaryInstance() 还是 null，
	//   SetPendingAttack 会静默失败 → 技能拿不到数据、空放。挂在角色上就没有这个时序问题。
	UFUNCTION(BlueprintCallable, Category = "Boss")
	void SetPendingAttackData(UBossAttackData* InAttack);

	// ⚠ 突进（冲刺）位移期间必须【冻结朝向】，Brain 的战斗追踪会跳过它。
	// 为什么：突进的目标点在起步瞬间就算好了、朝向也锁在冲刺方向上；
	// 如果这时候 Brain 还在持续把身体拧向玩家，直线突进会被拧成一条弧线，
	// 表现为"BOSS 边平移边甩身体"，收尾斩会砍偏。位移任务本身才是这段的主人。
	UPROPERTY(BlueprintReadWrite, Category = "Boss|Movement")
	bool bDashFacingLocked = false;

	// 角色自身是否还在执行一次"指定朝向"的转向（FaceTarget 触发的那次）。
	// Brain 的持续追踪要让路，否则两者同时写 SetActorRotation 会互相拉扯出抖动。
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Movement")
	bool IsSmoothFacingActive() const { return bHasFacingTarget; }

	// 胶囊体最多能"迈上"多高的台阶（cm）。UE 默认只有 45，BOSS 会被竞技场的
	// 台阶/缘石/矮台卡住直线驱动的路径（无导航时不会绕路）→ 站在原地磨蹭。
	// 在 BeginPlay 里应用（CDO 可能存着旧组件值，构造函数里设会被蓝图压回去）。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Movement")
	float BossMaxStepHeight = 110.f;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss")
	UBossAttackData* GetPendingAttackData() const { return PendingAttackData; }

	// 保障 4 类招式技能都已授予（BP_Boss 的 DefaultAbilities 漏配时自动补上）
	UFUNCTION(BlueprintCallable, Category = "Boss")
	void EnsureBossAbilitiesGranted();

	// 播一段 BOSS 蒙太奇（阶段演出 / 受击 / 破防 / 死亡都走它）。
	// 内部会先做根位移模式自愈，避免"动画推着 Mesh 走、播完弹回去"。
	// 单节点关键修复：用 PlayAnimation 把蒙太奇设为单节点 CurrentAsset（单节点下只有 CurrentAsset
	// 会被求值，Montage_Play 创建的实例永不推进、永不回调 → 攻击不可见且能力卡死），并按蒙太奇时长
	// 设定定时器，在播完时触发 Callback（等价原 UAbilityTask_PlayMontageAndWait 的 OnCompleted）。
	// 注意：本函数【不能】标 UFUNCTION —— 其 Callback 参数是非反射委托 FOnBossMontageFinished
	// （DECLARE_DELEGATE_OneParam），UHT 无法解析该默认值；且它只被 C++（BossBrain / 能力系统）调用，
	// 无需暴露给蓝图。
	void PlayBossMontage(UAnimMontage* Montage, float PlayRate = 1.f, float RootMotionScale = 1.f,
		FOnBossMontageFinished Callback = FOnBossMontageFinished());

	// 主蒙太奇"播完"定时器回调（触发方为 PlayBossMontage 设定的定时器）
	void HandleBossMontageFinished();

	// 主动停掉当前主蒙太奇 + 取消"播完"定时器（切段 / 收招 / 被打断时调用）
	void StopBossMontage();

	// ---------- 平滑转向 ----------
	// 出招前要转向玩家。直接 SetActorRotation 会让 BOSS 在那一瞬间"啪"地拧过去
	// （出招动作里最出戏的一处硬切），所以改成登记"想朝哪"，由 Tick 恒速插值转过去。
	UFUNCTION(BlueprintCallable, Category = "Boss|Animation")
	void SetFacingTarget(const FVector& WorldLocation, float TurnSpeedDegPerSec = 720.f, float MaxTurnTime = 0.35f);

	UFUNCTION(BlueprintCallable, Category = "Boss|Animation")
	void ClearFacingTarget();

	// 当前血量比例（Brain 判阶段 / 大招门槛用）
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Vital")
	float GetHealthRatio() const;

	// ---------- 范围冲击（突进斩收尾 / 大招收尾共用）----------
	//
	// 以自身为圆心做球形范围查询，对每个敌对目标结算一次 ApplyHit，并按需击退。
	//
	// HalfAngleDeg = 判定扇形的半角（度）：
	//   · 180 → 整圈，不做方向过滤（大招收尾用：躲不掉的收束技）
	//   · 75  → 只打正前方 ±75°，玩家可以靠"往侧后方绕"躲开（突进斩收尾用）
	//   · <= 0 → 视为整圈（防呆）
	// ImpactDirection 只在扇形模式下有意义；零向量时用角色正前方。
	//
	// 返回实际命中的目标数（用于日志 / 调参）。
	UFUNCTION(BlueprintCallable, Category = "Boss|Combat")
	int32 ApplyAreaImpact(float Radius, float HalfAngleDeg, const FVector& ImpactDirection,
		float Damage, float PoiseDamage,
		bool bCanBeParried, bool bCanBeBlocked,
		float KnockbackStrength, float KnockbackUpward,
		EDemoDamageSource DamageSource = EDemoDamageSource::HeavyImpact,
		bool bDrawDebug = false);

	// 是否处于霸体（出招中 + 该招式允许霸体）
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Combat")
	bool HasSuperArmor() const;

	// 当前招式是否可被【完美弹反打断动画】。重型招式（突进斩/大招）返回 false：
	// 即使被完美弹反也只免伤+奖励，招式继续，玩家必须改走闪避。
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Combat")
	bool IsCurrentAttackInterruptible() const { return bCurrentAttackInterruptibleByParry; }

	// 由 UGA_BossAttackBase::ActivateAbility 在起手时按招式数据写入
	void SetCurrentAttackInterruptible(bool bVal) { bCurrentAttackInterruptibleByParry = bVal; }

	// 由 Brain 在阶段切换时调用：打上二阶段标记 + 施加狂化 GE
	UFUNCTION(BlueprintCallable, Category = "Boss")
	void EnterPhase(int32 PhaseIndex, bool bIsFinalPhase, TSubclassOf<UGameplayEffect> EnrageEffect);

	// 根位移模式自愈（与玩家角色同一套逻辑）：
	// NoRootMotionExtraction 会让动画位移留在 Mesh 上 → 播完"弹回原位"，且胶囊体没真动。
	UFUNCTION(BlueprintCallable, Category = "Boss|Animation")
	void EnsureRootMotionExtraction(const FString& Reason);

	// 当前是否正在播某个套路招式（Brain 判断"能不能打断"用）
	// 注意：包含【预警红光 Telegraph】+【真正出招 Active】+【后摇 Recover】整个过程。
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Combat")
	bool IsAttacking() const;

	// ⚠⚠ 只判断"真正挥出那一击"（Attack 状态），【不含预警红光 Telegraph】（2026-10-02 第十八轮）。
	//
	// 用途：移动逻辑据此决定"要不要定身"。
	//   Telegraph（预警红光）→ 返回 false → BOSS 继续向玩家逼近【并播行走动画】（压迫感 + 可预判落点）
	//   Attack（真正出招）  → 返回 true  → BOSS 定身站着打（否则会出现"整体平移、腿不动"）
	// 两个需求本来是互斥的（前几轮为了不乱摇一度让出招期间完全不管移动），这里拆成两个状态就都满足了。
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Combat")
	bool IsStrikeActive() const;

	// 由 BOSS 技能在起手时调用：清零复仇计数（数的是"上一次出招之后"又被揍了几下）
	UFUNCTION(BlueprintCallable, Category = "Boss")
	void ResetRevengeCounter() { HitsSinceLastAttack = 0; }

	// ---------- 起手预警红光 ----------
	// BOSS 每次出招前的 Telegraph 阶段会全身泛红并"越闪越急"：
	// 玩家不看预警条也能凭节奏判断"还要多久出招"，读招成本大幅下降。
	//
	// 三层叠加（任一层缺失都能优雅降级，不会报错也不会看不见）：
	//   ① 覆盖材质（OverlayMaterial）—— 真正的"身上泛红"，需要 /Game/Boss/VFX/MI_BossFlash
	//   ② 红色点光 —— 照亮 BOSS 本人和脚下地面，零资产依赖，永远有效
	//   ③ 胸口红光爆闪 —— 复用 ADemoImpactBurst，起手瞬间"炸"一下
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Telegraph")
	bool bTelegraphFlashEnabled = true;

	// 红光点光的峰值强度 / 半径（越大越像"整个人烧起来了"）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Telegraph")
	float TelegraphFlashLightIntensity = 12000.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Telegraph")
	float TelegraphFlashLightRadius = 1000.f;

	// 起手瞬间在胸口炸一下的红光半径（0 = 不要这一层）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Telegraph")
	float TelegraphFlashBurstRadius = 70.f;

	// 覆盖材质（留空时按约定路径 /Game/Boss/VFX/MI_BossFlash 兜底加载）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Telegraph")
	TObjectPtr<UMaterialInterface> TelegraphFlashMaterial;

	// 把 BOSS 的显示名强制成配置里的名字（绕开蓝图 CDO 里存着的旧值 "Sword Boss"）
	void EnsureBossDisplayName();

	// ---------- 受击闪白（"砍到 BOSS 的感觉"核心反馈）----------
	// 不管 BOSS 是否霸体 / 是否在出招，每次被命中都"啪"地闪一下白光 + 原有的火花，
	// 给玩家明确的"我这一刀确实砍进去了"的视觉确认。否则霸体期间 BOSS 只爆个火花、
	// 不僵直也不闪，玩家就感觉"伤害在掉但没砍到"。
	// 用独立点光（和预警红光分开），不抢占 OverlayMaterial 槽位，也不会和红光冲突。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Hit")
	bool bHitFlashEnabled = true;

	// 受击白光的峰值强度 / 半径 / 持续时间（一帧级的脉冲，靠 UpdateHitFlash 衰减归零）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Hit")
	float HitFlashLightIntensity = 9000.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Hit")
	float HitFlashLightRadius = 700.f;

	// 预警红光的基础半径（2026-10-02 新增）：实际半径会按【当前姿势的骨骼包围盒】自动缩放
	// （= 体型大的 BOSS 也能整体被染色），这个值只作下限/基准。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Telegraph")
	float TelegraphFlashRadius = 700.f;

	// ---------- 预警红光"贴身"参数（2026-10-04）----------
	//
	// 用户反馈"预警红光没有贴合模型"。真因【不是灯位】—— 灯位每帧已经对准躯干中心了
	// （UpdateFlashLightPositions），而是【半径大得离谱】：
	// TelegraphFlashRadius=700 被当作 clamp 的下限用，于是红光点光的衰减半径是 7 米，
	// 把 BOSS 周围的地面和空气全都染红 → 画面上就是"一大坨红光罩着模型"，
	// 而不是"模型自己在发光"。点光源天生照亮范围内所有表面，想贴合只能把半径收到躯干尺寸。
	//
	// 红光本体还有一层 OverlayMaterial（逐像素贴在网格上）负责真正的"轮廓在烧"，
	// 点光只留一点点贴身染色，两层加起来才是"贴着模型"。
	//
	// ⚠ 全部用【新属性名】：C++ 的默认值压不过蓝图 CDO 里已经存下的旧值。
	//   半径 = 躯干半高 × TelegraphLightFitScale，再夹在 [Min, Max] 之间。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Telegraph")
	float TelegraphLightFitScale = 0.7f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Telegraph")
	float TelegraphLightFitMinRadius = 70.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Telegraph")
	float TelegraphLightFitMaxRadius = 230.f;

	// 半径收小之后强度也要跟着降：点光的观感亮度大致随半径平方变化，
	// 7 米半径配 12000 收成 2 米半径就得按平方缩，否则会变成"贴脸爆白"。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Telegraph")
	float TelegraphLightFitIntensity = 2600.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Hit")
	float HitFlashDuration = 0.12f;

	// 命中 / 格挡时在 BOSS 身上炸一个"爆闪球"（ADemoImpactBurst，引擎基础球体网格）。
	//
	// ⚠ 2026-09-29 默认关闭：这个球用的是 /Game/Boss/VFX/MI_BossImpact，
	//   其父材质 M_BossSpriteAdd 在 PCD3D_SM6 上【编译失败】→ 引擎回退成默认材质 →
	//   屏幕上就是"BOSS 头顶 / 胸口挂着一个灰球"（用户明确要求去掉）。
	//   命中反馈已由『受击白光 HitFlashLight + 伤害飘字 + HUD 血条』三条通道承担，
	//   关掉它不损失任何读招信息。
	//   等材质修好（编辑器里跑一次 Tools/FixBossVFXMaterial.py）再把这里勾回来即可。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Hit")
	bool bBossImpactBurstEnabled = false;

	// ---------- 失衡（UNSTABLE）表现：绿色轮廓 ----------
	// 状态信号规范的第二条通道（第一条是 HUD 的浮动标签）：
	// 参考 Demo 在异常条打满时把 BOSS 的材质轮廓变绿，玩家不用看血条就知道"它失衡了"。
	// 留空时按约定路径 /Game/Boss/VFX/MI_BossUnstable 兜底加载；
	// 该资产由 Tools/SetupBossVFX.py 生成（和 MI_BossFlash 同族，只是颜色参数不同）。
	// 拿不到材质就优雅降级：不挂覆盖材质，只有 HUD 标签那一条通道 —— 不会崩、不会变黑。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Unstable")
	TObjectPtr<UMaterialInterface> UnstableOverlayMaterial;

	// 失衡轮廓的呼吸强度（越大越刺眼）
	// ⚠ 2026-10-04：改名 UnstableOverlayIntensity → UnstableGlowIntensity 并从 2.5 降到 0.9。
	//   改名原因同上：BP_Boss 的 CDO 里存过旧值，不改名 C++ 的新值不生效。
	//   降值原因：失衡绿轮廓和起手红光共用 M_BossFlashOverlay，2.5 在加法混合下
	//   会把 BOSS 整个烧成一团绿雾（玩家反馈"绿光没有贴合模型"，其实就是被光吞了）。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Unstable")
	float UnstableGlowIntensity = 0.9f;

	// ---------- 位移安全阀（BOSS 不许被"打飞 / 推着走"的最后一道闸）----------
	//
	// 前几轮分别堵过：突进速度、击退上抛、受击蒙太奇的根位移。
	// 但仍然可能有漏网的推力（典型的是【胶囊体互相穿透后的解穿透】：
	// UE 默认允许一次最多把角色弹开 32cm，而且是瞬间位移 ——
	// 一帧位移 32cm ≈ 1900cm/s，看起来就是"被撞飞 / 被推着滑"）。
	//
	// 这道闸不猜来源，直接看结果：**BOSS 每一帧的位移不许超过它自己的合法速度**。
	// 合法速度 = 出招中 1500cm/s（略高于突进斩的 1100）/ 平时 MaxWalkSpeed×1.4。
	// 超了就把这一帧的位置拉回到"最多只能走到这里"，并打一条日志说明方向和大小。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Combat")
	bool bDisplacementGuardEnabled = true;

	// 出招期间允许的位移速度上限（cm/s）。必须 > 突进斩速度（1100），否则突进会被自己截断
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Combat")
	float GuardAttackMaxSpeed = 1500.f;

	// 非出招时：允许速度 = MaxWalkSpeed × 这个系数（1.4 是给斜坡 / 起步加速留的余量）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Combat")
	float GuardPassiveSpeedMargin = 1.4f;

	// 坠落时不夹（重力下落速度远大于上面的阈值，夹了 BOSS 会悬在空中）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Combat")
	float GuardFallingMaxSpeed = 3000.f;

	// ---------- 落地广播 ----------
	UPROPERTY(BlueprintAssignable, Category = "Boss|Events")
	FDemoOnBossLanded OnBossLanded;

	// ---------- 蓝图表现钩子（特效 / 音效 / 材质闪白 / 震屏）----------
	UFUNCTION(BlueprintImplementableEvent, Category = "Boss")
	void OnBossPhaseEntered(int32 PhaseIndex, bool bIsFinalPhase);

	// 霸体受击：只做轻微反馈，不打断动作
	UFUNCTION(BlueprintImplementableEvent, Category = "Boss")
	void OnBossSuperArmorHit(float Damage);

	// 范围冲击已结算：在这里放尘土特效、震屏、冲击波 Decal
	UFUNCTION(BlueprintImplementableEvent, Category = "Boss")
	void OnBossLandedImpact(const FVector& ImpactLocation, float Radius);

	// 突进斩冲出去的那一下（起步）。做残影 / 破风特效 / 地面扬尘挂在这里。
	UFUNCTION(BlueprintImplementableEvent, Category = "Boss")
	void OnBossChargeStart(const FVector& Direction, float Distance);

	// 受击表现（方向、伤害）—— 想做整身闪白 / 音效就实现它
	UFUNCTION(BlueprintImplementableEvent, Category = "Boss")
	void OnBossHitReact(EDemoHitDirection Direction, float Damage);

	// 调试绘制：招式距离带、落地半径、目标连线
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Boss|Debug")
	bool bDrawDebugCombat = false;

	// 由 Brain 在每次"下令移动"时调用：把时间戳写进 LastMoveCommandTime，
	// 让底层行走动画跟着【移动意图】走，而不是跟着 AI 直驱下逐帧 0↔最大 抖动的原始速度
	// （原始速度抖动会让"腿冻住(Idle)↔动(Jog)"反复翻转 → 表现为"模型不动整体滑动，但有时又动"）。
	// 必须 public：Brain（异类）要在 DriveMoveNoNav 里直接调用。
	// CommandedSpeed = Brain 本帧下令的速度（cm/s）。⚠ 必须传：BOSS 自己读 GetVelocity()
	// 在 ACharacter::Tick 里读到的永远是 0（RequestDirectMove 的速度已被 Movement 消费），
	// 拿不到它播放倍速就会长期钉在下限 → 腿慢动作爬、胶囊照常平移 = "只有模型平移没有动画"。
	UFUNCTION(BlueprintCallable, Category = "Boss|Animation")
	void NotifyBossMoveCommand(float CommandedSpeed);

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;
	virtual void OnDamagedReaction(EDemoHitDirection Direction, float Damage) override;
	virtual void OnDeathReaction() override;
	virtual void Landed(const FHitResult& Hit) override;

	UFUNCTION()
	void HandleBossPoiseBroken();

	// 破防（韧性清零）后的虚弱窗口到点 → 摘掉硬直标签，Brain 自动从 Stagger 回到 Chase。
	// 做成独立函数而不是内联 Lambda，是为了能在 EndPlay / 死亡时安全地清掉，不留下野定时器。
	void RecoverFromPoiseBreak();
	FTimerHandle PoiseBreakRecoverTimerHandle;

	void PlayMontageOnMesh(UAnimMontage* Montage, float PlayRate = 1.f);

	// ---------- 受击/虚弱蒙太奇：不许推着 BOSS 走 ----------
	// 四向受击、虚弱、硬直这些蒙太奇的源动画都取自 SwordAnimsetPro 的 Root-Motion 目录，
	// 天生带根位移 —— 播一次就等于把胶囊体推着滑一段。
	// 结果是"我砍一刀 BOSS 就退一大截"，玩家能硬生生把 BOSS 推着满场走。
	// 这些【被动反应类】蒙太奇必须在播放期间关掉根位移：动作照播，人不许动。
	void PlayReactionMontage(UAnimMontage* Montage, float PlayRate = 1.f);

	// 每帧自愈：受击蒙太奇播完 / 被出招打断 → 立刻把根位移通道还回去，
	// 绝不让"关掉过"这件事影响下一招的位移。
	void UpdateReactionRootMotionGuard();

	bool IsReactionMontage(const UAnimMontage* Montage) const;

	// ---------- 起手预警红光 ----------
	void UpdateTelegraphFlash(float DeltaTime);

	// 受击闪白：每次被命中调用一次（无论是否霸体），由 UpdateHitFlash 衰减。
	void TriggerHitFlash();
	void UpdateHitFlash(float DeltaTime);

	// 失衡态的绿色轮廓（State_Unstable 期间持续挂覆盖材质，退出即摘掉）
	void UpdateUnstableVisual(float DeltaTime);

	// ---------- 默认格挡（P1-4：让 BOSS 从沙袋变成博弈对象）----------
	//
	// 没有它的时候，玩家随时都能把伤害打进去，"击破窗口"和"后摇惩罚窗口"就都没有稀缺性
	// —— 既然站着砍也行，为什么要费劲去打满异常条？整个反馈链因此塌掉一半。
	//
	// 举盾时机刻意只放在【中立状态】（Idle / Chase / Reposition）：
	//   起手(Telegraph) / 出招(Attack) / 后摇(Recover) / 硬直(Stagger) / 击破(Broken) 一律不挡，
	//   于是"BOSS 正在做事"的那几段时间天然就是玩家的输出窗口，
	//   玩家要做的是【读它什么时候在做事】，而不是无脑贴脸。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Guard")
	bool bGuardByDefault = true;

	void UpdateGuardStance();

	// 举盾的可见姿态：没有它玩家只能从"伤害变 25%"去反推 BOSS 在防御，
	// 而伤害数字是最不可靠的读数（血厚、格挡、抗性都会让数字变小）。
	// 冷蓝轮廓 + 护体光 + 被挡瞬间的爆闪，是"它在防御"这件事的第三、四条通道。
	void UpdateGuardVisual(float DeltaTime);

	// 举盾姿态蒙太奇的播放/停止（循环，只在 Idle 举盾时播；用蒙太奇自带混合出时间收刀）
	void PlayGuardPose();
	void StopGuardPose();

	// 举盾覆盖材质（冷蓝描边）。留空时按约定路径 /Game/Boss/VFX/MI_BossGuard 兜底加载；
	// 该资产由 Tools/SetupBossVFX.py 生成（与 MI_BossFlash 同族，只改颜色参数）。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Guard")
	TObjectPtr<UMaterialInterface> GuardOverlayMaterial;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Guard")
	float GuardOverlayIntensity = 1.2f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Guard")
	float GuardLightIntensity = 1200.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Guard")
	float GuardLightRadius = 300.f;

	// 被格挡的撞击音。留空时按约定路径 /Game/Boss/SFX/SC_BossBlock 兜底；
	// 音频资源缺失不会报错，只是静音 —— 视觉通道（火花/爆闪/轮廓）仍然完整。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Guard")
	TObjectPtr<USoundBase> BlockSound;

	// 举盾姿态蒙太奇（循环）：站着(Idle)举盾时播，让"它在防御"有可见的肢体语言，
	// 不再只有轮廓+光。留空时按约定路径 /Game/Boss/Animation/Boss_Guard_Idle_Montage 兜底
	// （Tools/SetupBossMontages.py 生成，取材 Block_Hold_Anim）。移动中(Chase/Reposition)不播，
	// 避免全身蒙太奇抢掉腿部位移 —— 那时靠轮廓+护体光表达"在防御"。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Guard")
	TObjectPtr<UAnimMontage> GuardMontage;

	// 重写基类事件：被玩家压着打（连续格挡）到阈值就反制一次
	virtual void OnBlockedIncomingHit_Implementation(AActor* Attacker) override;

	// 连续挡下玩家多少次（反制后清零；举盾中断也清零）
	int32 BlockedStreak = 0;

	// ---- 举盾表现 ----
	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> GuardMID;

	UPROPERTY()
	TObjectPtr<UPointLightComponent> GuardLight;

	bool bGuardVisualApplied = false;
	float GuardVisualTime = 0.f;

	// 被挡瞬间的护盾爆闪（额外亮度，0.25s 衰减）
	float GuardImpactFlashRemaining = 0.f;

	// 当前是否在举盾（避免每帧重复 Add/Remove tag）
	bool bGuardActive = false;

	// 举盾姿态蒙太奇当前是否在播（避免每帧重复 Montage_Play）
	bool bGuardPosePlaying = false;

	// 举盾蒙太奇是加法动画这件事只警告一次（避免每帧刷屏）
	bool bGuardAdditiveWarned = false;

	// 当前招式是否可被完美弹反打断（由 UGA_BossAttackBase 起手时写入；见 IsCurrentAttackInterruptible）
	bool bCurrentAttackInterruptibleByParry = true;

	// ---------- 位移安全阀 ----------
	void UpdateDisplacementGuard(float DeltaTime);

	// ---------- Mesh ↔ 胶囊体贴地对齐（每帧）----------
	// 目标：无论蓝图里 Mesh 的 Z 偏移被谁改成什么样，BOOT 都要【踩在地面上】。
	//
	// ⚠⚠⚠ 2026-10-12 决定性修复：用户录屏显示 BOSS 整体悬空（脚底离地一大截），
	//   视觉上就是"移动时模型不动、贴着地面平移"。原因有两个，都已修：
	//   ① 原来只在 BeginPlay 校准一次、且用【参考姿势】Bounds →
	//      动画换成行走/攻击姿势后最低点变了却没人复查 → 越动越悬空。现在【每帧】校准。
	//   ② 原来用 `AddWorldOffset` 修正，它不写回相对位移、每次调用都继续累加 → 网格漂走。
	//      现在用 `SetRelativeLocation`（幂等），并按【当前动画姿势的真实脚骨】算最低点。
	//
	// 保留 MeshAlignmentTolerance：偏差在这个范围内完全不动，避免每帧抖。
	void UpdateMeshGroundAlignment();
	void EnsureMeshCapsuleAlignment();

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Mesh")
	float MeshAlignmentTolerance = 2.f;

	// 贴地修正的最大速度（cm/s）。限速而不是一次性拉到底：
	// 出生/传送/换姿势的瞬间猛地一弹看起来更糟，快速平滑地贴地才自然。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Mesh")
	float MeshAlignmentMaxSpeed = 600.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Mesh")
	float MeshAlignCheckInterval = 1.f;

	FTimerHandle MeshAlignTimerHandle;

	// 命中火花（C++ 兜底）：每次被命中都在胸口爆一团，确认伤害确实进了（不依赖蓝图受击事件）
	void SpawnBossHitSpark() const;

	// 播蒙太奇并把动画根位移模式掰正（Boss 和玩家一样需要真位移，不要"拉回"）
	void PlayCombatMontage(UAnimMontage* Montage, float PlayRate = 1.f);

	// ---------- 行走【蒙太奇】（2026-10-02 第十六轮新增，优先于上面的序列）----------
	//
	// ⚠⚠⚠ 为什么不用序列、改用蒙太奇（这是"移动时没有动画、整体在地面平移"的结构性根因）：
	//   单节点模式 `PlayAnimation(AnimSequence, true)` 有两个先天缺陷：
	//     ① 单节点实例【只有 SetAnimationAsset 的对象是蒙太奇时才会注册它的 Slot 节点】。
	//        序列没有 Slot → DefaultSlot 不存在 → 攻击/受击蒙太奇 Montage_Play 静默失败。
	//     ② 行走用序列、招式用蒙太奇，两者【不在同一条 Slot 通道】里：
	//        招式一来就把行走整个盖掉（腿停摆），招式一结束又从第 0 帧重播（步态走不完）。
	//   改成"行走也用蒙太奇"后，行走与招式都挂在 DefaultSlot，走同一条分层通道，
	//   切换由引擎做标准 Slot 混合，步态也能真正循环。
	//
	// 由 Tools/SetupBossWalkMontages.py 从 Khaimera 的 Jog 序列生成（骨架必须是 Khaimera）。
	// 留空则自动回退到上面的序列方案（见 UpdateBossLocomotion）。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Animation")
	TObjectPtr<UAnimMontage> BossIdleMontageAsset;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Animation")
	TObjectPtr<UAnimMontage> BossWalkFwdMontage;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Animation")
	TObjectPtr<UAnimMontage> BossWalkBwdMontage;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Animation")
	TObjectPtr<UAnimMontage> BossWalkLeftMontage;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Animation")
	TObjectPtr<UAnimMontage> BossWalkRightMontage;

	// ---------- 基础动画驱动（待机 / 移动，彻底不依赖任何 AnimBlueprint）----------
	// 之前 BOSS 的 Mesh 被指到了 Khaimera_AnimBlueprint（那是玩家角色 ABP，内部依赖英雄状态变量
	// → 除以零 → 整只 BOSS 完全不动 / 永远 T-Pose）。改为 C++ 单节点模式：直接循环播【待机】，
	// 移动时切到【Jog】，攻击 / 受击 / 破防 / 死亡走蒙太奇盖在上面。
	// 好处：零手动编辑器操作、彻底绕开坏掉的 ABP、编译即生效。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Animation")
	TObjectPtr<UAnimSequence> BossIdleAnimation;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Animation")
	TObjectPtr<UAnimSequence> BossJogAnimation;

	// 方向化行走（前后左右四向 Jog）：按"移动方向相对自身朝向"选对应序列，
	// 绕圈 / 后撤时不再全程播前冲，否则画面上"腿往前跑、人往旁边/往后溜"= 溜冰/倒滑感。
	// 任一方向素材缺失 / 是加法动画 → 留空，运行时 PickLocoJogSequence 回退到前冲。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Animation")
	TObjectPtr<UAnimSequence> BossJogBwdAnimation;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Animation")
	TObjectPtr<UAnimSequence> BossJogLeftAnimation;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Animation")
	TObjectPtr<UAnimSequence> BossJogRightAnimation;

	// Jog 播放倍速基准速度（cm/s）：实际水平速度 / 此值 = 播放倍速，用来消"脚速≠移速"的打滑。
	// ⚠⚠⚠ 这个值必须是 Jog 动画的【原生地面速度】（支撑相脚相对身体向后蹬的速度），
	//   【不是】MaxWalkSpeed。倍速偏小 → 脚向后蹬的量跟不上身体前进量 → 脚在地上往前滑 = 空步。
	//
	//   ⚠⚠⚠⚠ 2026-10-03：之前这个值 415 是【只测 Jog_Fwd 得到的】，被四方向共用 → 空步。
	//   按主轴自适应重测四方向（Tools/MeasureWalkSpeedAxisAware.py，500 点采样，
	//   自动判断推进轴是 X 还是 Y —— 第一版固定测 Y，对 Strafe 完全测错了，
	 //   因为横移是沿 X 迈的，导致算出"横移原生速度 12cm/s"这种荒谬值）：
	 //
	//     Jog_Fwd      主轴 Y  抬腿 62.6cm  原生 354  → 倍率 1.07
	//     Jog_Bwd      主轴 Y  抬腿 31.8cm  原生 325  → 倍率 1.17
	//     Strafe_Left  主轴 X  抬腿 11.3cm  原生 364  → 倍率 1.04
	//     Strafe_Right 主轴 X  抬腿 17.6cm  原生 370  → 倍率 1.03
	//
	//   四条都在 325~370，而旧值 415 偏大 12~22% → 倍率偏小 → 脚蹬得慢于身体 → 空步。
	//   换动画 / 换骨架后重跑该脚本刷新下面这四个值。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Animation")
	float BossJogReferenceSpeed = 354.f;      // 前冲 Jog_Fwd

	// 下面三个是【其它方向】各自的原生地面速度。
	// 单独暴露成属性，而不是只留一个总开关 —— 四条动画的步幅本来就不同，
	// 共用一个值必然有一两个方向对不上（这是之前空步反复修不好的根本原因）。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Animation")
	float BossJogReferenceSpeedBwd = 325.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Animation")
	float BossJogReferenceSpeedLeft = 364.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Animation")
	float BossJogReferenceSpeedRight = 370.f;

	// 水平速度超过这个阈值（cm/s）就播 Jog，否则待机
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Animation")
	float BossJogSpeedThreshold = 50.f;

	// ⚠⚠ 迟滞参数（2026-09-30 定案，别删）
	// 速度在阈值上下抖动时，若每帧都在 Idle/Jog 之间切换，每次切换都会 PlayAnimation() 从头播，
	// 动画就永远停在第 0 帧 —— 画面表现为「移动动画没播出来」「一直在重复同一个动作」。
	// 实测：13 秒的 PIE 里刷了 1066 次切换。
	//   ① 迟滞：进 Jog 用阈值×1.0，退 Jog 用阈值×LocoJogExitScale（低很多），中间是死区。
	//   ② 驻留：切换后 LocoMinDwellTime 秒内不允许再翻面。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Animation")
	float LocoJogExitScale = 0.5f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Animation")
	float LocoMinDwellTime = 0.4f;

	// 距上次切换位移动画过了多久（内部计时器，非配置）
	float LocoDwellTimer = 0.f;

	// 速度低通滤波值（内部状态，非配置）：AI 直驱下 GetVelocity() 会逐帧 0↔最大速度抖动，
	// 直接用原始速度做 Idle/Jog 判定会让腿在"冻住(Idle)↔动(Jog)"之间翻转 → 表现为
	// "模型不动整体滑动，但有时又会有移动动作动画"。用平滑速度吸收抖动，持续移动时不再掉回 Idle。
	float SmoothedLocoSpeed = 0.f;

	// ★ 判定"真的在不在移动"的【唯一可靠依据】：上一帧 Actor 的世界坐标（2026-10-02）。
	// 原因：GetVelocity() 对 RequestDirectMove 驱动的 AI 恒为 0；
	// 而"移动意图"窗口在 Brain 不下令的间隙（转身/出招）就过期 → 两者都会误判成"没在走"，
	// 于是满速移动时每 0.5s 就在 Jog↔Idle 之间翻面，每次翻面都 PlayAnimation 清零播放时间
	// → 腿反复从第 0 帧重来 = 视觉上的"没有走路动画、只是模型平移"。
	// 胶囊的世界位移由 CharacterMovement 直接写入，不受上述任何间接环节影响。
	FVector LastLocoSampleLoc = FVector::ZeroVector;

	// 由上一条实测位移速度做的高强度平滑（仅压单帧噪声，延迟极小）
	float MeasuredLocoSpeed = 0.f;

	// 本帧是否已经推进过 LastLocoSampleLoc。
	// 原因：UpdateBossLocomotion 里"蒙太奇正在播"的分支可能已经提前推进了采样点，
	//   下面再推进一次会让算出来的速度变成两倍（或 0），导致移动判定完全失真。
	bool bLocoSampleAdvanced = false;

	// 本帧的真实世界位移（LastLocoSampleLoc → CurrentLoc，未归一化的向量）。
	//
	// ⚠⚠⚠ 这是【判定"往哪个方向走"的唯一可靠来源】（2026-10-02 第二十轮）。
	// 绝不能改用 GetVelocity()：BOSS 由 AIController 驱动、靠 RequestDirectMove 写速度，
	// 而 ACharacter::Tick 读 GetVelocity() 的时机在 CharacterMovement 消费掉本帧速度【之后】
	// → 实测每帧几乎恒为 0 → 方向判定永远"没在动" → 只播前冲 → 四个方向素材全用不上。
	FVector MoveDeltaThisFrame = FVector::ZeroVector;

	// 方向化 Jog 限频计时（内部状态，非配置）：绕圈走位时移动方向每帧小幅旋转，
	// 若不限频、每帧都按新方向重选 Jog_Fwd/Right/Left/Bwd，会每帧 PlayAnimation 清零进度
	// → 卡在第 0 帧、看着像滑步。限频到 LocoDirSwitchCooldown 切一次，腿循环能连续播完。
	float LocoDirSwitchTimer = 0.f;

	// 方向化 Jog 的切换冷却（秒）：冷却内沿用当前方向资产，避免绕圈时每帧重置动画。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Animation")
	float LocoDirSwitchCooldown = 0.75f;

	// ---------- 行走蒙太奇通道的状态（2026-10-02 第十六轮）----------
	// 行走改用蒙太奇后：行走与招式同挂 DefaultSlot，切换由引擎做 Slot 混合。
	// 下面的成员只用于"要不要换/换到哪"的判定。
	bool bUseMontageLocomotion = false;      // 蒙太奇资产是否就绪（BeginPlay 决定）
	TObjectPtr<UAnimMontage> CurrentLocoMontage;  // 当前正在播的行走蒙太奇
	// 本次选中的行走方向（0=前 1=后 2=左 3=右），
	// 供播放倍速按方向取各自的原生地面速度（325~370，不能共用一个值）。
	int32 CurrentLocoDirIndex = 0;
	// 本帧是否有【除行走蒙太奇以外】的蒙太奇在播（攻击/受击/破防…）。
	// 由 UpdateBossLocomotion 写、ApplyLocomotionForFrame 的诊断日志读。
	bool bOtherMontagePlayingNow = false;

	// Brain 最近一次下令的移动方向（世界空间，XY 平面）。
	// 用途：实测位移方向算不出来时（起步那一帧 / 位移极小）作为方向判定的兜底。
	// ⚠ 绝不能靠 GetVelocity() 拿方向 —— AI 直驱下它恒为 0（见 NotifyBossMoveDirection 注释）。
	FVector LastMoveCommandDir = FVector::ZeroVector;

	// 方向化 Jog 换向的角度死区（度）：新方向与当前方向夹角小于它就不换。
	// 绕圈/侧移时移动方向每帧小幅旋转，若只看"前向分量 vs 右向分量谁大"，
	// 分量在分界线附近来回跳 → 每帧换资产 → 动画反复从头播。50° 死区让腿能连续跑完。
	// 换向死区（度）：与"正在播的方向"夹角小于它就不换向，避免打断步态。
	//
	// ⚠⚠⚠ 2026-10-03 从 50 调到 22 —— 50° 太宽是"太空步"的一个直接来源。
	//   绕圈时移动方向相对自身朝向缓慢旋转，50° 的死区意味着
	//   "方向已经明显变了但动画不换" → 腿还在朝旧方向蹬，身体已经朝新方向平移
	//   → 脚在地上横向拖 = 滑步。
	//   22° 仍够宽容（不会因噪声频繁换向），又能在方向真的变了时及时跟上。
	//   配合下面的 LocoMinDwellTime（驻留）一起防抖，两者分工不同：
	//     · 死区管"角度差多大才换"  · 驻留管"换完多久内不许再换"
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Animation")
	float LocoDirSwitchAngleDeg = 22.f;

	// 【当前正在播的】移动方向（角色局部空间，X=前 Y=右），用于换向角度死区比较。
	// ⚠ 必须是"正在播的"，不能是"上一帧的"：上一帧方向在绕圈时持续缓慢漂移，
	//   拿它做比较，死区照样会被跨过 → 每 0.5s 换向一次 → 播放时间反复归零 → 滑步。
	//   只在真正切换位移动画资产时更新它。
	// ⚠ UE 5.7 里 FVector2D = TVector2<double>，初始化值要写 double 字面量。
	FVector2D PlayingLocoLocalDir = FVector2D(0.0, 0.0);

	// BeginPlay 里初始化动画驱动（单节点兜底 + 动态待机/行走蒙太奇注册 DefaultSlot；或切 BOSS 专用 ABP）
	void SetupBossAnimation();
	// 每帧按移动速度在 待机 / Jog 之间切换（有战斗蒙太奇时有条件地让位，见实现注释）
	void UpdateBossLocomotion(float DeltaTime);

	// 选定并应用本帧的位移动画（Idle / Jog）。
	// 拆出来是为了让两条路径共用同一段"应用"逻辑：
	//   ① 正常路径（本帧自己算速度）
	//   ② 蒙太奇正在播但 BOSS 确实在移动（此时速度已在上面算过，不能重复算）
	// 否则两条路各写一份，很容易出现"一边算了、一边没算"的状态错位。
	void ApplyLocomotionForFrame(bool bMovingNow, float DeltaTime);

public:
	// Brain 每帧下发移动方向用（详见下面 NotifyBossMoveDirection 的注释）。
	// 放在 public 段：UBossBrainComponent 要调它，而它上面处于 protected 段 → C2248。
	void NotifyBossMoveDirection(const FVector& InDir);

protected:
	// 按"移动方向相对自身朝向"选前后左右 Jog（绕圈/后撤不再播前冲）；素材缺失则回退前冲。
	// ⚠ 不能是 const 函数：它要写 LastLocoLocalDir（换向角度死区的记忆状态）。
	//   返回 nullptr = 新方向还在死区内、调用方应保留当前正在播的那个，别打断步态。
	UAnimSequence* PickLocoJogSequence();
	// 【蒙太奇版】的同款选择（2026-10-02 第十六轮新增：行走改走蒙太奇通道时用）。
	// 返回 nullptr = 新方向还在换向死区内，调用方保留当前正在播的那个。
	UAnimMontage* PickLocoWalkMontage();

	// ⚠⚠⚠ Brain 每次下令移动时调用，把【方向】传下来（2026-10-02 决定性修复）
	//
	// 为什么必须由 Brain 传：方向化行走需要知道"此刻往哪走"，
	// 但角色自己的 GetVelocity() 在 AI 直驱（RequestDirectMove）下【恒为 0】——
	// CharacterMovement 在本函数被调用的时机已经消费掉了本帧速度。
	// 实测后果：方向判定永远等于"没在动" → 永远 return 前冲 →
	//   整局 21 帧行走动画【全部】是 Boss_Walk_Fwd_Montage，绕圈/后撤也在往前跑 = 溜冰。
	// Brain 是方向的【下达者】，由它传下来最准。
	//（本函数的声明已提到上面的 public 段，供 UBossBrainComponent 调用。）
	// Jog 播放倍速 = 实际水平速度 / 基准速度，消"脚速≠移速"的打滑（夹在 0.6~2.5）
	//
	// ⚠⚠⚠ 2026-10-03：新增【方向】参数。
	//   四条行走动画的原生地面速度本来就不一样（Jog_Fwd 354 / Jog_Bwd 325 /
	//   Strafe_Left 364 / Strafe_Right 370），之前四方向共用一个 415，
	//   偏大 12~22% → 倍率偏小 → 脚蹬得比身体慢 → 空步。这是最难缠的那类 bug：
	//   逻辑读起来完全正确，数值却是错的。
	//   传 0 / 越界都退化成用前冲的值（安全兜底）。
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Boss|Animation")
	float ComputeLocoPlayRateForDir(float Speed2D, int32 LocalDirIndex) const;

	// 旧接口保留：等价于"前冲方向"（LocalDirIndex = 0）
	float ComputeLocoPlayRate(float Speed2D) const;

	// 招式动画体检：开局遍历所有阶段的招式，列出"没资产" / "加法动画(播不出)"的招。
	// 存在的意义：直接回答"BOSS 这一拍到底有没有动作动画"，不用靠肉眼在游戏里试。
	void AuditAttackMontages();

	// 动画可见性自检（DebugDump，不是游戏逻辑）：
	// 每 0.5s 打一条 —— 单节点播放时间有没有推进、GAS 蒙太奇有没有在播、
	// 关键骨骼（手部）相对角色的位置有没有变。
	// 存在的意义：日志说"切到 Jog 了"不等于画面在动，只有骨头在动才算数。
	void DebugDumpAnimState(float DeltaTime);
	float AnimDebugTimer = 0.f;
	int32 DebugBoneIndex = INDEX_NONE;
	FVector LastDebugBoneLocalPos = FVector::ZeroVector;
	// 上一帧脚骨的【世界】坐标（诊断用：直接看脚有没有在世界里朝移动方向迈步）
	FVector LastDebugFootWorld = FVector::ZeroVector;
	// 开局资产体检只打一次（避免刷屏）
	bool bLocoAuditLogged = false;

	// 供 anim自检 日志使用：最近一帧的"移动判定"结果。
	// ⚠ 必须和资产名打在同一行！否则要靠时间戳把两条日志配对，
	//   而它们是各自独立 0.5s 节流的，配对会错位 → 得出错误结论。
	float DebugMeasuredSpeed = 0.f;
	bool DebugMoveIntent = false;
	bool DebugPhysMoving = false;
	float LastDebugNodeTime = 0.f;

	// 运行期由待机/行走序列动态生成的【循环蒙太奇】，挂在 DefaultSlot 上。
	// ⚠ 这两个动态蒙太奇目前【只剩历史意义】：底层待机/行走已经改回直接播序列
	// （原因见 CurrentLocoAsset 的注释）。留着是因为其他表现通路可能还引用得到，
	// 删掉要连带改一串代码，收益不大。
	UPROPERTY()
	TObjectPtr<UAnimMontage> BossIdleMontage;

	UPROPERTY()
	TObjectPtr<UAnimMontage> BossJogMontage;

	// 当前底层循环播放的待机/行走资产 —— 是【序列】，不是蒙太奇。
	//
	// ⚠⚠ 为什么从"循环蒙太奇"改回"序列"（2026-09-30 定案，别改回去）：
	//   实测（真实 PIE，骨骼级自检）：循环动态蒙太奇当 CurrentAsset 时
	//   `GetCurrentTime()` 恒为 0.00、骨头一动不动；而所有【非循环】蒙太奇
	//   （攻击/受击/破防/死亡，全部走 PlayAnimation(..., bLoop=false)）时间正常推进。
	//   也就是说：单节点下"循环蒙太奇"这条组合是坏的，而"序列循环"是单节点最原始的通道。
	//   于是待机/行走直接播序列，绕开这个坑。
	//   （附带好处：BOSS 的表现动画本来就都走 PlayAnimation 设为 CurrentAsset，
	//     不再依赖蒙太奇槽位分层，"必须先注册 DefaultSlot" 那套前提也不需要了。）
	UPROPERTY()
	TObjectPtr<UAnimationAsset> CurrentLocoAsset = nullptr;

	// 单节点主蒙太奇"播完"回调机制所需的状态（替代被废弃的 UAbilityTask_PlayMontageAndWait）
	FOnBossMontageFinished ActiveMontageFinishedCallback;
	TWeakObjectPtr<UAnimMontage> ActivePlayedMontage = nullptr;
	bool bMontageFinishCallbackArmed = false;
	FTimerHandle BossMontageFinishedTimer;

	// BOSS 专用极简动画蓝图（骨架必须 = Khaimera_Skeleton，否则蒙太奇一个都播不了）。
	// 为什么需要它：之前用单节点模式（SetAnimationMode(AnimationSingleNode)）虽然能循环播待机，
	// 但单节点实例【只注册被 Set 进去的那个资产的 Slot】，待机是个序列、没有 Slot，
	// 于是所有 BOSS 攻击蒙太奇（AnimMontageFactory 默认挂在 "DefaultSlot"）找不到对应 Slot 节点
	// → Montage_Play 静默失败 → BOSS 攻击/受击/破防/死亡全都不播动画。
	// 换成真正的 AnimBlueprint 后，动画图里放一个 Slot 'DefaultSlot' 节点，
	// Idle/Jog 走动画图、攻击/受击/破防/死亡蒙太奇盖在 Slot 上，分层就正常了。
	// ⚠ 这个 ABP 必须在编辑器里手工建（Python 造不了 AnimGraph 节点），步骤见交付说明。
	//   没建 / 加载失败时退回单节点待机兜底，BOSS 至少不会 T-Pose。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Animation")
	TSoftClassPtr<UAnimInstance> BossAnimBlueprint;

	// 运行时是否已成功切到 BOSS 专用 ABP（切到了就由 ABP 自己按速度切 Idle/Jog，C++ 不再插手）
	bool bUsesBossAnimBP = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Boss")
	TObjectPtr<UBossBrainComponent> Brain;

	// 四类招式对应的技能（蓝图里配；留空走约定路径兜底）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Abilities")
	TMap<EBossAttackType, TSubclassOf<UGameplayAbility>> AttackAbilityClasses;

	// ---------------- 受击表现 ----------------
	// 按受击方向播不同蒙太奇（正面 / 背面 / 左侧 / 右侧）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Hit")
	TMap<EDemoHitDirection, TObjectPtr<UAnimMontage>> HitReactMontages;

	// 方向表没配时的兜底受击蒙太奇
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Hit")
	TObjectPtr<UAnimMontage> HitReactMontage;

	// 破防（韧性清零）三段表现：进虚弱 → 虚弱待机（循环）→ 恢复
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Hit")
	TObjectPtr<UAnimMontage> WeakMontage;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Hit")
	TObjectPtr<UAnimMontage> WeakLoopMontage;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Hit")
	TObjectPtr<UAnimMontage> WeakRecoverMontage;

	// 被弹反时的大硬直（可留空，退化用 WeakMontage）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Hit")
	TObjectPtr<UAnimMontage> StaggerMontage;

	// 受击分级阈值（最终伤害，已含减免）：低于此值算"轻击踉跄"（短硬直 + 较快收招），
	// 高于等于此值算"重击"（整段硬直）。玩家普攻连段 28~56、技能 120，故 70 能干净分开
	// "轻连段" 与 "重技能"。想让更多命中走"轻踉跄"就调高，想更"脆"就调低。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Hit")
	float BossLightHitDamageThreshold = 70.f;

	// 轻击踉跄的硬直时长相对 BossHitstunTime 的缩放（值越小越短，越不像"被定身"）。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Hit")
	float BossLightHitstunScale = 0.45f;

	// ⚠⚠⚠ 硬直【冷却】（秒）—— 2026-10-03 新增，治"一顿一顿的地面平移"。
	//
	// 实测（心跳状态分布）：Stagger 硬直占 31%，和 Chase 追击一样多！
	// 也就是 BOSS 有一半时间在被定身 → 每挨一刀就停 0.28s →
	// 移动变成"走一截、停一下、再走一截" → 行走动画永远走不完一个完整步态
	// → 玩家看到的正是【太空步 / 地面平移】。
	//
	// 动作游戏里硬直是【偶尔的奖励窗口】，不是常态。
	// 这个冷却保证两次定身之间至少间隔这么久，期间照样播受击动画（视觉有反馈）
	// 但【不定身】，BOSS 能连续走位 —— 移动动画才连贯。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Hit")
	float StaggerCooldown = 2.2f;
	double LastStaggerTime = -999.0;

	// ---------- BOSS 格挡（概率 + 冷却 + 保底，2026-10-03 重做）----------
	//
	// 需求原文："boss 也可以格挡角色的攻击，不要太频繁，要有蒙太奇动画"。
	//
	// ⚠⚠⚠ 为什么整体重写（第一版实测【一次都没触发】，日志佐证）：
	//   ① 判定时机错了：原来在 OnDamagedReaction 里掷骰子 —— 那是【伤害结算之后】的回调，
	//      此时血已经掉了。结果就是"举盾动画播了，血照掉"，表现和逻辑对不上；
	//      更糟的是它压根没被判定成"格挡"（ApplyHit 只看 State.Blocking 标签），
	//      所以受击白闪 / 硬直 / 削韧全都照常发生 → 玩家看到的是"挨打"，不是"挡住"。
	//   ② 概率太低：0.18 概率 + 4s 冷却 + 还要求"BOSS 不在出招/起手中"，
	//      三个条件相乘 ≈ 一场遭遇战里 0~1 次。实测一整局只触发 1 次（14:58:55）。
	//
	// 现在的做法：
	//   · 判定挪到【伤害结算之前】（ADemoCharacterBase::TryGuardIncomingHit，由 ApplyHit 调用），
	//     所以格挡是真的减伤（伤害 ×0.25 + 扣攻方架势），再叠表现。
	//   · 概率 + 冷却 +【保底】：连续 BossGuardPityHits 次"有资格格挡"的命中都没随机上，
	//     下一次强制格挡 —— 保证玩家一场里一定看得到 BOSS 格挡。
	//
	// ⚠ 属性全部换成了新名字（老 BossBlockChance / BossBlockCooldown 已删）。
	//   原因：BP_Boss 的 CDO 里已序列化过旧默认值，沿用旧名字改 C++ 默认值【无效】
	//   （CDO 值优先于构造函数默认值），换名才能让新默认值生效。

	// 每次"有资格格挡"的命中，成功格挡的概率。
	//
	// ⚠⚠ 2026-10-04：下面三个参数是【改名】改的，不是只改数值。
	//   原因：蓝图 BP_Boss 的 CDO 里存着旧属性名（BossGuardChance 等）的值，
	//   C++ 构造函数/默认成员初始化的新值【压不过 CDO】（这个坑本项目踩过两次）。
	//   属性改名 = CDO 里那条旧记录作废 → 新的默认值一定生效。
	//
	// 玩家反馈"BOSS 不能一味挨打"：0.35 / 冷却 2.5s / 保底 4 次，实测命中率只有 ~33%，
	// 加上"常驻举盾"只在 Idle 挂着（非 Idle 全部交给概率判定），体感就是一直在挨打。
	// 现在 0.5 / 1.8s / 保底 3 次 → 实测应落在 50~60%：
	//   仍有近一半的命中能打进去（博弈感在），但 BOSS 不再像个不会防御的木桩。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Guard")
	float GuardRollChance = 0.55f;

	// 两次格挡之间的最短间隔（秒）。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Guard")
	float GuardRollCooldown = 1.1f;

	// 保底：连续这么多次"有资格格挡"却没随机成功 → 下一次强制格挡。
	// 没有它，低概率在短遭遇战里可能一次都不出现 = 玩家以为"BOSS 根本不会格挡"。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Guard")
	int32 GuardPityHits = 3;

	// 举盾姿态保持多久（秒）后交还给行走/待机。
	// 举盾蒙太奇本身 3.3s，播完再收会变成"BOSS 举着盾发呆三秒"，所以到点主动收。
	// 0.8s = 够玩家看清"它举盾了"（配冷蓝护体光 + 火花 + 撞击音），又不至于打断战斗节奏。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Guard")
	float GuardHoldTime = 0.8f;

	double LastBlockTime = -999.0;
	int32 GuardEligibleHits = 0;      // 自上次格挡以来"有资格格挡"的命中数（保底计数）
	bool bGuardedThisHit = false;     // 本次命中已经由 TryGuardIncomingHit 判定为格挡
	FTimerHandle GuardRestoreHandle;  // 举盾保持计时器

	// ★ 举盾窗口（2026-10-03）：概率格挡成功后的 GuardHoldTime 秒内 = 真的举着盾。
	//
	// 为什么要它：实测 10 次模拟命中里 7 次被挡，而【格挡判定日志只出现 3 次且全部失败】——
	// 说明那 7 次根本没走概率判定，是 UpdateGuardStance 常驻挂着的 State_Blocking 全挡下来了。
	// 后果有两个：① 太频繁（玩家打不进伤害，变成"太肉"而不是"会格挡"）；
	//            ② 常驻挡下的命中不会调 OnBossBlockPlayerAttack → 举盾蒙太奇一次都不播
	//               → 玩家只能从"伤害变小"去猜，于是得出"BOSS 根本不会格挡"的结论。
	// 现在：State_Blocking 只在【举盾窗口】里挂，窗口外一律走概率判定。
	bool bGuardWindowActive = false;

	// 格挡时播放的蒙太奇（留空则用 GuardMontage / 约定路径）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Guard")
	TObjectPtr<UAnimMontage> BlockMontage;

	// 格挡时护体光（冷蓝）亮多久（秒）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Guard")
	float GuardFlashDuration = 0.35f;
	FTimerHandle GuardFlashOffHandle;

	// ★ 受击【结算之前】的格挡判定（由 UDemoAbilitySystemLibrary::ApplyHit 调用）。
	// 返回 true = 这一下被挡住了 → 伤害 ×0.25、扣攻方架势、播举盾蒙太奇。
	// 返回 false = 正常挨打。判定细节见 .cpp。
	virtual bool TryGuardIncomingHit(AActor* Attacker, float IncomingDamage, bool bCanBeBlocked) override;

	// 格挡表现：播举盾蒙太奇 + 护体光亮起（判定与冷却在 TryGuardIncomingHit 里）
	void OnBossBlockPlayerAttack();
	void TurnOffGuardFlash();

	// 举盾保持时间到 → 把当前底层动画交还给行走/待机（下次 Tick 的 loco 会自动重挂）
	void RestoreLocoAfterGuard();

	// ★ 调试钩子：模拟"玩家砍中 BOSS"，用来验证格挡判定真的会触发（2026-10-03）
	//
	// 必要性：无头实跑里玩家是没人操作的木桩，一整局 BOSS 一下都挨不到 →
	// 格挡判定永远走不到，"BOSS 会不会格挡"只能靠读代码猜（已经猜错好几轮了）。
	// 有了它就能在【无头日志】里直接数出"★格挡★"出现了几次，用事实说话。
	//
	// 用法（编辑器 / 无头都行，控制台输入）：
	//   demo.BossGuardTest 6   → 之后每 1.2 秒模拟一次玩家命中，共 6 次，打完自动归零
	//   demo.BossGuardTest 0   → 立刻停
	void TickGuardSelfTest(float DeltaTime);
	float GuardTestTimer = 0.f;
	int32 GuardTestArmed = 0;      // 上次装填时读到的 CVar 值（用来识别"控制台又设了一次"）
	int32 GuardTestHitsLeft = 0;   // 还剩几次没打

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Death")
	TObjectPtr<UAnimMontage> DeathMontage;

	// 死亡序列（单节点直接播、不循环 → 停在死亡姿势）。留空时按约定路径
	// /Game/ParagonKhaimera/.../Death_A 兜底；都拿不到就退回播 DeathMontage 蒙太奇。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Death")
	TObjectPtr<UAnimSequence> DeathSequence;

	// 死亡后多久销毁尸体（秒；0 = 不销毁，留着看倒地姿势）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Death")
	float DeathDestroyDelay = 8.f;

	// 死亡时是否清空场上所有己方弹幕（避免死后还被自己的弹幕打）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Death")
	bool bClearProjectilesOnDeath = true;

	// ---------------- 战斗手感 ----------------
	// 出招期间霸体：BOSS 的动作不会被玩家普攻点掉，只能靠打空韧性破防来打断
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Combat")
	bool bSuperArmorWhileAttacking = true;

	// 近战武器扫掠半径（比玩家大一圈，体现体型差）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Boss|Combat")
	float BossWeaponTraceRadius = 45.f;

	// ---------------- 复仇机制 ----------------
	// 被玩家打满 MaxHitsBeforeRevenge 下（自上次出招起算），强制还手一次。
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Boss|Combat")
	bool bEnableRevengeCounter = true;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Boss|Combat")
	int32 MaxHitsBeforeRevenge = 5;

	// ---------------- 属性兜底 ----------------
	// ⚠ 原先这里有一套 DefaultMaxHealth 等属性，现已删除：
	//   它们的守卫写成 `if (GetMaxHealth() > 0) return;`，而 UDemoAttributeSet 的构造函数
	//   已经 InitMaxHealth(100)，守卫永远命中 → 兜底从未执行过，
	//   于是 BP_Boss 的 AttributeSet 一直是构造默认值（血量 100），
	//   "BOSS 两下就被打死、玩家也两下被打死"就是这么来的。
	//
	// 现在统一走基类 ADemoCharacterBase::EnsureDefaultAttributes()（读 Demo|Balance 的 Balance* 属性），
	// BOSS 的数值在构造函数里把 Balance* 覆盖成 BOSS 档即可 —— 一套代码，双方各取所需。
	// 数值来源与推导见 Core/DemoCombatTuning.h。

private:
	// 当前招式数据（Brain 在激活技能前写入，技能激活时来取）
	UPROPERTY()
	TObjectPtr<UBossAttackData> PendingAttackData = nullptr;

	// 蒙太奇槽位为空时按约定路径加载（Tools/SetupBossMontages.py 生成的位置）
	UAnimMontage* ResolveMontage(TObjectPtr<UAnimMontage>& Slot, const TCHAR* ConventionalPath);

	// 破防 / 死亡的状态边沿（用于播一次性表现）
	bool bWasBroken = false;
	bool bWasDead = false;

	// 复仇计数：上一次出招之后又被玩家揍了几下（到阈值由 Brain 强制还手）
	int32 HitsSinceLastAttack = 0;

	// 根位移模式是否已经掰正过（避免每帧重复设置）
	bool bRootMotionVerified = false;

	// ---- 被动反应蒙太奇的根位移闸 ----
	// true = 当前正为了某个受击/虚弱蒙太奇把根位移关着（播完必须还原）
	bool bReactionRootMotionSuppressed = false;

	// 当前底层单节点循环在播 Jog（true）还是待机（false），避免每帧重启动画
	bool bBossLocoPlayingJog = false;

	// 由 Brain 写入的"最近一次下令移动"时间戳（世界时间，秒）。
	// 配合 NotifyBossMoveCommand 使用：只要距上次下令 < 移动意图窗口，就强制播 Jog，
	// 不让 AI 直驱的速度抖动把腿冻成 Idle（否则画面上就是"身体僵着、整体滑过去"）。
	// 窗口常量定义在 BossCharacterBase.cpp（UE 5.7 的 UHT 不识别 UCLASS 里的 constexpr，放 .cpp 避坑）。
	float LastMoveCommandTime = -999.f;

	// 由 Brain 写入的"最近一次下令移动的速度"（cm/s）。
	// ⚠ 这是播放倍速的【唯一可靠来源】：BOSS 在自己的 Tick 里读 GetVelocity() 永远得到 0
	//   （CharacterMovement 已消费掉 RequestDirectMove 的本帧速度），若拿 0 去算倍速，
	//   倍速会长期钉在下限 → 腿慢动作爬、胶囊照常平移 → "没有动画、只是模型平移"。
	float LastMoveCommandSpeed = 0.f;

	// ---- 位移安全阀运行时状态 ----
	// 上一帧"合法"的位置（安全阀基准）；false = 还没采到基准（刚出生 / 刚开关）
	FVector GuardAnchorLocation = FVector::ZeroVector;
	bool bGuardAnchorValid = false;
	// 日志节流：撞闸可能每帧发生，别把日志刷爆
	float GuardLogCooldown = 0.f;

	// ---- 起手预警红光运行时状态 ----
	UPROPERTY()
	TObjectPtr<UPointLightComponent> TelegraphFlashLight;

	// ⚠⚠ 每帧按【当前姿势的真实骨骼范围】校正红光位置，让红光严格罩住模型（第二十一轮新增）。
	// 之前红光是挂在 Mesh 组件原点、固定不动的，而 Mesh 原点在【脚底】——
	 // 于是红光只照亮 BOSS 的脚，画面上就是"红光和模型错位、糊在脚下"。
	// 这个函数原本只存在于注释里、根本没有实现。现在真正实现它。
	void UpdateFlashLightPositions(float DeltaTime);

	// 灯位/半径诊断日志的节流（每 1s 一条，无头跑直接读它核对"光到底贴不贴身"）
	float LightDiagTimer = 0.f;

	// ---- 死亡后冻结（2026-10-04）----
	//
	// 用户反馈"BOSS 倒地后还在移动"。OnDeathReaction 里虽然已经
	// StopMovementImmediately + DisableMovement + 关碰撞，但每帧的
	// ① 位移安全阀 UpdateDisplacementGuard —— 它只把单帧位移夹在
	//    "走路速度 × 余量"（约 600cm/s），死亡后照样放行，
	//    一旦有任何外力（弹反击退、残留 RequestDirectMove、被夹回时的 SetActorLocation），
	//    尸体就会一路滑出去；
	// ② 平滑转向 UpdateSmoothFacing —— 死了还能转身；
	// ③ 任何迟到的位移请求。
	// 所以死亡不再只"关闸"，而是每帧把 Actor 硬钉回死亡瞬间的位姿。
	void UpdateDeathFreeze(float DeltaTime);

	// 死亡锚点：OnDeathReaction 里记下倒下那一刻的位置/朝向，之后每帧强制还原
	FVector DeathAnchorLocation = FVector::ZeroVector;
	float DeathAnchorYaw = 0.f;
	bool bDeathFrozen = false;

	// ---- 破防跪地锁死（2026-10-04，2026-10-06 加"整个停机"）----
	//
	// 用户反馈"BOSS 破防跪地时还能动"。和死亡是同一类问题，但不能用死亡那套：
	// 破防是会【恢复】的（RecoverFromPoiseBreak 到点自动起身），所以不能一去不回地 DisableMovement。
	//
	// ★ 2026-10-06 补上最后一层：破防期间把 MovementMode 直接设成 **MOVE_None**。
	//   之前只做"每帧清速度 + 钉回锚点"，看起来是钉住了，但移动组件【内部仍然认为
	//   自己在下落】：下一帧 FindFloor 判定悬空 → 又把胶囊体往下拽 → 视觉上原地抖动，
	//   玩家看起来就是"跪着还在动"。
	//   进出都只在这一处处理：进入时把当时的 MovementMode 存进 SavedBreakMoveMode，
	//   起身那一帧原样还回去（不写死 Walking —— 万一破防发生在空中/被击飞时）。
	void UpdateBreakFreeze(float DeltaTime);

	// 是否处于破防（跪地）状态
	bool IsPoiseBroken() const;

	FVector BreakAnchorLocation = FVector::ZeroVector;
	float BreakAnchorYaw = 0.f;
	bool bBreakAnchorValid = false;

	// 破防前的 MovementMode（MOVE_None 表示"没进过破防锁"，别用来当恢复值）
	EMovementMode SavedBreakMoveMode = EMovementMode::MOVE_None;
	bool bBreakSavedMoveMode = false;

	// 破防期位移取证（无头跑直接读：被推多少 / 拉回多少）
	float BreakDriftTotal = 0.f;
	float BreakRevertTotal = 0.f;
	float BreakFreezeLogTimer = 0.f;

	// 死亡后的位移取证（无头跑直接读这组数：累计/最大偏移都该是 0）
	float DeathDriftAccum = 0.f;      // 累计偏离锚点多少 cm
	float DeathDriftMax = 0.f;        // 单帧最大偏离
	float DeathRevertTotal = 0.f;     // 被硬拉回去的总距离
	float DeathFreezeLogTimer = 0.f;  // 每 1s 打一条统计
	float DeathStressTimer = 0.f;     // 死亡后强推压力测试的节流（仅 demo.BossKill 时开）

	// 调试钩子 demo.BossKill：只触发一次，避免每帧重复结算
	bool bKillTestFired = false;

	// 调试钩子 demo.BossBreak：立刻打空韧性进破防，并在跪地期间持续外力推挤，
	// 用来证明"倒地期间净位移恒为 0"（外力确实作用在它身上，但它一步都没动）
	bool bBreakTestFired = false;
	float BreakTestPushTimer = 0.f;
	int32 BreakTestPushCount = 0;
	float BreakTestInjectedCm = 0.f;   // 累计注入的直接位移（应与"已拉回"总量相等）

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> TelegraphFlashMID;

	// 失衡轮廓用的动态材质实例（由 UnstableOverlayMaterial 创建）
	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> UnstableMID;

	// 覆盖材质是否已经挂到 Mesh 上（避免每帧重复 SetOverlayMaterial）
	bool bUnstableOverlayApplied = false;

	// 轮廓呼吸相位（秒）
	float UnstableVisualTime = 0.f;

	// 闪动相位（秒）。持续累加，用 sin 换出"均匀呼吸"
	float TelegraphFlashTime = 0.f;

	// 上一次是否在起手（用于捕捉"刚进入起手"这个边沿 → 炸一下红光）
	bool bWasTelegraphing = false;

	// ---- 受击闪白运行时状态 ----
	UPROPERTY()
	TObjectPtr<UPointLightComponent> HitFlashLight;

	// 受击闪白剩余时间（秒）。>0 时点亮白光，逐帧衰减到 0 后熄灭
	float HitFlashRemaining = 0.f;

	// 覆盖材质是否已经挂在 Mesh 上（避免每帧重复设置）
	bool bTelegraphOverlayApplied = false;

	// ⚠⚠ 三条"状态光"共用 Mesh 的【同一个 OverlayMaterial 槽】，必须仲裁（2026-10-04）
	//
	// USkeletalMeshComponent 只有一个 overlay 槽。起手预警红光、失衡绿轮廓、格挡冷蓝轮廓
	// 三个通道此前各自直接 SetOverlayMaterial()，于是【谁最后跑谁赢】：
	// BOSS 一边起手（红）一边处于失衡（绿）时，红光会把绿轮廓整个顶掉 →
	// 玩家看到"绿光时有时无 / 不在身上"，读起来就是"绿光不贴合模型"。
	// 现在三条通道只各自置标记，由 RefreshBossOverlay() 按优先级统一挂载：
	//   起手预警红 > 失衡绿 > 格挡冷蓝（红色是"马上要挨打"的最高优先级信息）
	void RefreshBossOverlay();

	// 当前真正挂在 Mesh 上的那个 overlay（用于避免每帧重复 SetOverlayMaterial）
	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> AppliedOverlayMID;

	// ---- 平滑转向状态 ----
	void UpdateSmoothFacing(float DeltaTime);

	// 让 BOSS 在移动时忽略玩家：根治"玩家把 BOSS 推着走 / 打飞"。
	// UE 角色移动组件即便胶囊是 Overlap 响应，每帧仍做"去穿插"把重叠双方互相顶开，
	// 玩家贴上来就会把 BOSS 一帧推几厘米、累积成大幅度位移。让 BOSS 忽略玩家这个 Actor 后，
	// CMC 的位移修正不再顶 BOSS，玩家就推不动它（玩家自己仍被 BOSS 挡住，不会穿模）。
	// ⚠ 不影响命中检测：玩家砍 BOSS 走的是 OverlapMultiByChannel(ECC_Pawn) 独立查询，
	//    与移动时的 IgnoreActorWhenMoving 无关。
	void UpdatePlayerPushImmunity();

	bool bHasFacingTarget = false;
	FVector FacingTargetLocation = FVector::ZeroVector;
	float FacingTurnSpeed = 720.f;
	float FacingTimeRemaining = 0.f;
};
