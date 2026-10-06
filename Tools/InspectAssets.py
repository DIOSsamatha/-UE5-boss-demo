# -*- coding: utf-8 -*-
"""
InspectAssets.py —— 不开编辑器，直接从 .uasset 二进制里核对蓝图配置

为什么需要它：
    UE 里"我明明配了但没生效"时，光看编辑器界面判断不出来。.uasset 是序列化后的
    二进制，父类、引用的资产、属性名、输入键位名都以字符串形式躺在文件里，
    直接扫字符串就能比对"资产里到底存了什么"。

用法：
    python Tools/InspectAssets.py                  # 扫整个 Content
    python Tools/InspectAssets.py Content/Game     # 只扫某个子目录
    python Tools/InspectAssets.py --all            # 额外列出每个资产的父类
"""

import os
import re
import struct
import sys

PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONTENT = os.path.join(PROJECT, "Content")


def has_fname(data, name):
    """精确判断资产里是否真的存在某个 FName。

    名字表里每一项按「int32 长度 + 字符 + \\0」序列化（长度含结尾的 \\0），
    所以 "W" 会存成 02 00 00 00 57 00。
    单字符键名如果用子串匹配（b"Q" in data）会被 "GA_..." 之类的字符串误命中，
    必须按这个格式精确匹配。
    """
    raw = name.encode("ascii", "ignore") + b"\x00"
    return struct.pack("<i", len(raw)) + raw in data


def contains(data, name):
    """普通资产名（IA_/BP_/AM_ 等）用子串匹配即可"""
    return name.encode("ascii", "ignore") in data


# ---------------------------------------------------------------- 期望清单
# 资产名 -> [(说明, 必须出现的字符串), ...]；出现即 OK，没出现会标 MISS
EXPECT = {
    "BP_FightGameMode": [
        ("默认Pawn类 = BP_FightPlayer  ★最关键，缺它=看不见角色", "BP_FightPlayer"),
        ("HUDWidgetClass = WBP_CombatHUD", "WBP_CombatHUD"),
        ("BossBarWidgetClass = WBP_BossBar", "WBP_BossBar"),
    ],
    "BP_FightPlayer": [
        ("网格 = SK_Mannequin", "SK_Mannequin"),
        ("动画 = ABP_FightPlayer", "ABP_FightPlayer"),
        ("输入 = IMC_FightPlayer", "IMC_FightPlayer"),
        # ↓ 这 8 个槽位是"按键完全没反应"的头号原因：空着就不会绑任何按键
        ("InputAction Move", "IA_Move"),
        ("InputAction Look", "IA_Look"),
        ("InputAction Jump", "IA_Jump"),
        ("InputAction Run", "IA_Run"),
        ("InputAction NormalAttack", "IA_NormalAttack"),
        ("InputAction AttackSkill", "IA_AttackSkill"),
        ("InputAction Defense", "IA_Defense"),
        ("InputAction Flash", "IA_Flash"),
        ("技能 GA_NormalAttack", "GA_PlayerNormalAttack"),
        ("技能 GA_AttackSkill", "GA_PlayerAttackSkill"),
        ("技能 GA_Defense", "GA_PlayerDefense"),
        ("技能 GA_Parry", "GA_PlayerParry"),
        ("技能 GA_Dodge", "GA_PlayerDodge"),
    ],
    "BP_Boss": [
        ("网格 = SK_Mannequin", "SK_Mannequin"),
        ("动画 = ABP_FightPlayer", "ABP_FightPlayer"),
        ("BossAttackAbility = GA_BossAttack", "GA_BossAttack"),
        ("阶段数据 Phases（Content/Data 里的 DA_*）", "DA_"),
    ],
    "BP_PlayerController": [("父类 = FightPlayerController", "FightPlayerController")],
    "WBP_CombatHUD": [("父类 = CombatHUDWidget", "CombatHUDWidget")],
    "WBP_BossBar": [("父类 = BossHealthBarWidget", "BossHealthBarWidget")],
    "IMC_FightPlayer": [
        ("IA 全部 8 个", "IA_NormalAttack"),
        ("IA_Flash（闪避）", "IA_Flash"),
        ("IA_Defense（格挡）", "IA_Defense"),
        ("IA_Run（奔跑）", "IA_Run"),
        ("鼠标左键", "LeftMouseButton"),
        ("鼠标右键", "RightMouseButton"),
        ("空格", "SpaceBar"),
        ("左Shift", "LeftShift"),
        ("鼠标 2D（视角）", "Mouse2D"),
        # 单字母键必须精确匹配，否则会被 "GA_..." 里的 G/A 之类误命中
        ("W 键", "W"),
        ("A 键", "A"),
        ("S 键", "S"),
        ("D 键", "D"),
    ],
    "GA_NormalAttack": [("连招 Montage（AM_*）", "AM_")],
    "GA_AttackSkill": [("技能 Montage（AM_*）", "AM_")],
    "GA_Dodge": [("闪避 Montage（AM_*）", "AM_")],
}

