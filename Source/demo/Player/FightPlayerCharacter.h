// 持剑玩家角色：相机、8 个 Enhanced Input 绑定、把按键转成 GAS 技能激活
#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"
#include "Animation/AnimEnums.h"
#include "Core/DemoCharacterBase.h"
#include "Abilities/GameplayAbility.h"
#include "InputActionValue.h"
#include "FightPlayerCharacter.generated.h"

class USpringArmComponent;
class UCameraComponent;
class USoundBase;
class ULockOnComponent;
class UInputMappingContext;
class UInputAction;
class UInputComponent;
class UAnimMontage;
class UAnimInstance;
struct FAnimMontageInstance;
struct FInputActionValue;
class UCombatHUDWidget;

UCLASS()
class DEMO_API AFightPlayerCharacter : public ADemoCharacterBase
{
	GENERATED_BODY()

public:
	AFightPlayerCharacter();

	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual void PossessedBy(AController* NewController) override;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|Player")
	ULockOnComponent* GetLockOnComponent() const { return LockOn; }

	// ---------------- 道具：回血"酒" ----------------
	// 当前持有数量 / 携带上限（开局由 DemoBalance::HealItemInitialCount / HealItemMaxCount 初始化）。
	// HUD 每帧读取 GetHealItemCount / GetHealItemMaxCount 显示"酒 ×N/M"。
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|HealItem")
	int32 GetHealItemCount() const { return HealItemCount; }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|HealItem")
	int32 GetHealItemMaxCount() const { return HealItemMaxCount; }

	// 消耗 1 瓶：数量>0 才扣并返回 true。供 UGA_PlayerUseHeal 调用。
	// 满血分支在技能里短路、不会调用本函数，所以"满血"不会白扣一瓶。
	UFUNCTION(BlueprintCallable, Category = "Demo|HealItem")
	bool TryConsumeHealItem();

	// 反向引用到战斗 HUD（由 AFightGameMode 在创建并绑定 HUD 后调用 SetCombatHUD）。
	// UGA_PlayerUseHeal 的绿色回血反馈需要它。
	UFUNCTION(BlueprintCallable, Category = "Demo|HealItem")
	void SetCombatHUD(UCombatHUDWidget* InHUD) { CombatHUD = InHUD; }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|HealItem")
	UCombatHUDWidget* GetCombatHUD() const { return CombatHUD; }

	// 按 UseItemKey（默认 R）触发的"喝一口酒"入口；Tick 里做原始键轮询后调用。
	void HandleUseItem();

	// ---------- 移动输入锁（普攻/弹反/闪避/喝酒 期间由各技能控制） ----------
	// 锁上时 HandleMove 直接 return：WASD 完全不进角色（比只改 MaxWalkSpeed 更彻底，
	// 因为 MaxWalkSpeed 只限制速度上限，残余速度仍会让角色滑动）。
	//
	// ⚠⚠⚠ 2026-10-03：这是【引用计数】，不是布尔开关。
	// 原来只有一个 bool bMoveInputLocked，而"锁移动"这件事同时可能由多个技能持有：
	//   普攻（NormalAttack）、弹反（PlayerParry）、闪避（PlayerDodge）、喝酒（UseItem），
	//   任何两个都可能同时存在（弹反会 CancelAbilitiesWithTag 取消普攻）。
	// 用 bool 时，"谁最后解锁谁说了算" → 经典竞态：
	//   普攻 Activate(锁) → 弹反取消普攻 → 普攻 EndAbility(解锁) → 弹反 Activate(锁)
	 //   若这两步顺序反了，普攻的解锁会把【弹反刚加的锁】一起放掉；
	 //   反过来则弹反的解锁把普攻的锁提前放掉。
	//   两种都表现为"弹反后角色僵在那里动不了"（或"普攻后还能跑"）。
	// 引用计数让每个技能只减自己加的那一份，谁先结束都不影响别人。
	void LockMoveInput();
	void UnlockMoveInput();
	// 兼容旧调用点（内部转成 Lock/Unlock）；新代码请用上面两个。
	void SetMoveInputLocked(bool bLocked) { bLocked ? LockMoveInput() : UnlockMoveInput(); }
	bool IsMoveInputLocked() const { return MoveInputLockCount > 0; }
	// 当前有几层锁（诊断用：>1 说明技能重叠，=0 且仍动不了就是别的原因）
	int32 GetMoveInputLockCount() const { return MoveInputLockCount; }

