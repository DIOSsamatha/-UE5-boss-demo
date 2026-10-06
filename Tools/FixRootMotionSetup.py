# -*- coding: utf-8 -*-
"""
FixRootMotionSetup.py
=====================
一次性把"位移像动画、播完拉回"的根因在【资产】上彻底修掉。

背景（引擎源码级结论，不用猜）：
  1) 蒙太奇姿势里根骨骼是否被重置，取决于 AnimInstanceProxy.cpp:1604：
         bExtractRootMotion = Montage->HasRootMotion() && RootMotionMode != NoRootMotionExtraction
     - RootMotionMode == NoRootMotionExtraction 时：根骨骼【不重置】→ 动画自带的位移留在
       Mesh 上（看着在动），蒙太奇播完/被下一段切走时 Mesh 弹回胶囊体原点 = "拉回"；
       同时蒙太奇根位移也进不了 CharacterMovement → 胶囊体压根没动
       = "是动画而不是真正的移动"。
  2) Montage->HasRootMotion() 又取决于它引用的 AnimSequence 的 bEnableRootMotion
     （FAnimTrack::HasRootMotion → AnimReference->HasRootMotion()）。
     源动画没勾 Enable Root Motion 时，效果和上面第 1 条一样。

所以本脚本做三件事：
  A. 把 ABP_FightPlayer 的 Root Motion Mode 锁成 RootMotionFromMontagesOnly；
  B. 给所有出招用的源动画补勾 Enable Root Motion；
  C. 顺手把闪避距离参数写进 GA_Dodge（120cm / 上限 150 / 0.32s），
     与 C++ 默认值保持一致，避免蓝图 CDO 残留旧值。

运行：UE 编辑器 -> Output Log 右下角切 Python -> 输入框填本文件绝对路径 -> 回车。
      K:/selfgame/demo/Tools/FixRootMotionSetup.py
无需重编 C++、无需重启（C++ 侧运行时会做同样的自愈，本脚本只是把资产也摆正）。
"""

import unreal

ASSET_LIB = unreal.EditorAssetLibrary

ABP_PATH = "/Game/FightPlayer/Animation/ABP_FightPlayer"
GA_DODGE = "/Game/FightPlayer/Abilities/GA_Dodge"
PLAYER_BP = "/Game/FightPlayer/System/BP_FightPlayer"

# 出招用到的源动画（必须勾 Enable Root Motion，蒙太奇才有根位移数据）
SOURCE_ANIMS = [
    "/Game/SwordAnimsetPro/Animations/Root-Motion/Attack_Combo_01_01_Anim",
    "/Game/SwordAnimsetPro/Animations/Root-Motion/Attack_Combo_01_02_Anim",
    "/Game/SwordAnimsetPro/Animations/Root-Motion/Attack_Combo_01_03_Anim",
    "/Game/SwordAnimsetPro/Animations/Root-Motion/Attack_Combo_01_04_Anim",
    "/Game/SwordAnimsetPro/Animations/Root-Motion/Dodge_F_Anim",
    "/Game/SwordAnimsetPro/Animations/Root-Motion/Dodge_B_Anim",
    "/Game/SwordAnimsetPro/Animations/Root-Motion/Dodge_L_Anim",
    "/Game/SwordAnimsetPro/Animations/Root-Motion/Dodge_R_Anim",
]

# 与 PlayerAbilities.h 的 C++ 默认值保持一致
DODGE_DISTANCE = 120.0
DODGE_DISTANCE_MAX = 150.0
DODGE_DURATION = 0.32


def log(msg, warn=False):
    if warn:
        unreal.log_warning(msg)
    else:
        unreal.log(msg)


# ----------------------------------------------------------------- A
def fix_root_motion_mode():
    log("========== A. ABP Root Motion Mode ==========")
    cdo = unreal.load_object(None, f"{ABP_PATH}.Default__ABP_FightPlayer_C")
    if cdo is None:
        # 有些版本生成类名不带 _C 后缀，兜底再试一次
        cdo = unreal.load_object(None, f"{ABP_PATH}.ABP_FightPlayer_C")
    if cdo is None:
        log(f"[FAIL] 拿不到 {ABP_PATH} 的生成类 CDO（路径/类名不对？），跳过 A。", warn=True)
        return

    try:
        cur = cdo.get_editor_property("root_motion_mode")
    except Exception as e:
        log(f"[FAIL] 读不到 root_motion_mode：{e}", warn=True)
        return

    log(f"当前 Root Motion Mode = {cur}（0=Everything 1=MontagesOnly 2=NoExtraction 3=Ignore）")

    target = None
    try:
        target = unreal.RootMotionMode.ROOT_MOTION_FROM_MONTAGES_ONLY
    except Exception:
        target = 1   # 枚举名不可用时直接给数值

    try:
        cdo.set_editor_property("root_motion_mode", target)
        ASSET_LIB.save_asset(ABP_PATH)
        log(f"已写入 Root Motion Mode = {target}（RootMotionFromMontagesOnly）并保存", warn=True)
    except Exception as e:
        log(f"[FAIL] 写入失败：{e}（可在编辑器里手动改：打开 ABP_FightPlayer -> "
            f"Class Defaults -> Root Motion -> Root Motion Mode = Root Motion from Montages Only）", warn=True)


