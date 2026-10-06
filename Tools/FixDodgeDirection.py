# 修正 GA_Dodge 的方向蒙太奇顺序 + 闪避手感参数。
#
# 症状：按 W 向右闪、按 S 向左闪、按 A 向后闪、按 D 向前闪，且闪避距离明显偏长。
# 根因：Tools/FixCombatAbilityWiring.py 把 DirectionalMontages 按 [右,左,后,前] 写进了蓝图 CDO，
#       而代码约定是 [前,后,左,右] —— 正好构成一次“前后/左右互换”，
#       再加上闪避动画自带根位移会叠加到技能的定距位移上（距离变长、方向被动画带偏）。
#       C++ 侧已改成“按资产名认方向 + 双层压制动画根位移”（见 UGA_PlayerDodge /
#       AFightPlayerCharacter::SuppressMontageRootMotion），本脚本把蓝图里的数组顺序也摆正，
#       避免以后在编辑器里看配置时又被误导。
#
# 运行：UE 编辑器 -> Output Log 右下角切 Python -> 输入框填本文件绝对路径 -> 回车。
# 无需重编 C++、无需重启。

import unreal

ASSET_LIB = unreal.EditorAssetLibrary

GA_DODGE = "/Game/FightPlayer/Abilities/GA_Dodge"
PLAYER_BP = "/Game/FightPlayer/System/BP_FightPlayer"
MONTAGE_DIR = "/Game/FightPlayer/Animation"

# 期望顺序（下标 0..3）：前 / 后 / 左 / 右
ORDER = ["F", "B", "L", "R"]

# ---- 闪避手感参数（想改“闪多远 / 多快”直接改这里）----
# 注意：这几个值必须与 C++ 默认值（PlayerAbilities.h 的 DodgeDistance / DodgeDistanceMax /
#       DodgeDuration）保持一致，否则跑到这个脚本会把 C++ 里的改动又写回旧值。
DODGE_DISTANCE = 120.0   # 厘米。120 = 1.2 米；嫌远改 100，嫌近改 140
DODGE_DISTANCE_MAX = 150.0  # 厘米。距离上限（C++ 侧会 Clamp，防止蓝图残留旧值让改动失效）
DODGE_DURATION = 0.32    # 秒。位移在这段时间内匀速走完，越短越“爆”
RIGID_CAMERA = True      # 位移期间相机是否与角色刚性同步（视角跟着角色一起走）


def dir_of(asset_name):
    """从资产名里认方向：Dodge_F / ..._Forward / ... 等。"""
    n = asset_name.upper()
    if "_FORWARD" in n:
        return "F"
    if "_BACK" in n:
        return "B"
    if "_LEFT" in n:
        return "L"
    if "_RIGHT" in n:
        return "R"
    if "_F" in n:
        return "F"
    if "_B" in n:
        return "B"
    if "_L" in n:
        return "L"
    if "_R" in n:
        return "R"
    return None


def load_dodge_cdo():
    if ASSET_LIB.load_blueprint_class(GA_DODGE) is None:
        unreal.log_error(f"[FAIL] 加载 {GA_DODGE} 的生成类失败，路径对吗？")
        return None
    cdo = unreal.load_object(None, f"{GA_DODGE}.Default__GA_Dodge_C")
    if cdo is None:
        unreal.log_error("[FAIL] 找不到 CDO Default__GA_Dodge_C")
    return cdo


cdo = load_dodge_cdo()
if cdo is None:
    unreal.log_error("中止：GA_Dodge 没加载成功。")
else:
    cur = cdo.get_editor_property("directional_montages")
    names = [m.get_name() if m else "None" for m in (cur or [])]
    unreal.log_warning(f"当前 DirectionalMontages（下标 0..3 应为 前/后/左/右）：{names}")

    by_dir = {}
    for m in (cur or []):
        if m is None:
            continue
        d = dir_of(m.get_name())
        if d and d not in by_dir:
            by_dir[d] = m

    # 缺哪段就按约定路径补
    for d in ORDER:
        if d not in by_dir:
            p = f"{MONTAGE_DIR}/Dodge_{d}_Montage"
            m = ASSET_LIB.load_asset(p)
            if m:
                by_dir[d] = m
                unreal.log(f"[补] {p}")
            else:
                unreal.log_error(f"[FAIL] 找不到 {p}")

    if len(by_dir) == 4:
        fixed = [by_dir[d] for d in ORDER]
        cdo.set_editor_property("directional_montages", fixed)
        unreal.log_warning(f"已改为 [前, 后, 左, 右] = {[m.get_name() for m in fixed]}")
    else:
        unreal.log_error("4 段没全部识别到，DirectionalMontages 保持不变（C++ 侧仍会按名字选，不影响功能）。")

    if "F" in by_dir:
        cdo.set_editor_property("dodge_montage", by_dir["F"])   # 单段兜底 = 前

    cdo.set_editor_property("dodge_distance", DODGE_DISTANCE)
    cdo.set_editor_property("dodge_distance_max", DODGE_DISTANCE_MAX)
    cdo.set_editor_property("dodge_duration", DODGE_DURATION)
    ASSET_LIB.save_asset(GA_DODGE)

    # 玩家 BP：位移期间的相机模式（属性名已从 bRigidCameraDuringDodge 改名为
    # bRigidCameraDuringDisplacement —— 现在普攻和闪避共用同一套“位移辅助”）
    p_cdo = unreal.load_object(None, f"{PLAYER_BP}.Default__BP_FightPlayer_C")
    if p_cdo:
        wrote = False
        for prop in ("b_rigid_camera_during_displacement", "b_rigid_camera_during_dodge"):
            try:
                p_cdo.set_editor_property(prop, RIGID_CAMERA)
                ASSET_LIB.save_asset(PLAYER_BP)
                unreal.log_warning(f"BP_FightPlayer.{prop} = {RIGID_CAMERA}")
                wrote = True
                break
            except Exception as e:
                unreal.log_warning(f"{prop} 写入失败：{e}")
        if not wrote:
            unreal.log_warning("跳过相机属性写入（默认值已经是 True，无需处理）。")
    else:
        unreal.log_warning("找不到 BP_FightPlayer 的 CDO，跳过相机属性写入（默认 True）。")

    # ---------------- 复核 ----------------
    cls2 = ASSET_LIB.load_blueprint_class(GA_DODGE)
    cdo2 = unreal.load_object(None, f"{GA_DODGE}.Default__GA_Dodge_C")
    if cdo2:
        n2 = [m.get_name() if m else "None" for m in (cdo2.get_editor_property("directional_montages") or [])]
        unreal.log_warning("========== 复核 ==========")
        unreal.log_warning(f"DirectionalMontages = {n2}")
        unreal.log_warning(f"DodgeDistance      = {cdo2.get_editor_property('dodge_distance')} cm")
        unreal.log_warning(f"DodgeDistanceMax   = {cdo2.get_editor_property('dodge_distance_max')} cm")
        unreal.log_warning(f"DodgeDuration      = {cdo2.get_editor_property('dodge_duration')} s")
        unreal.log_warning("期望：按下 W 朝屏幕前方闪、D 向右、A 向左、S 向后；不按方向键时向前闪。")