	// 闪避期间屏蔽“动画自带的根位移”。闪避动画（Dodge_*）本身就是带位移的 Root-Motion 动画，
	// 若让它自己和代码给的位移同时生效，距离会翻倍、还会出现拉回/方向打架。
	// 打开后动画原地播放，位移完全由 UGA_PlayerDodge 用 DodgeDistance 精确控制；结束时自动恢复原设置。
	void SetIgnoreAnimRootMotion(bool bIgnore);

	// ---------- 蒙太奇根位移的“硬屏蔽”（闪避专用） ----------
	// SetIgnoreAnimRootMotion 改的是 AnimInstance 的 Root Motion Mode（全局开关）；
	// 引擎里还有一道更靠上游的开关：FAnimMontageInstance::PushDisableRootMotion()。
	// 只要它开着，这段蒙太奇的根位移会被【直接跳过】（AnimInstance.cpp:1958 的
	// bExtractRootMotion 判定），连 ABP 自己的 Root Motion Mode 都绕不过去。
	// 闪避期间用它把 Dodge_* 的位移彻底压掉，位移 100% 由技能里的 DodgeDistance 决定。
	bool SuppressMontageRootMotion(UAnimMontage* Montage);
	void RestoreMontageRootMotion();

	// ---------- 位移辅助（普攻 / 闪避共用，由技能在起手/收招时调用） ----------
	// 做两件事：
	//   1) 记下位移起点 → 结束时算出【本次真实位移】并打日志，
	//      把“位移”从手感变成可核对数字（能一眼分辨“真的动了”还是“只有 Mesh 在滑”）；
	//   2) 位移期间让镜头与角色刚性同步 → 视角跟着角色一起走，角色始终钉在画面同一位置。
	//
	// 可重入（内部计数）：普攻与闪避的辅助重叠时不会提前还原。
	void BeginDisplacementAssist();

	// 结束位移辅助：还原镜头参数；返回【本次实际位移（厘米，只取水平分量）】
	float EndDisplacementAssist();

	// 是否正处于位移辅助中
	bool IsDisplacementAssisting() const { return DisplacementAssistDepth > 0; }

	// 【镜头】位移期间是否让相机与角色“刚性同步”（默认开 = 视角跟着角色）。
	//
	// true  = 位移期间关掉 CameraBoom 的 CameraLag → 相机 1:1 跟着角色走，
	//         角色在画面里始终是同一个位置（不会出现“人窜出去、镜头慢慢追”的错觉）。
	// false = 用下面的 DisplacementCameraLagSpeed 做软跟随（位移看得更清楚，
	//         但角色会短暂偏离画面中心，视觉上像被镜头“拉回”）。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Displacement")
	bool bRigidCameraDuringDisplacement = true;

	// 【仅当 bRigidCameraDuringDisplacement = false 时生效】位移期间的相机跟随速度。
	// 越小 = 追得越慢；越大 = 越贴身。<= 0 = 完全不动相机参数。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Displacement")
	float DisplacementCameraLagSpeed = 12.f;

	// ---------- 根位移模式（“拉回”的根治开关） ----------
	//
	// 引擎源码里，蒙太奇姿势的根骨骼重置条件是（AnimInstanceProxy.cpp:1604）：
	//     ExtractionContext.bExtractRootMotion
	//         = Montage->HasRootMotion() && RootMotionMode != NoRootMotionExtraction
	// 也就是说，只要动画实例处于 NoRootMotionExtraction：
	//   ① 根骨骼【不会】被重置 → 动画自带的位移留在 Mesh 上（看着在动），
	//      蒙太奇播完 / 被下一段切走时 Mesh 弹回胶囊体原点 = 你说的“拉回”；
	//   ② 蒙太奇根位移也进不了 CharacterMovement → 胶囊体压根没动
	//      = “是动画而不是真正的移动”。
	// 所以本项目统一要求 RootMotionFromMontagesOnly（移动动画等效原地播，
	// 只有蒙太奇贡献位移）。出招前调一次即可，幂等。
	void EnsureRootMotionExtraction(const TCHAR* Reason);

	// 当前动画实例的根位移模式编号（日志用；-1 = 拿不到动画实例）
	int32 GetAnimRootMotionMode() const;

	// “非攻击状态”下应有的移速上限（长按 Shift 奔跑 = RunSpeed，否则 WalkSpeed）。
	// 攻击结束恢复移动时用它设置，避免沿用一个在攻击期间按/松过 Shift 的过期值。
	float GetDesiredMaxWalkSpeed() const { return bShiftRunActive ? RunSpeed : WalkSpeed; }