# ----------------------------------------------------------------- B
def fix_source_animations():
    log("========== B. 源动画 Enable Root Motion ==========")
    ok = 0
    for path in SOURCE_ANIMS:
        asset = ASSET_LIB.load_asset(path)
        if asset is None:
            log(f"[SKIP] 找不到 {path}", warn=True)
            continue
        try:
            before = asset.get_editor_property("enable_root_motion")
        except Exception as e:
            log(f"[SKIP] {path.rsplit('/', 1)[-1]} 读不到 enable_root_motion：{e}", warn=True)
            continue
        if before:
            log(f"  OK   {path.rsplit('/', 1)[-1]}：已勾选")
            ok += 1
            continue
        try:
            asset.set_editor_property("enable_root_motion", True)
            ASSET_LIB.save_asset(path)
            log(f"  FIX  {path.rsplit('/', 1)[-1]}：enable_root_motion False -> True（已保存）", warn=True)
            ok += 1
        except Exception as e:
            log(f"  FAIL {path.rsplit('/', 1)[-1]}：{e}", warn=True)
    log(f"源动画检查完成：{ok}/{len(SOURCE_ANIMS)} 可用")


# ----------------------------------------------------------------- C
def fix_dodge_params():
    log("========== C. GA_Dodge 闪避距离参数 ==========")
    if ASSET_LIB.load_blueprint_class(GA_DODGE) is None:
        log(f"[FAIL] 加载 {GA_DODGE} 失败，跳过 C。", warn=True)
        return
    cdo = unreal.load_object(None, f"{GA_DODGE}.Default__GA_Dodge_C")
    if cdo is None:
        log("[FAIL] 找不到 CDO Default__GA_Dodge_C，跳过 C。", warn=True)
        return

    for prop, value in (("dodge_distance", DODGE_DISTANCE),
                        ("dodge_distance_max", DODGE_DISTANCE_MAX),
                        ("dodge_duration", DODGE_DURATION)):
        try:
            cdo.set_editor_property(prop, value)
            log(f"  {prop} = {value}")
        except Exception as e:
            log(f"  {prop} 写入失败：{e}", warn=True)
    ASSET_LIB.save_asset(GA_DODGE)

    # 位移期间的相机模式（属性名已改为 ..._displacement，闪避与普攻共用）
    p_cdo = unreal.load_object(None, f"{PLAYER_BP}.Default__BP_FightPlayer_C")
    if p_cdo:
        for prop in ("b_rigid_camera_during_displacement", "b_rigid_camera_during_dodge"):
            try:
                p_cdo.set_editor_property(prop, True)
                ASSET_LIB.save_asset(PLAYER_BP)
                log(f"  BP_FightPlayer.{prop} = True")
                break
            except Exception:
                continue
    else:
        log("  找不到 BP_FightPlayer 的 CDO（默认值已是 True，可忽略）", warn=True)


# ----------------------------------------------------------------- 复核
def verify():
    log("========== 复核 ==========")
    cdo = unreal.load_object(None, f"{ABP_PATH}.Default__ABP_FightPlayer_C")
    if cdo:
        try:
            log(f"ABP Root Motion Mode = {cdo.get_editor_property('root_motion_mode')}")
        except Exception as e:
            log(f"读 root_motion_mode 失败：{e}", warn=True)
    d = unreal.load_object(None, f"{GA_DODGE}.Default__GA_Dodge_C")
    if d:
        log(f"DodgeDistance    = {d.get_editor_property('dodge_distance')} cm")
        log(f"DodgeDistanceMax = {d.get_editor_property('dodge_distance_max')} cm")
        log(f"DodgeDuration    = {d.get_editor_property('dodge_duration')} s")
    log("进 PIE 后看 Output Log：")
    log("  [RootMotion] 普攻：根位移模式 ... （期望已是 RootMotionFromMontagesOnly(1)）")
    log("  [Combo] 第 N 段：HasRootMotion=1 ...")
    log("  [Combo] 本轮普攻实际位移 = XX cm（>0 才是真正的胶囊体位移）")
    log("  [Dodge] 本次实际位移 = XXX cm（目标 120 cm）")


fix_root_motion_mode()
fix_source_animations()
fix_dodge_params()
verify()
