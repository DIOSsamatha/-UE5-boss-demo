// 命中 Screen Shake：真·镜头震动（走 PlayerCameraManager 的震动管线）。
//
// 和既有三通道的分工（见 Core/DemoCombatTuning.h "打击感参数包"）：
//   · ApplyCameraPunch    —— 拉近臂长（非锁定）
//   · PulseCameraFOV      —— 扩视野（锁定安全）
//   · PlayHitImpactShake  —— 相机相对旋转微震（自研 sin 衰减，锁定安全）
//   · 本类                —— 整个 PlayerCameraManager 级的位移+旋转震荡，
//                            "世界在手里震了一下"的分量感，四道叠加构成完整命中冲击。
//
// 免资产：波形全部在构造函数里用 DemoBalance 常量写死，蓝图/编辑器里不需要配任何东西。
// 强度由调用方 StartCameraShake(Class, Scale) 的 Scale 控制（0~1+），
// 时长分两档（普通/重击）→ 用两个子类？不必：Scale 缩放振幅足够，时长取中庸值。
#pragma once

#include "CoreMinimal.h"
// UE 5.7：LegacyCameraShake 挪进了 EngineCameras 插件（引擎自带、默认启用）
#include "Shakes/LegacyCameraShake.h"
#include "DemoHitCameraShake.generated.h"

UCLASS()
class DEMO_API UDemoHitCameraShake : public ULegacyCameraShake
{
	GENERATED_BODY()

public:
	UDemoHitCameraShake();
};