	// ---------------- 镜头冲击 ----------------
	//
	// 短暂把弹簧臂拉近 Length 厘米，再用 Duration 秒弹回原位。
	// 弹反 / 完美闪避时调用 —— 配合顿帧和整屏闪光，构成"这一下打成了"的完整反馈。
	//
	// 为什么是"拉近"而不是"推远"：拉近 = 视野收窄 = 视觉上的"挤压感"，
	// 和顿帧的"时间被压住"是同一个心理模型，两者叠加最顺；
	// 推远则更像"被击退"，那是挨打该有的表现。
	//
	// 多次调用会取【较大】的 Length 而不是叠加：连续弹反时镜头不该越拉越近。
	UFUNCTION(BlueprintCallable, Category = "Demo|Camera")
	void ApplyCameraPunch(float Length = 26.f, float Duration = 0.32f);

	// 立刻结束镜头冲击并还原（死亡 / 切场景时用）
	UFUNCTION(BlueprintCallable, Category = "Demo|Camera")
	void ClearCameraPunch();

	// 命中 BOSS 时的一记轻微"旋转微震"：和 ApplyCameraPunch 的拉近叠加，
	// 给"砸实了"的镜头回馈，解决"砍在 BOSS 身上没手感"。
	// 强度随伤害缩放（见 DemoCombatTuning.h 的 HitImpactShake*），连段叠起来是节奏感不是眩晕。
	UFUNCTION(BlueprintCallable, Category = "Demo|Camera")
	void PlayHitImpactShake(float Damage, float Duration = 0.08f);

	// 命中 Screen Shake：真·镜头震动（PlayerCameraManager 级的位移+旋转震荡），
	// 波形写在 UDemoHitCameraShake 构造里（免资产），这里只管按强度触发。
	// Intensity：0.3（被格挡的闷震）~1.0+（破防/暴击满额），具体取值见 DemoCombatTuning.h 打击感参数包。
	UFUNCTION(BlueprintCallable, Category = "Demo|Camera")
	void PlayHitScreenShake(float Intensity = 1.f);

	// 受击 / 命中的 FOV 脉冲：短暂把 FOV 扩开再弹回，制造"怼到脸上"的镜头冲击。
	//
	// 为什么它比"拉近臂长"更适合锁定时用：FOV 只改视野宽度，
	// 既不碰弹簧臂（不会和锁定臂长打架），也不碰控制朝向（不会让视角漂走），
	// 所以是【锁定安全】的那一档冲击 —— 锁定时照样能给"砍实了"的反馈。
	//
	// bAllowWhileLocked：默认 false（受击时保持锁定视角绝对稳定，历史上用户明确要求过）；
	//   玩家命中敌人时由 HUD 传 true 打开。
	UFUNCTION(BlueprintCallable, Category = "Demo|Camera")
	void PulseCameraFOV(float DeltaDeg, float Duration, bool bAllowWhileLocked = false);

	// ---------------- 命中音效 ----------------
	// 打击感里"声音"是半壁江山：画面再抖，没有那一下"铛"，玩家依然觉得砍在棉花上。
	// 两个槽位分轻/重（普通命中 / 破防暴击），蓝图里可换；留空则按约定路径兜底加载，
	// 再拿不到就静音（不 warn 刷屏 —— 缺资源不该淹没真正的问题日志）。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Audio")
	TObjectPtr<USoundBase> HitSound;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Audio")
	TObjectPtr<USoundBase> HeavyHitSound;

	// 播一次命中音。bHeavy = 破防/暴击那一下（用重音）。
	UFUNCTION(BlueprintCallable, Category = "Demo|Audio")
	void PlayHitSound(bool bHeavy, const FVector& Location);

	// 弹刀成功 → 一记很轻的镜头冲击（只狼弹刀"叮"的那一下的分量）。
	//
	// 覆写 ADemoCharacterBase::PlayParryFeedback。火花 / 爆点已经在 ApplyHit 里生成了，
	// 这里只负责镜头：长度刻意压得很小 —— 连弹时镜头不该一直抖，
	// 否则玩家读不出"下一招什么时候到"。
	virtual void PlayParryFeedback(const FVector& ContactLocation, bool bPerfect) override;

	// ---------------- 锁定镜头 ----------------
	//
	// 分两层：
	//   · ULockOnComponent 决定「锁谁」和「身体朝哪」；
	//   · 这里决定「镜头怎么摆」—— 控制朝向、臂长、肩部偏移、俯仰限位。
	//
	// 锁定期间鼠标不再直接转控制朝向（那会和"对准目标"打架，表现为镜头被来回拽），
	// 而是变成一点点"手动偏移"，松手后自动回中 —— 既能微调看清战场，又不会脱锁。
	UFUNCTION(BlueprintCallable, Category = "Demo|LockOn")
	void ToggleLockOn();

