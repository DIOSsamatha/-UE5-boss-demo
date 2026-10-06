// 目标锁定：自动挑选视野中心附近最近的敌人；锁定后角色始终面向目标、镜头对准目标。
//
// 分工（这一点很关键，别混）：
//   · 本组件  —— 管「锁谁」+「角色朝哪」，即目标选取、失效判定、角色 Yaw 跟随。
//   · 玩家角色 —— 管「镜头怎么摆」，即控制朝向、臂长、肩部偏移、俯仰限位、鼠标微调回中。
// 之所以把镜头拆出去：镜头参数是纯表现，且要和镜头冲击（弹反拉近）共用同一个出口，
// 放在角色里才拿得到 ApplyCameraPunch 的状态。
//
// 锁定时会把角色移动组件的朝向模式改成「外部驱动」（两个开关都关掉），
// 由本组件每帧把 Yaw 插值到目标方向 —— 这样玩家的鼠标微调只动镜头、不动身体。
// 解锁时【原样还原】进入前的设置，绝不写死成某个值。
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "LockOnComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDemoOnLockTargetChanged, AActor*, NewTarget);

UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class DEMO_API ULockOnComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	ULockOnComponent();

	// 已经锁着就解锁，没锁就去找一个锁上（中键的默认行为）
	UFUNCTION(BlueprintCallable, Category = "Demo|LockOn")
	void ToggleLockOn();

	// 返回是否成功锁到目标（false = 视野里没有符合条件的敌人）
	UFUNCTION(BlueprintCallable, Category = "Demo|LockOn")
	bool EnableLockOn();

	UFUNCTION(BlueprintCallable, Category = "Demo|LockOn")
	void DisableLockOn();

	// 左右切换目标（按角色当前面向来分左右）
	UFUNCTION(BlueprintCallable, Category = "Demo|LockOn")
	void SwitchTarget(bool bToRight);

	// 只做筛选、不改状态：给 UI 预览、外部 AI 复用
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|LockOn")
	AActor* FindBestTarget() const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|LockOn")
	bool IsLockedOn() const { return LockedTarget != nullptr; }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|LockOn")
	AActor* GetLockedTarget() const { return LockedTarget; }

	// 已锁定时长（秒）——准星做入场动画用它，不用自己再记一个计时器
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|LockOn")
	float GetLockedDuration() const { return LockedTime; }

	// 镜头该看哪儿：目标身上「胸口偏上」那一点。
	// 不做成"目标原点"是为了不让镜头盯着脚下 —— 目标是有高度的，瞄 Z=0 会一路往下压视角。
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Demo|LockOn")
	FVector GetFocusLocation() const;

	// 目标换了 / 解除锁定都会广播（nullptr = 已解锁）
	UPROPERTY(BlueprintAssignable, Category = "Demo|LockOn")
	FDemoOnLockTargetChanged OnLockTargetChanged;

