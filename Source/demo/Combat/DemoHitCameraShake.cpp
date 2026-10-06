#include "Combat/DemoHitCameraShake.h"
#include "Core/DemoCombatTuning.h"

UDemoHitCameraShake::UDemoHitCameraShake()
{
	// ---- 时长与淡入淡出 ----
	// 命中震动必须"来得快去得快"：0.01s 淡入 = 命中瞬间立刻到位（没有淡入就等于没打中），
	// 0.10s 淡出让震尾自然收敛；总时长 0.18s —— 比单帧长、比一个攻击节奏短，
	// 连段时每刀都震但不叠加成持续眩晕。
	OscillationDuration = DemoBalance::HitShakeDuration;
	OscillationBlendInTime = 0.01f;
	OscillationBlendOutTime = 0.10f;
	// 播放空间：UCameraShakeBase 默认就是 CameraLocal（相机局部），无需显式设置。

	// ---- 旋转震荡（主体：命中"错位感"的来源）----
	// 偏航给满、俯仰减半：水平方向的震最像"被后坐力顶了一下"，
	// 垂直震多了会像晕车。24Hz 在 0.18s 里约 4 个来回 —— 数得清但够密。
	RotOscillation.Yaw.Amplitude = 2.2f;
	RotOscillation.Yaw.Frequency = 24.f;
	RotOscillation.Pitch.Amplitude = 1.3f;
	RotOscillation.Pitch.Frequency = 24.f;
	// 滚转微量：加一点"斜了一下"的味道，纯 Yaw+Pitch 会显得太"平"
	RotOscillation.Roll.Amplitude = 0.6f;
	RotOscillation.Roll.Frequency = 18.f;

	// ---- 位移震荡（辅助：沉重的"砸"感）----
	// 只做垂直（相机被向下砸再弹回），幅度小 —— 位移震大了会直接把画面推出目标。
	LocOscillation.Z.Amplitude = 3.f;
	LocOscillation.Z.Frequency = 20.f;
	// X/Y 不震：前后震会和臂长冲击(ApplyCameraPunch)打架，横向震会干扰锁定目标居中。
}