	UFUNCTION(BlueprintCallable, Category = "Demo|LockOn")
	void SetLockOnEnabled(bool bEnabled);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|LockOn")
	bool IsLockedOn() const;

	// 鼠标横/纵拖拽量（由 HandleLook 在锁定时转进来）
	void AddLockOnViewOffset(float DeltaYaw, float DeltaPitch);

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;
	virtual void OnDamagedReaction(EDemoHitDirection Direction, float Damage) override;
	virtual void OnDeathReaction() override;

	// ---------- 道具：回血"酒" ----------
	// 携带上限（开局由 DemoBalance::HealItemMaxCount 覆盖）；可在蓝图 Defaults 调整。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|HealItem")
	int32 HealItemMaxCount = 3;

	// ---------- 组件 ----------
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Demo|Camera")
	TObjectPtr<USpringArmComponent> CameraBoom;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Demo|Camera")
	TObjectPtr<UCameraComponent> FollowCamera;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Demo|LockOn")
	TObjectPtr<ULockOnComponent> LockOn;

	// ---------- 输入资产（在蓝图里指定） ----------
	// 注意：构造函数里已按 /Game/FightPlayer/Input/ 的约定路径做了兜底加载，
	// 所以即使蓝图里忘了填这些槽位也能正常操作；蓝图里填的值优先。
	//
	// 当前键位（IMC_FightPlayer 的映射由 Tools/SetupInputRemap.py 维护）：
	//   WASD       → IA_Move          移动
	//   鼠标移动    → IA_Look          视角
	//   空格        → IA_Jump          跳跃
	//   左Shift     → IA_Run           短按 = 闪避 / 长按 = 奔跑
	//   鼠标左键    → IA_NormalAttack  普攻（4 段连击）
	//   鼠标右键    → IA_Defense       弹刀（Parry）
	//
	// ⚠ UE5.7 的 IMC 映射数据实际存在 DefaultKeyMappings（旧的 Mappings 属性已废弃、
	//   运行时不再读取）。如果编辑器脚本没跑成功，左Shift 可能还停留在早期的 IA_Flash 上，
	//   所以 C++ 里把 IA_Run / IA_Flash / IA_Dodge 三个 Input Action 都接了同一套
	//   “短按闪避 / 长按奔跑”逻辑（同一按下的事件由内部去重），保证按 Shift 一定有反应。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Input")
	TObjectPtr<UInputMappingContext> DefaultMappingContext;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Input")
	TObjectPtr<UInputAction> MoveAction;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Input")
	TObjectPtr<UInputAction> LookAction;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Input")
	TObjectPtr<UInputAction> JumpAction;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Input")
	TObjectPtr<UInputAction> RunAction;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Input")
	TObjectPtr<UInputAction> NormalAttackAction;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Input")
	TObjectPtr<UInputAction> AttackSkillAction;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Input")
	TObjectPtr<UInputAction> DefenseAction;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Input")
	TObjectPtr<UInputAction> DodgeAction;

	// 早期版本的 IMC 里 LeftShift 绑的是 IA_Flash；这个槽位保留下来做兼容，
	// 让“按 Shift 闪避”在 IMC 没改到位的旧资产上也能生效。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Input")
	TObjectPtr<UInputAction> FlashAction;

	// ---------- 技能（可选） ----------
	// 留空时会自动从 DefaultAbilities 里按 AbilityTag 查找对应技能，
	// 所以正常只需在 DefaultAbilities 里配一次即可
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Ability")
	TSubclassOf<UGameplayAbility> NormalAttackAbility;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Ability")
	TSubclassOf<UGameplayAbility> AttackSkillAbility;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Ability")
	TSubclassOf<UGameplayAbility> DefenseAbility;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Ability")
	TSubclassOf<UGameplayAbility> ParryAbility;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Ability")
	TSubclassOf<UGameplayAbility> DodgeAbility;

	// ---------- 移动 ----------
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Movement")
	float WalkSpeed = 480.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Movement")
	float RunSpeed = 700.f;

	// ---------------- 战斗间距（不许贴进敌人身体里）----------------
	//
	// 为什么必须自己实现：Pawn 通道对 Pawn 是【Overlap】不是 Block（近战命中要靠
	// OverlapMultiByChannel(ECC_Pawn) 才查得到人），所以胶囊体之间根本不会互相挡 ——
	// 玩家可以一路走进 BOSS 体内，两个模型糊在一起。
	// 而普攻又有几百厘米的根位移突进，冲进去之后就"贴脸输出"，既看不清动作也丢了对峙感。
	//
	// 这里做的是动作游戏通行的【软碰撞】：
	//   ① 距离小于 CombatSeparationRadius 时，先削掉"继续往对方身上挤"的速度分量 ——
	//      玩家不再被推着走，也不会和推挤力互相打架产生抖动；
	//   ② 已经重叠时以 CombatSeparationPushSpeed 把玩家推回到边界上（带 sweep，不会穿墙）。
	// 只推玩家、不推敌人（BOSS 有"不许被打飞/推走"的硬要求）。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Movement")
	bool bCombatSeparationEnabled = true;