protected:
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// ---------- 选目标 ----------
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|LockOn")
	float MaxLockDistance = 1800.f;

	// 超过这个距离自动脱锁。要比 MaxLockDistance 大 —— 否则刚锁上就掉，
	// 因为玩家一边打一边走位，距离是持续变化的。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|LockOn")
	float LockBreakDistance = 2800.f;

	// 太近就不锁（贴脸时锁上会让镜头糊在 BOSS 身上）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|LockOn")
	float MinLockDistance = 60.f;

	// 目标必须落在"镜头正前方"这个夹角内才算候选
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|LockOn")
	float MaxLockAngle = 80.f;

	// 切换目标时允许的角度（比 MaxLockAngle 宽：已经锁上了就别太挑）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|LockOn")
	float SwitchTargetMaxAngle = 140.f;

	// 只锁定这种类（留空 = 任何带 ASC 的敌对角色）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|LockOn")
	TSubclassOf<AActor> TargetClassFilter;

	// 目标被挡住时是否脱锁。默认关：战斗中柱子/柱子常常挡住射线，
	// 频繁掉锁比"隔着障碍锁定"更让人烦。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|LockOn")
	bool bBreakOnLineOfSightBlocked = false;

	// ---------- 朝向 ----------
	// 锁定时由本组件驱动角色 Yaw。关掉它就只锁镜头不锁身体（个别项目喜欢这样）。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|LockOn")
	bool bDriveOwnerRotation = true;

	// 转向目标的速度（度/秒 → RInterpTo 的插值速度）
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|LockOn")
	float TurnSpeed = 11.f;

	// Turn In Place 死区：当前朝向与目标朝向的夹角【小于】这个度数时不强制转向，
	// 用来吃掉"每帧都在做零点几度的微调"造成的细微抖动。
	//
	// ⚠ 默认 0 = 不做死区、持续精确追踪目标（与历史行为一致）。
	//
	// 踩坑记录：先后试过 35° 和 5° 的死区，两次都出问题 ——
	//   35° 太大：锁定后角色最多偏 35° 不转身，持剑对峙里"锁定感"直接没了；
	//   5° 也不安全：相机挂在 480cm 的长臂上，角色朝向哪怕只差 5°，
	//   换算到相机位置就是 480×sin(5°) ≈ 42cm 的横移；而且死区会让角色
	//   "停在死区边缘→超过→转回来→再停"形成台阶式微抖，反而更晃。
	// 结论：锁定态就该【持续】平滑追踪（RInterpTo 本身就是指数收敛，不会抖），
	// 死区在这里有害无益。除非有实测数据证明需要滤噪，否则保持 0。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|LockOn")
	float TurnInPlaceAngleThreshold = 0.f;

	// ---------- 出招期间的【弱跟随】（2026-10-02 新增，治"连打打到空气"）----------
	//
	// 背景：原先出招期间直接 return 不管朝向（为了不让镜头乱摇），
	//   结果连续攻击时朝向完全失控 → 玩家绕圈连打，身体越歪越远 → 后续攻击全砍空。
	// 现在出招期间改做【弱跟随】：不碰镜头、不抢根运动方向盘，只在"明显打空"时慢慢拉回。
	//
	// ① 出招期间转向速度 = TurnSpeed / AttackTurnSpeedScale。
	//    常态 TurnSpeed=11；取 4 → 出招期间只有 2.75 度/秒，非常"懒"，
	//    不足以和招式根运动抢方向盘（镜头不会乱摇），但几十帧下来足够把身体拉回目标方向。
	// ② 偏差 < AttackTurnAngleThreshold 时【完全不动】。
	//    这是防抖的关键：绝大多数帧都不碰身体，镜头彻底安静。
	//    取 30°：差 30° 以内攻击基本还是砍在 BOSS 身上（BOSS 宽 + 剑有长度），
	//    超过就属于"明显打空"，才值得拉。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|LockOn")
	float AttackTurnSpeedScale = 4.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|LockOn")
	float AttackTurnAngleThreshold = 30.f;

	// ---------- 焦点高度（准星画在目标身上的哪一点）----------
	//
	// 焦点 = 胶囊中心 + 半高 × FocusHeightRatio + FocusHeightOffset（世界 cm）。
	//
	// ⚠ 为什么要有这两个值：以前焦点用的是【整个 Actor 的包围盒】中心 + 半高 × 0.35，
	//   而包围盒会把武器 / 挂件一起算进去 —— 大型 BOSS 的武器常常高过头顶，
	//   焦点被抬到领口以上，屏幕上看起来就像"锁定在头顶血条上"。
	//   现在只取胶囊体，才是"锁在人身上"（具体比例见下方 FocusHeightRatio）。

	// 0.0：焦点 = 胶囊中心（角色身体正中）。这是"锁在身上"最稳的点 ——
	//   之前 0.25 落在 162cm 上胸，相机上仰、浮动血条（头顶 295cm）显得更"抢眼"，
	//   观感就像"锁定在血条上"。归零到身体正中后，BOSS 在画面里更居中、俯角更平。
	// ⚠ 这个值要和 FightPlayerCharacter::UpdateLockOnCamera 一起看：
	//   那边从【相机】位置算朝向，本焦点才会精确落在画面正中。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|LockOn")
	float FocusHeightRatio = 0.0f;

	// 想要更精确就直接给绝对上抬量（比如固定 +40cm），和上面那个比例是相加关系
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Demo|LockOn")
	float FocusHeightOffset = 0.f;

private:
	TArray<AActor*> CollectCandidates() const;
	AActor* PickBestCandidate(const TArray<AActor*>& Candidates, bool bRelativeToLocked) const;
	void SetLockedTarget(AActor* NewTarget);

	// 进入 / 退出「朝向由本组件驱动」模式。原样保存与还原，不写死任何值。
	void EnterExternalRotationMode();
	void ExitExternalRotationMode();

	UPROPERTY()
	TObjectPtr<AActor> LockedTarget = nullptr;

	float LockedTime = 0.f;

	// 进入锁定前角色移动组件的样子
	bool bSavedOrientRotationToMovement = true;
	bool bSavedUseControllerDesiredRotation = false;
	bool bRotationModeOverridden = false;
};
