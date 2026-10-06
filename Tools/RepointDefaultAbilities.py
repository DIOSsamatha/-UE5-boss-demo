# 修复：BP_FightPlayer.DefaultAbilities 当前引用的是 C++ 基类（GA_PlayerDodge 等），
# 而 Montage 被配在了蓝图子类（GA_Dodge 等）上。基类里 ComboMontages/DirectionalMontages
# 是空数组，导致运行时授予基类 -> 蒙太奇为 null -> 技能瞬间 EndAbility -> "放不出技能"。
# 本脚本把 DefaultAbilities 重指向 5 个蓝图子类（它们继承 C++ 标签、且持有已填的 Montage）。
# 运行方式：UE 编辑器 -> Output Log 右下角切 Python -> 输入框填绝对路径 -> 回车。
# 无需重编 C++、无需重启。

import unreal

ASSET_LIB = unreal.EditorAssetLibrary

PLAYER_PATH = "/Game/FightPlayer/System/BP_FightPlayer"

# 蓝图子类（持有已填的 Montage/Cooldown）
GA_BLUEPRINTS = {
    "GA_Dodge":        "/Game/FightPlayer/Abilities/GA_Dodge",
    "GA_NormalAttack": "/Game/FightPlayer/Abilities/GA_NormalAttack",
    "GA_AttackSkill":  "/Game/FightPlayer/Abilities/GA_AttackSkill",
    "GA_Defense":      "/Game/FightPlayer/Abilities/GA_Defense",
    "GA_Parry":        "/Game/FightPlayer/Abilities/GA_Parry",
}

# 1) 收集 5 个蓝图的生成类（generated_class）
ga_classes = []
for name, path in GA_BLUEPRINTS.items():
    ga_bp = ASSET_LIB.load_asset(path)
    if ga_bp is None:
        unreal.log_error(f"[SKIP] 找不到蓝图 {path}")
        continue
    # load_blueprint_class 直接返回蓝图生成类（比 generated_class 属性更稳，全版本可用）
    cls = ASSET_LIB.load_blueprint_class(path)
    if cls is None:
        unreal.log_error(f"[SKIP] {name} 加载生成类失败（先在编辑器里编译一次该蓝图再跑）")
        continue
    ga_classes.append(cls)
    unreal.log(f"[OK] {name} -> {cls.get_name()}")

if len(ga_classes) != len(GA_BLUEPRINTS):
    unreal.log_error("有蓝图加载失败，已中止，未改动 BP_FightPlayer")
else:
    # 2) 加载玩家蓝图 CDO（默认对象）
    cdo = unreal.load_object(None, PLAYER_PATH + ".Default__BP_FightPlayer_C")
    if cdo is None:
        unreal.log_error("找不到 BP_FightPlayer 的 CDO（Default__BP_FightPlayer_C）")
    else:
        # 3) 写回 DefaultAbilities（Python 属性名 = default_abilities）
        old = cdo.get_editor_property("default_abilities")
        unreal.log(f"[INFO] 修改前 DefaultAbilities 元素数 = {len(old) if old else 0}")
        cdo.set_editor_property("default_abilities", ga_classes)
        ASSET_LIB.save_asset(PLAYER_PATH)
        unreal.log_warning(
            f"BP_FightPlayer.DefaultAbilities 已重指向 {len(ga_classes)} 个蓝图子类"
            f"（GA_Dodge / GA_NormalAttack / GA_AttackSkill / GA_Defense / GA_Parry）。"
            f"它们持有已填的 Montage 与 Cooldown。")
        unreal.log_warning("保存成功。直接 PIE 测试：左键普攻、左Shift短按闪避。")