	// 与敌对角色的最小【水平中心距】（cm）。
	// ⚠ 调大 = 更不容易贴脸，但过大就砍不到 —— 近战判定在刀锋上（身前约 0~130cm），
	//   BOSS 胶囊半径 60、玩家 34，两者"皮贴皮"是 94cm；
	//   110~140 是"刀够得着、身体进不去"的区间。默认 128（约留 34cm 身位）。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Movement")
	float CombatSeparationRadius = 128.f;

	// 被顶开时的最大回推速度（cm/s）。
	// ⚠ 必须明显大于普攻突进的速度，否则玩家会"慢慢挤进去"（推挤追不上突进）。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Movement")
	float CombatSeparationPushSpeed = 900.f;

	// 左Shift 按住超过这个秒数才算"长按奔跑"，在阈值前松开算"短按闪避"
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Movement")
	float RunHoldThreshold = 0.25f;

	// 闪避/奔跑用的物理按键 —— 直接轮询原始按键状态，完全不经过 IMC。
	// 这是“按 Shift 一定有反应”的最终兜底：即使 IMC_FightPlayer 里这个键映射到了
	// 别的 Input Action（或压根没映射），也照样能短按闪避、长按奔跑。
	// 想换键直接在这里改（编辑器里可改，不用重编）。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Input")
	FKey DodgeRunKey = EKeys::LeftShift;

	// ---------- 锁定相关按键（同样是原始按键轮询，不依赖 IMC 资产） ----------
	// 为什么锁定键也不走 Enhanced Input：IMC 要新增一条映射就得动资产，
	// 而这些键不需要"按下/松开/长按"这些触发器语义，直接读物理键最省事也最不会出问题。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|LockOn")
	FKey LockOnKey = EKeys::MiddleMouseButton;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|LockOn")
	FKey SwitchTargetLeftKey = EKeys::Q;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|LockOn")
	FKey SwitchTargetRightKey = EKeys::E;

	// ---------- 道具（回血"酒"）按键 ----------
	// 原始键轮询，不依赖 IMC 资产（和 DodgeRunKey / LockOnKey 同一套兜底思路）。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|HealItem")
	FKey UseItemKey = EKeys::R;

	// ---------- 锁定镜头手感 ----------
	// 锁定时镜头臂长。比默认（380）远一些：要同时装下玩家和 BOSS。
	//
	// ⚠ 保持【固定值】，不要让它随距离自适应。
	//   引擎的弹簧臂把相机放在枢轴【后方】（SpringArmComponent.cpp:185 是 `-= 朝向 * 臂长`），
	//   所以**臂长越大，相机离 BOSS 越远**，固定臂长本来就安全。
	//   曾经因为把方向搞反，加过"贴身收缩臂长"的逻辑 —— 那实际是在把相机往自己身体里塞，
	//   近裁剪面会切掉自身模型（用户反馈的"我的角色慢慢消失"）。已撤销。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|LockOn|Camera")
	float LockOnArmLength = 480.f;

	// 锁定时把镜头挪到肩膀外侧：玩家偏在画面一侧，BOSS 居中，
	// 近战贴脸时才不会被自己的模型挡住视野。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|LockOn|Camera")
	FVector LockOnSocketOffset = FVector(0.f, 85.f, 95.f);

	// 肩位偏移的【恒定夹角】（度）：偏移长度 = 【相机→焦点】距离 × tan(本角度)，
	// 上限仍是上面 LockOnSocketOffset 的长度。这样无论玩家离 BOSS 多远，
	// "把 BOSS 拉回画面中心所需的补偿角"都恒等于这个值（默认 7°），观感一致、不抖。
	//
	// ⚠ 分母必须是【相机→焦点】= 枢轴到焦点的距离 D【+】臂长（相机在枢轴后方，见上）。
	//   历史上这里先写过 D、后写过 D − 臂长，两个都错；而且因为方向符号反了，
	//   曾经推导出的"贴身会爆炸到 58°"是错误结论 —— 不要再照着那段推导改代码。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|LockOn|Camera")
	float LockOnShoulderAngleDeg = 7.f;

