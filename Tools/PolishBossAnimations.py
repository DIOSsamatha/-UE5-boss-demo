# -*- coding: utf-8 -*-
"""
PolishBossAnimations.py
=======================
把 BOSS / 玩家两边所有蒙太奇的「混合参数」统一成一套手调过的值。

为什么单独做这一步：
  动作"不流畅"在 UE 里绝大多数不是动画本身的问题，而是**切换时没有交叉淡入淡出**。
  蒙太奇默认的 BlendIn/BlendOut 是 0 —— 上一段动画瞬间断电、下一段瞬间上电，
  表现就是每次出招、每次切状态都"啪"地跳一下。
  这个脚本要做的就是给每个蒙太奇补上合适的混合时间。

参数不是拍脑袋定的，按"这个动作是被什么打断的"分四档：

  档位            混合进/出      用在什么动作            为什么
  ------------    -----------    --------------------    ------------------------------------------
  急停/受击       0.04 / 0.10    受击、闪避              被打断必须立刻反应，混太久等于"延迟吃招"
  攻击起手        0.08 / 0.18    挥砍、突进、弹幕施法    起手要快（不然读招时间变长），收招要稳
  蓄力/循环       0.12 / 0.12    蓄力循环、狂化循环      循环段首尾姿势接近，混合长一点更顺
  死亡/演出       0.12 / 0.30    死亡、阶段演出          演出类要给足时间，戛然而止会很廉价

另外还会设置 BlendOutTriggerTime（提前多久开始淡出）：
  -1  = 淡出结束时刚好动画播完（循环段/死亡用这个，避免"还没播完就开始消失"）
  ≥0  = 在"总长 - 该值"处开始淡出，让淡出在动画结束时刚好归零（攻击收招用，收得干净）

用法（编辑器内）：
  菜单 Tools → Execute Python Script...，或 Output Log 切到 Python 模式：
      exec(open(r"K:/selfgame/demo/Tools/PolishBossAnimations.py", encoding="utf-8").read())

⚠ 这个脚本是「幂等」的：重复跑只会把参数写回同一套值，不会越跑越乱。
⚠ 它只改动画资产的混合参数，不碰 C++、不碰骨骼、不碰根运动。
"""

import unreal

ASSET_LIB = unreal.EditorAssetLibrary
EAL = ASSET_LIB

SEARCH_DIRS = [
    "/Game/Boss/Animation",
    "/Game/FightPlayer/Animation",
]

# FAlphaBlend 结构体上的属性名在不同版本里可能是 blend_time 或 blend_time_seconds
BLEND_TIME_KEYS = ("blend_time", "blend_time_seconds")


class Profile:
    """一套混合参数"""

    def __init__(self, blend_in, blend_out, option, trigger, label):
        self.blend_in = blend_in
        self.blend_out = blend_out
        self.option = option
        self.trigger = trigger
        self.label = label


# 档位定义（顺序即匹配优先级：先匹配到的赢）
PROFILES = [
    # ---- 受击 / 被打断：必须"立刻"响应 ----
    (("Hit_", "Stagger", "Weak_In", "Weak_Recover"), Profile(
        0.04, 0.10, unreal.AlphaBlendOption.LINEAR, 0.06, "急停/受击")),

    # ---- 循环 / 持续段 ----
    (("Loop", "Hold", "Idle"), Profile(
        0.12, 0.12, unreal.AlphaBlendOption.CUBIC, -1.0, "蓄力/循环")),

    # ---- 死亡 / 演出（阶段转换、狂化进出）----
    (("Death", "Dead", "Phase2_Enter", "Phase2_Out"), Profile(
        0.12, 0.30, unreal.AlphaBlendOption.CUBIC, -1.0, "死亡/演出")),

    # ---- 闪避：比受击更急，闪避接闪避要能连上 ----
    (("Dodge",), Profile(
        0.04, 0.10, unreal.AlphaBlendOption.LINEAR, 0.05, "闪避")),

    # ---- 攻击起手（近战 / 突进 / 弹幕 / 弹反 / 大招收力）----
    (("Slash", "Charge", "Ranged", "Attack", "Parry", "Ult_Charge", "Ult_Out"), Profile(
        0.08, 0.18, unreal.AlphaBlendOption.CUBIC, 0.10, "攻击起手")),
]