# 输入映射里可能出现的键名（单字母键也算）
KEY_NAMES = [
    "LeftMouseButton", "RightMouseButton", "MiddleMouseButton", "Mouse2D", "MouseWheelAxis",
    "SpaceBar", "LeftShift", "RightShift", "LeftControl", "LeftAlt", "LeftCommand",
    "Q", "E", "R", "F", "C", "V", "Tab", "Escape", "W", "A", "S", "D",
    "Gamepad_LeftX", "Gamepad_LeftY", "Gamepad_RightX", "Gamepad_RightY",
]

TRIGGERS = ["InputTriggerPressed", "InputTriggerReleased", "InputTriggerDown",
            "InputTriggerHold", "InputTriggerHoldAndRelease", "InputTriggerTap",
            "InputTriggerPulse", "InputTriggerChordAction"]
MODIFIERS = ["InputModifierNegate", "InputModifierSwizzleAxis", "InputModifierDeadZone",
             "InputModifierScalar", "InputModifierScaleByDeltaTime"]

SCRIPT_RE = re.compile(rb"/Script/([A-Za-z0-9_]+)\.([A-Za-z0-9_]+)")


def read(path):
    with open(path, "rb") as f:
        return f.read()


def native_classes(data):
    """资产引用的原生类（含父类）"""
    out = []
    for mod, cls in SCRIPT_RE.findall(data):
        name = "%s.%s" % (mod.decode(), cls.decode())
        if name not in out:
            out.append(name)
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    show_all = "--all" in sys.argv
    root = os.path.join(PROJECT, args[0]) if args else CONTENT
    if not os.path.isdir(root):
        print("目录不存在:", root)
        return 1

    assets = []
    for dp, _, ns in os.walk(root):
        for n in ns:
            if n.endswith(".uasset"):
                assets.append(os.path.join(dp, n))
    assets.sort()
    if not assets:
        print("没找到 .uasset:", root)
        return 1

    problems = 0
    for path in assets:
        stem = os.path.splitext(os.path.basename(path))[0]
        rel = os.path.relpath(path, PROJECT)
        data = read(path)
        lines = []

        # 1) 父类候选
        natives = native_classes(data)
        own = [n for n in natives if n.lower().endswith("_" + stem.lower()) or stem.lower() in n.lower()]
        parent = [n for n in natives if n not in own]
        if parent:
            lines.append("    父类: " + ", ".join(parent[:3]))
        elif show_all and natives:
            lines.append("    原生类: " + ", ".join(natives[:3]))

        # 2) 期望项
        for label, needle in EXPECT.get(stem, []):
            # 短名字（单字母键名）必须精确匹配 FName，长名字子串匹配就够
            ok = has_fname(data, needle) if len(needle) <= 3 else contains(data, needle)
            lines.append("    %s %s" % ("[OK]  " if ok else "[缺! ]", label))
            if not ok:
                problems += 1

        # 3) IMC 专属：键 / 触发器 / 修饰器
        if stem.startswith("IMC_"):
            keys = [k for k in KEY_NAMES if has_fname(data, k)]
            trg = [t for t in TRIGGERS if has_fname(data, t)]
            mod = [m for m in MODIFIERS if has_fname(data, m)]
            n_actions = sum(1 for ia in ("IA_Move", "IA_Look", "IA_Jump", "IA_Run",
                                         "IA_NormalAttack", "IA_AttackSkill", "IA_Defense", "IA_Flash")
                            if contains(data, ia))
            lines.append("    键位(%d): %s" % (len(keys), ", ".join(keys) if keys else "（无）"))
            lines.append("    触发器: " + (", ".join(t.replace("InputTrigger", "") for t in trg) if trg else "（无，全部走默认 Down）"))
            lines.append("    修饰器: " + (", ".join(m.replace("InputModifier", "") for m in mod) if mod else "（无）"))
            # Move 需要 W/A/S/D 四个键 + 其余每个 Action 至少一个键
            if keys and len(keys) < n_actions + 3:
                lines.append("    [注意] 只找到 %d 个键，但引用了 %d 个 Action"
                             "（Move 要占 4 个键）→ 至少有 Action 没绑键" % (len(keys), n_actions))
            if not any(t == "InputTriggerDown" for t in trg):
                lines.append("    [注意] 没有 Down 触发器 → 需要\"按住生效\"的 Action（Defense/Run）"
                             "若用了 Pressed 会一按就结束、用了 Hold 要等延迟")
            # 数字键的原始值只写在 X 轴（引擎 PlayerInput.cpp：RawValueAccumulator.X = AmountDepressed），
            # 所以 Move 的 W/S 必须靠 Swizzle 把值搬到 Y 轴，A/S 必须靠 Negate 取反向。
            # 缺了它们 → 四个键全都输出 (1,0) → W/A/S/D 全在"向右走"。
            if "InputModifierSwizzleAxis" not in mod:
                lines.append("    [注意] 没有 SwizzleAxis 修饰器 → W/S 输出恒为 (1,0)，"
                             "和 A/D 一模一样（四个键都在往右走）。"
                             "修法：W 加 Swizzle(YXZ)；S 加 Swizzle(YXZ)+Negate")
            if "InputModifierNegate" not in mod:
                lines.append("    [注意] 没有 Negate 修饰器 → A 出不了 -X、S 出不了 -Y，"
                             "只能往正方向走。修法：A 加 Negate")
            if "InputModifierScalar" not in mod:
                lines.append("    [注意] 没有 Scalar 修饰器 → IA_Look 把鼠标原始像素增量"
                             "直接喂给相机，视角会飞快。修法：给 IA_Look 加 Scalar，X/Y≈0.15")

        # 4) 关卡
        print("=== " + rel)
        for l in lines:
            print(l)

    # 5) 关卡单独列
    maps = []
    for dp, _, ns in os.walk(CONTENT):
        for n in ns:
            if n.endswith(".umap"):
                maps.append(os.path.join(dp, n))
    if maps:
        print("\n=== 关卡 (.umap)")
        for m in sorted(maps):
            data = read(m)
            marks = [s for s in ["BP_Boss", "BP_FightGameMode", "NavMeshBoundsVolume",
                                 "PlayerStart", "Landscape", "BP_FightPlayer"]
                     if s.encode() in data]
            print("    %s  ->  %s" % (os.path.relpath(m, PROJECT),
                                      ", ".join(marks) if marks else "（空）"))

    print("\n共 %d 个资产，%d 处缺失。" % (len(assets), problems))
    return 0


if __name__ == "__main__":
    sys.exit(main())