	// ---------- 绕背预测偏移 ----------
	// 玩家【横向绕着 BOSS 跑】时，把解算用的焦点沿速度方向外推一小段，
	// 让相机"预判"玩家要去的位置 —— 否则镜头始终被 BOSS 拖着走，
	// 绕到侧背面时 BOSS 会被甩到画面边缘，观感就是"锁了但没锁住"。
	//
	// ⚠⚠ 这里踩过一次坑，改之前务必先读：
	//   第一版把外推写成【固定厘米数】（上限 120cm），结果贴身近战时距离只有 ~150cm，
	//   atan(120/150) ≈ 39° —— 瞄准点被整体推出 BOSS 身体，镜头疯狂晃动、BOSS 也不在画面中心。
	//   这正是本项目早先在肩位偏移上踩过的同一个坑：
	//   **任何横向偏移都必须按【恒定角度】缩放，绝不能写死长度**（长度对距离极度敏感）。
	//   所以现在外推长度 = 到焦点距离 × tan(LockOnPredictAngleDeg)，任何距离下偏角都恒定。
	//
	// ⚠ 默认 0° = 【关闭】：瞄准点恒等于 BOSS 身体中心，绝对居中。
	//   想要"轻微预判"的手感就调到 2~4°；再大就会明显偏离身体中心，不建议。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|LockOn|Camera")
	float LockOnPredictAngleDeg = 0.f;     // 0 = 关闭（默认）；2~4 = 轻微预判

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|LockOn|Camera")
	float LockOnPredictLeadTime = 0.12f;   // 外推距离 = 速度 × 这个时间（再被上面的角度上限夹住）

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|LockOn|Camera")
	float LockOnPredictMinSpeed = 50.f;    // 低于这个速度(cm/s)不预测，避免站桩时镜头漂

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|LockOn|Camera")
	float LockOnPredictStartAngle = 50.f;  // 夹角超过这个度数才开始预测（0°=正对着冲/退，不该偏头）

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|LockOn|Camera")
	float LockOnPredictRampAngle = 35.f;   // 从 StartAngle 平滑升到满值的角度跨度（消除硬开关跳变）

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|LockOn|Camera")
	float LockOnPredictBlendSpeed = 6.f;   // 外推偏移的逐帧插值速度（越大越跟手，越小越柔）

	// 外推偏移的平滑中间值（世界空间，非 UPROPERTY）。
	// 必须存在：不平滑的话，夹角一穿越阈值、或玩家一停步，镜头就会整段跳一下。
	FVector CurrentPredictOffset = FVector::ZeroVector;

	// 镜头/肩位在锁定与解锁之间过渡的速度（越大越干脆，越小越电影感）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|LockOn|Camera")
	float LockOnCameraBlendSpeed = 6.5f;

	// 控制朝向追向目标的速度
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|LockOn|Camera")
	float LockOnRotationSpeed = 9.f;

	// 俯仰限位：防止目标贴近/远离时镜头翻过去或直冲地面。
	// 验收要求锁定后俯仰保持在 -30°~10°（贴合 Spec：俯角不会越漂越大）。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|LockOn|Camera")
	float LockOnMinPitch = -30.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|LockOn|Camera")
	float LockOnMaxPitch = 10.f;

	// ---------- 锁定时的鼠标微调 ----------
	// 允许拖拽的最大角度（度）。别给太大 —— 超过一定角度就该"脱锁重锁"，
	// 而不是让镜头扭到看不见目标。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|LockOn|View")
	float LockOnManualYawLimit = 32.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|LockOn|View")
	float LockOnManualPitchLimit = 22.f;

	// 松手回中速度（0 = 不回中，保留玩家拖到的角度）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|LockOn|View")
	float LockOnViewReturnSpeed = 5.5f;

	// 关掉 → 锁定期间鼠标完全不产生任何视角变化（最接近手柄式纯锁定）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|LockOn|View")
	bool bAllowManualViewWhileLocked = true;

	// ---------- 动画 ----------
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Animation")
	TObjectPtr<UAnimMontage> HitReactMontage;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|Animation")
	TObjectPtr<UAnimMontage> DeathMontage;

private:
	// 把 DefaultMappingContext 注册进本地玩家的 EnhancedInput 子系统。
	// 放在 PossessedBy / BeginPlay 两处调用并做幂等保护：
	// 关卡启动时 Pawn 的 BeginPlay 晚于 Possess（World 还没 BegunPlay），
	// 运行时重生时又相反，两边都调一次最稳。
	void AddDefaultMappingContext();
	bool bMappingContextAdded = false;

	// SetIgnoreAnimRootMotion 用：缓存进入闪避前的 RootMotionMode，结束后还原
	TEnumAsByte<ERootMotionMode::Type> CachedRootMotionMode = ERootMotionMode::RootMotionFromMontagesOnly;
	bool bRootMotionModeCached = false;