DEFAULT_PROFILE = Profile(0.10, 0.18, unreal.AlphaBlendOption.CUBIC, 0.08, "默认")


def pick_profile(name):
    for patterns, profile in PROFILES:
        for p in patterns:
            if p.lower() in name.lower():
                return profile
    return DEFAULT_PROFILE


def set_blend_struct(montage, prop_name, time_value, option_value):
    """写 FAlphaBlend 结构（blend_in / blend_out）。

    UE5 把这俩从裸 float 换成了 FAlphaBlend 结构（含 blend_time 与 blend_option），
    但旧字段 blend_in_time 在某些版本还在。两条路都试，能写进去就行。
    """
    try:
        blend = montage.get_editor_property(prop_name)
    except Exception:
        blend = None

    if blend is not None and hasattr(blend, "set_editor_property"):
        wrote = False
        for key in BLEND_TIME_KEYS:
            try:
                blend.set_editor_property(key, time_value)
                wrote = True
                break
            except Exception:
                continue
        try:
            blend.set_editor_property("blend_option", option_value)
        except Exception:
            pass
        if wrote:
            try:
                montage.set_editor_property(prop_name, blend)
                return True
            except Exception:
                pass

    # 退化路径：老版本的裸 float 属性
    for key in ("{}_time".format(prop_name),):
        try:
            montage.set_editor_property(key, time_value)
            return True
        except Exception:
            continue
    return False


def set_simple(montage, prop_name, value):
    try:
        montage.set_editor_property(prop_name, value)
        return True
    except Exception:
        return False


def polish_montage(path, name, profile):
    montage = EAL.load_asset(path)
    if montage is None:
        unreal.log_warning("[Polish] 读不到资产：{}".format(path))
        return False

    ok_in = set_blend_struct(montage, "blend_in", profile.blend_in, profile.option)
    ok_out = set_blend_struct(montage, "blend_out", profile.blend_out, profile.option)

    # 提前起淡，让淡出在动画结束时刚好归零（收招干净）
    set_simple(montage, "blend_out_trigger_time", profile.trigger)

    # 播完自动淡出。关掉它的话蒙太奇会"停"在最后一帧硬切回待机 —— 非常明显的一顿
    if not set_simple(montage, "enable_auto_blend_out", True):
        set_simple(montage, "b_enable_auto_blend_out", True)

    EAL.save_loaded_asset(montage)

    unreal.log("[Polish] {:<30} [{}] in={:.2f} out={:.2f} trigger={:+.2f}{}".format(
        name, profile.label, profile.blend_in, profile.blend_out, profile.trigger,
        "" if (ok_in and ok_out) else "  ← 部分属性写入失败（不影响功能）"))
    return True


def main():
    unreal.log_warning("========== PolishBossAnimations 开始 ==========")

    total = 0
    polished = 0
    per_profile = {}

    for directory in SEARCH_DIRS:
        if not EAL.does_directory_exist(directory):
            unreal.log("[Polish] 跳过不存在的目录 {}".format(directory))
            continue

        for path in EAL.list_assets(directory, recursive=True, include_folder=False):
            # list_assets 返回的是完整对象路径，取资产名
            asset_name = path.split("/")[-1].split(".")[0]
            if not asset_name.lower().endswith("_montage"):
                continue

            total += 1
            profile = pick_profile(asset_name)
            if polish_montage(path, asset_name, profile):
                polished += 1
                per_profile[profile.label] = per_profile.get(profile.label, 0) + 1

    unreal.log_warning("[Polish] 共处理 {}/{} 个蒙太奇".format(polished, total))
    for label, count in sorted(per_profile.items(), key=lambda kv: -kv[1]):
        unreal.log_warning("[Polish]    {}：{} 个".format(label, count))

    unreal.log_warning(
        "提示：\n"
        "  · 混合时间调完是「立刻生效」的，不用重启编辑器 —— 直接 PIE 看效果。\n"
        "  · 如果某个动作显得粘手（按了没反应），把它那一档的 blend_in 再调小 0.02。\n"
        "  · 如果某个收招显得抽一下，把它那一档的 blend_out 再调大 0.04。\n"
        "  · 想让某一段彻底硬切（例如受击必须瞬间），把该蒙太奇的 blend_in 手工设成 0。"
    )
    unreal.log_warning("==============================================")


main()
