# -*- coding: utf-8 -*-
"""
在 UE 编辑器 Output Log(Python) 里运行：
    exec(open('K:/selfgame/demo/Tools/SetupInputRemap.py').read())

作用（当前键位方案）：
  LeftShift        -> IA_Run        （C++ 分流：短按闪避 / 长按奔跑）
  RightMouseButton -> IA_Defense    （弹刀 Parry）

★★★ 关键修复（之前改键一直不生效的真正原因）★★★
  UE5.7 把 UInputMappingContext 的映射数组从 `Mappings` 换成了
  `DefaultKeyMappings.Mappings`：
      - `Mappings`      已标 UE_DEPRECATED，运行时【完全不再读取】
      - `PostLoad` 里的旧数据迁移只在“资产版本号比 5.7 旧”时才跑一次，
        资产被新版保存过之后就不再迁移 → 往 `Mappings` 里写等于扔进垃圾桶
  所以本脚本改用引擎的官方接口：
      imc.unmap_all_keys_from_action(action)   # 底层操作 DefaultKeyMappings
      imc.map_key(action, key)                 # 同上
  若这两个接口在当前版本没暴露给 Python，则退化为直接修改
  `default_key_mappings` 结构体（同样是 DefaultKeyMappings）。

注意：UE Python 没有 InputActionFactory；`unreal.Key()` 不接收构造参数，
      必须空构造后再 set_editor_property("key_name", ...)。
"""
import unreal

INPUT_DIR = "/Game/FightPlayer/Input"
IMC_PATH = INPUT_DIR + "/IMC_FightPlayer"
IA_RUN_PATH = INPUT_DIR + "/IA_Run"
IA_DEFENSE_PATH = INPUT_DIR + "/IA_Defense"
IA_FLASH_PATH = INPUT_DIR + "/IA_Flash"


def log(msg):
    unreal.log("[SetupInput] " + str(msg))


def load_asset(path):
    asset = unreal.load_asset(path)
    if asset is None:
        log("ERROR: 找不到资产 " + path)
    return asset


def make_key(key_name):
    """构造 FKey。unreal.Key() 不能带构造参数，只能空构造后设 key_name。"""
    k = unreal.Key()
    k.set_editor_property("key_name", key_name)
    return k


def mapping_key_name(mapping):
    try:
        k = mapping.get_editor_property("key")
        return str(k.get_editor_property("key_name"))
    except Exception:
        return "?"


def mapping_action_name(mapping):
    try:
        a = mapping.get_editor_property("action")
        return a.get_name() if a else "None"
    except Exception:
        return "None"


def get_default_mappings(imc):
    """读 DefaultKeyMappings.Mappings —— 这才是运行时真正使用的映射数组。"""
    try:
        data = imc.get_editor_property("default_key_mappings")
        return list(data.get_editor_property("mappings"))
    except Exception as exc:
        log("读取 DefaultKeyMappings 失败：%s" % exc)
        return []


def dump(imc, title):
    log(title)
    mappings = get_default_mappings(imc)
    if not mappings:
        log("  （空）")
    for m in mappings:
        log("  %-22s -> %s" % (mapping_key_name(m), mapping_action_name(m)))


def action_path_name(action):
    try:
        return action.get_path_name()
    except Exception:
        return ""


# ---------------------------------------------------------------- 载入资产
imc = load_asset(IMC_PATH)
ia_run = load_asset(IA_RUN_PATH)
ia_defense = load_asset(IA_DEFENSE_PATH)
ia_flash = load_asset(IA_FLASH_PATH)   # 早期版本 LeftShift 绑的就是它，要清掉

if imc is None or ia_run is None or ia_defense is None:
    log("缺少必要资产，脚本中止。")
    raise SystemExit(1)

dump(imc, "当前映射（DefaultKeyMappings，修改前）:")

# ---------------------------------------------------------------- 执行改键
# 用官方接口（内部直接操作 DefaultKeyMappings，并会触发映射表重建）
use_official_api = hasattr(imc, "map_key") and hasattr(imc, "unmap_all_keys_from_action")
log("使用官方接口 map_key/unmap_all_keys_from_action：%s" % use_official_api)

if use_official_api:
    # 先清掉旧绑定：IA_Flash（LeftShift 的历史归属）、IA_Run、IA_Defense
    for action, label in ((ia_flash, "IA_Flash"), (ia_run, "IA_Run"), (ia_defense, "IA_Defense")):
        if action is None:
            continue
        try:
            imc.unmap_all_keys_from_action(action)
            log("  已清除 %s 的旧映射" % label)
        except Exception as exc:
            log("  清除 %s 旧映射失败：%s" % (label, exc))

    try:
        imc.map_key(ia_run, make_key("LeftShift"))
        log("  已绑定 LeftShift -> IA_Run")
    except Exception as exc:
        log("  绑定 LeftShift -> IA_Run 失败：%s" % exc)

    try:
        imc.map_key(ia_defense, make_key("RightMouseButton"))
        log("  已绑定 RightMouseButton -> IA_Defense")
    except Exception as exc:
        log("  绑定 RightMouseButton -> IA_Defense 失败：%s" % exc)
else:
    # 退化路径：直接改 DefaultKeyMappings 结构体
    log("退化路径：直接修改 default_key_mappings 结构体")
    data = imc.get_editor_property("default_key_mappings")
    old = list(data.get_editor_property("mappings"))

    removed_paths = {action_path_name(a) for a in (ia_flash, ia_run, ia_defense) if a}
    kept = []
    for m in old:
        a = m.get_editor_property("action")
        if a and action_path_name(a) in removed_paths:
            log("  移除: %s -> %s" % (mapping_key_name(m), mapping_action_name(m)))
            continue
        kept.append(m)

    def build(action, key_name):
        m = unreal.EnhancedActionKeyMapping()
        m.set_editor_property("action", action)
        m.set_editor_property("key", make_key(key_name))
        return m

    kept.append(build(ia_run, "LeftShift"))
    kept.append(build(ia_defense, "RightMouseButton"))

    data.set_editor_property("mappings", kept)
    imc.set_editor_property("default_key_mappings", data)

# ---------------------------------------------------------------- 保存 + 核对
unreal.EditorAssetLibrary.save_asset(IMC_PATH)
log("IMC 已保存。")

dump(imc, "最终映射（DefaultKeyMappings，修改后）:")

# 核对关键项
final = get_default_mappings(imc)
shift_target = None
rmb_target = None
for m in final:
    kn = mapping_key_name(m)
    an = mapping_action_name(m)
    if kn == "LeftShift":
        shift_target = an
    elif kn == "RightMouseButton":
        rmb_target = an

log("左Shift        -> %s" % shift_target)
log("鼠标右键       -> %s" % rmb_target)

if shift_target == "IA_Run" and rmb_target == "IA_Defense":
    log("OK：改键成功。请 Ctrl+Shift+S 全部保存，然后 PIE 验收。")
else:
    log("⚠ 期望 左Shift->IA_Run、右键->IA_Defense，实际如上表。"
        "若不符请把本段日志发出来。")

log("PIE 验收：")
log("  左Shift 短按  = 闪避（无方向键默认向前，WASD 决定方向）")
log("  左Shift 长按  = 奔跑")
log("  鼠标右键      = 弹刀（抬剑招架 → 弹到则反击）")