	// ---- 位移辅助状态（BeginDisplacementAssist / EndDisplacementAssist） ----
	int32 DisplacementAssistDepth = 0;
	// 位移起点，结束时长出的那段就是“实际位移”
	FVector DisplacementAssistStartLocation = FVector::ZeroVector;
	// 位移前镜头的 CameraLag 状态（开关 + 速度），结束时原样还原
	float DisplacementAssistSavedCameraLagSpeed = 15.f;
	bool DisplacementAssistSavedCameraLagEnabled = true;
	bool bCameraStateSavedByDisplacement = false;

	// 闪避期间被 PushDisableRootMotion 的那段蒙太奇实例（非 UObject，结束时要手动 Pop）
	FAnimMontageInstance* RootMotionSuppressedInstance = nullptr;
	TWeakObjectPtr<UAnimInstance> RootMotionSuppressedAnimInstance;

	// 压制期间每帧复查：确认蒙太奇根位移仍处于被屏蔽状态
	// （蒙太奇实例是延迟一帧才真正建好的，所以 Tick 里再补一次，避免第一帧漏掉）
	void EnsureDodgeRootMotionSuppressed();

	// ---- 镜头冲击状态 ----
	void UpdateCameraPunch(float DeltaTime);

	float CameraPunchRemaining = 0.f;
	float CameraPunchDuration = 0.f;
	float CameraPunchLength = 0.f;
	bool bCameraPunchActive = false;

	// ---- 命中相机微震（旋转 shake，不依赖 UCLASS，自衰减）----
	// 玩家命中 BOSS / 玩家被 BOSS 重击时各来一下，给"砸实了"的冲击感。
	// 实现：在 UpdateCameraPunch 里把衰减后的偏移叠到"相机基准朝向"上（锁定改弹簧臂世界朝向、
	// 自由改控制朝向，两种模式都覆盖）。
	void AddCameraShake(float YawDeg, float PitchDeg, float Duration);
	// 相机每帧"不含震动的基准朝向"：锁定=看向 Boss 的朝向，自由=当前控制朝向。
	// 每帧先捕获它，再叠震动，避免震动被反复累加。
	FRotator CameraBaseRotation = FRotator::ZeroRotator;
	float CameraShakeRemaining = 0.f;
	float CameraShakeDuration = 0.f;
	float CameraShakeYaw = 0.f;
	float CameraShakePitch = 0.f;

	// ---- 受击 FOV 脉冲（Boss 突进命中瞬间 +3~5°、0.1s 回弹）----
	// 声明在 public 段（HUD 命中反馈要用），见上面 PulseCameraFOV 的注释。
	float CameraFOVPulseRemaining = 0.f;
	float CameraFOVPulseDuration = 0.f;
	float CameraFOVPulseAmount = 0.f;
	float CachedDefaultFOV = 90.f;
	bool bFOVPulseCached = false;

	// ---- 锁定镜头状态 ----
	// 每帧把"锁定该用的臂长/肩位"插值到这两个值上；
	// 镜头冲击再在这之上做减法（见 UpdateCameraPunch）。
	// 这么分层是为了两个系统不互相覆盖：以前两边都直接写 TargetArmLength，谁后写谁说了算。
	float RestingArmLength = 380.f;
	FVector RestingSocketOffset = FVector::ZeroVector;

	// 蓝图可能覆盖过相机臂长 / 肩位，BeginPlay 时重新采一次作为基准
	float CachedDefaultArmLength = 380.f;
	FVector CachedDefaultSocketOffset = FVector::ZeroVector;

	// 蓝图里配的"非锁定态是否开启相机滞后"，BeginPlay 缓存一次。
	// 锁定态会强制关掉滞后（见 UpdateLockOnCamera 的注释），解锁时用它还原。
	bool CachedDefaultCameraLag = true;

	// 锁定期间玩家的鼠标拖拽量（度），松手回中
	float LockOnViewYawOffset = 0.f;
	float LockOnViewPitchOffset = 0.f;

	// 原始键轮询的"上一帧状态"，用于做 JustPressed 判定
	bool bLockOnKeyHeld = false;
	bool bSwitchLeftHeld = false;
	bool bSwitchRightHeld = false;
	bool bUseItemKeyHeld = false;

	void UpdateLockOnInput();
	void UpdateUseItemInput();
	void UpdateLockOnCamera(float DeltaTime);

	// ---- 战斗间距（软碰撞）：不许玩家走进敌人身体里 ----
	// 实现见 FightPlayerCharacter.cpp；开关与半径在上面 "Demo|Movement" 分类里。
	void UpdateCombatSeparation(float DeltaTime);

