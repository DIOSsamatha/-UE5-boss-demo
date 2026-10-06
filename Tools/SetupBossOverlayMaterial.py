# -*- coding: utf-8 -*-
"""
只重建「BOSS 状态光」材质：M_BossFlashOverlay + 三个实例（起手红 / 失衡绿 / 格挡蓝）。

为什么单独一个脚本：
    Tools/SetupBossVFX.py 会连带重建贴图和 Sprite 材质，而那些 .uasset
    在【编辑器开着的时候是被锁住的】—— 上一次跑它就直接
    "Error Code 32 / Error saving T_VFX_Glow.uasset" 然后整个脚本崩在第 200 行，
    根本轮不到 build_flash_overlay_material → 材质压根没更新，
    而 C++ 那边又已经把 Intensity 降下来了 → 两者不一致，画面只会更暗不会更贴合。

    这个脚本只碰状态光材质（通常没被打开、不被锁），可以在编辑器开着时跑。
    改完只需要在编辑器里【右键该材质 → 重新编译 / 或直接开始 PIE】让编辑器重载。

跑法（不需要关编辑器，~40 秒）：
    "K:/UE_5.7/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" "K:/selfgame/demo/demo.uproject" \
        -run=pythonscript -script="K:/selfgame/demo/Tools/SetupBossOverlayMaterial.py" \
        -unattended -NoLiveCoding -NoSound -NullRHI
"""
import unreal

import importlib.util
import os

_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
_SETUP_PATH = os.path.join(_SCRIPT_DIR, "SetupBossVFX.py")

# 复用 SetupBossVFX.py 里那一套节点搭建 / 保存 / 实例化的工具函数，
# 避免两份实现漂移。importlib 是因为 Tools 目录不是 Python 包。
_spec = importlib.util.spec_from_file_location("_setup_boss_vfx", _SETUP_PATH)
_mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_mod)

MEL = _mod.MEL
EAL = _mod.EAL


def main():
    parent = _mod.build_flash_overlay_material()

    # 三个实例：起手红 / 失衡绿 / 格挡冷蓝
    # Intensity 全部给 1.0 —— C++ 每帧会用 MID 覆写，
    # 这里的值只决定"万一 C++ 没写"时的兜底亮度（低一点更安全）。
    red = _mod.make_flash_instance("MI_BossFlash", parent, unreal.LinearColor(1.0, 0.10, 0.06, 1.0))
    green = _mod.make_flash_instance("MI_BossUnstable", parent, unreal.LinearColor(0.10, 1.0, 0.30, 1.0))
    blue = _mod.make_flash_instance("MI_BossGuard", parent, unreal.LinearColor(0.22, 0.55, 1.0, 1.0))

    for mi, label in ((red, "起手红"), (green, "失衡绿"), (blue, "格挡蓝")):
        if mi:
            MEL.set_material_instance_scalar_parameter_value(mi, "Intensity", 1.0)
        _mod.log("实例 %s（%s）就绪" % (label, mi.get_name() if mi else "缺失"))


main()