	// 切镜头参数时的兜底：蓝图改过臂长/肩位就按蓝图的来，别写死
	void CacheDefaultCameraRig();

	// 左Shift 短按/长按 分流状态
	bool bShiftRunActive = false;
	FTimerHandle ShiftHoldTimerHandle;

	// “左Shift 当前是否处于按下状态”。
	// 同一个物理键会同时命中多个 Input Action（IA_Run / IA_Flash / IA_Dodge 都挂了同一套
	// 短按闪避/长按奔跑逻辑），用它给 Started 与 Completed/Canceled 配对去重，
	// 保证一次点按只触发一次闪避，不会因为重复事件刷出两次闪避。
	bool bShiftHeld = false;

	// 移动输入锁的【持有层数】（引用计数，>0 = 锁住）。
	// 由 LockMoveInput() / UnlockMoveInput() 维护，不要直接改这个变量。
	// ⚠ 曾经这里是 bool bMoveInputLocked → 多技能叠加时互相误解锁，见上面那段注释。
	int32 MoveInputLockCount = 0;

	// ⚠ 移动锁看门狗：见 Tick 里的说明。防止任何"加了锁没解锁"的路径把玩家永久定死。
	// 阈值取 3 秒：足够覆盖目前最长的技能（完美弹反硬直 1.3s + 后摇），
	// 又短到玩家能立刻感知并恢复操作，不会"卡着等半天"。
	// ⚠ 不能用 UPROPERTY(BlueprintReadWrite)：这个成员在 private 段，UHT 会报
	//   "BlueprintReadWrite should not be used on private members"。保持纯 C++ 成员。
	float MoveLockStaleTimeout = 3.f;
	float StaleMoveLockTimer = 0.f;


	// ---- 道具：回血"酒" ----
	// HealItemCount 是运行时持有量（开局由 DemoBalance::HealItemInitialCount 初始化），不序列化；
	// HealItemMaxCount 是携带上限（开局由 DemoBalance::HealItemMaxCount 覆盖），可在蓝图调。
	int32 HealItemCount = 0;

	// 反向引用到战斗 HUD（由 AFightGameMode 在绑定 HUD 后写入），UGA_PlayerUseHeal 的绿色反馈用。
	TObjectPtr<UCombatHUDWidget> CombatHUD = nullptr;

	// 计时器到点回调：按住超过 RunHoldThreshold → 进入奔跑
	void OnShiftHoldExpired();

	// 诊断用：记录 IA_Move 已经打印过的不同输入值。
	// 数字键（W/A/S/D）不加修饰器时输入恒为 (1,0)，四个方向会全部变成"向右"，
	// 光看画面很难判断，把值打到 Output Log 就能一眼确认。
	TArray<FVector2D> LoggedMoveAxes;

	void HandleMove(const FInputActionValue& Value);
	// 松开 WASD → 清空“最近移动输入”，保证闪避不会复用旧方向
	void HandleMoveReleased();
	void HandleLook(const FInputActionValue& Value);
	void HandleJump();
	void HandleRunStart();
	void HandleRunEnd();
	void HandleNormalAttack();
	void HandleAttackSkill();
	void HandleDefenseStarted();

	// 完美闪避派生：若正在闪避且处于"派生窗"（State_DodgePerfect），
	// 取消闪避后摇并立即接招（"闪A"）。返回 true 表示已派生取消。
	bool TryPerfectDodgeFollowupCancel();
	void HandleDefenseCompleted();

	bool IsAbilityActive(TSubclassOf<UGameplayAbility> AbilityClass) const;
	// 普攻是否正在进行（用于攻击时禁用 WASD 移动）
	bool IsNormalAttacking() const;
	// 返回是否激活成功。
	// 弹刀要靠这个返回值区分两种情况："起手成功" vs "技能已经在跑"
	// —— 后者必须补发 Event_Ability_ParryRewindow 才能实现连弹（见 HandleDefenseStarted）。
	bool TryActivateCombatAbility(TSubclassOf<UGameplayAbility> AbilityClass);

	// 按资产路径强制授予一个战斗技能（蓝图子类优先，加载不到则退回原生 C++ 类）。
	// 授予前会移除已存在的同角色技能，避免 BP 配置错配导致重复授予。
	void GrantCombatAbility(const FGameplayTag& RoleTag, TSubclassOf<UGameplayAbility> NativeFallback);
	void SendComboInput();

	// 优先用显式配置，没配就按 AbilityTag 从 DefaultAbilities 里找
	TSubclassOf<UGameplayAbility> ResolveAbilityByTag(const FGameplayTag& AbilityTag) const;
};
