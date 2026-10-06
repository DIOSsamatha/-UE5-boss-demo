# 一次性诊断 + 修复玩家战斗技能接线（攻击 / 闪避）。
# 解决：攻击、闪避"放不出来 / 无动画"。
# 根因：BP_FightPlayer 的 DefaultAbilities 与显式技能槽若指向 C++ 基类
#       （GA_PlayerNormalAttack / GA_PlayerDodge），而蒙太奇只配在蓝图子类上，
#       运行时授予基类 -> 蒙太奇为 null -> 技能瞬间 EndAbility。
# 本脚本：① 用序列建好蒙太奇（若已存在则复用）② 填进蓝图子类 ③ 把玩家 BP 的
#       DefaultAbilities 与 5 个显式技能槽全部指到蓝图子类 ④ 打印最终真实状态。
# 运行：UE 编辑器 -> Output Log 右下角切 Python -> 输入框填本文件绝对路径 -> 回车。
# 无需重编 C++、无需重启。

import unreal

ASSET_LIB = unreal.EditorAssetLibrary
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()

PLAYER_PATH = "/Game/FightPlayer/System/BP_FightPlayer"
MONTAGE_DIR = "/Game/FightPlayer/Animation"

# 蓝图子类（持有已填的蒙太奇/Cooldown/Cost）
GA_PATHS = {
    "GA_NormalAttack": "/Game/FightPlayer/Abilities/GA_NormalAttack",
    "GA_AttackSkill":  "/Game/FightPlayer/Abilities/GA_AttackSkill",
    "GA_Defense":      "/Game/FightPlayer/Abilities/GA_Defense",
    "GA_Parry":        "/Game/FightPlayer/Abilities/GA_Parry",
    "GA_Dodge":        "/Game/FightPlayer/Abilities/GA_Dodge",
}

# 源动画序列（SwordAnimsetPro）
ATTACK_SEQ = "/Game/SwordAnimsetPro/Demo/AnimMontage/Attack_Combo_01_Anim"
DODGE_SEQS = {
    "R": "/Game/SwordAnimsetPro/Animations/Root-Motion/Dodge_R_Anim",
    "L": "/Game/SwordAnimsetPro/Animations/Root-Motion/Dodge_L_Anim",
    "B": "/Game/SwordAnimsetPro/Animations/Root-Motion/Dodge_B_Anim",
    "F": "/Game/SwordAnimsetPro/Animations/Root-Motion/Dodge_F_Anim",
}


def ensure_dir(path):
    if not ASSET_LIB.does_directory_exist(path):
        ASSET_LIB.make_directory(path)


def create_montage(seq_path, montage_name):
    """从 AnimSequence 创建一个 AnimMontage，已存在则直接加载复用。"""
    out_path = f"{MONTAGE_DIR}/{montage_name}"
    existing = ASSET_LIB.load_asset(out_path)
    if existing is not None:
        unreal.log(f"[复用] {montage_name}")
        return existing

    seq = ASSET_LIB.load_asset(seq_path)
    if seq is None:
        unreal.log_error(f"[SKIP] 找不到源序列 {seq_path}")
        return None

    skeleton = seq.get_editor_property("skeleton")
    factory = unreal.AnimMontageFactory()
    factory.set_editor_property("target_skeleton", skeleton)
    factory.set_editor_property("source_animation", seq)
    montage = TOOLS.create_asset(montage_name, MONTAGE_DIR, unreal.AnimMontage, factory)
    if montage is None:
        unreal.log_error(f"[FAIL] 创建蒙太奇失败 {montage_name}")
        return None
    ASSET_LIB.save_asset(out_path)
    unreal.log(f"[新建] {montage_name}")
    return montage


def load_ga_cdo(asset_path, asset_name):
    cls = ASSET_LIB.load_blueprint_class(asset_path)
    if cls is None:
        unreal.log_error(f"[SKIP] 加载生成类失败 {asset_path}")
        return None, None
    cdo = unreal.load_object(None, f"{asset_path}.Default__{asset_name}_C")
    if cdo is None:
        unreal.log_error(f"[SKIP] 找不到 CDO Default__{asset_name}_C")
        return cls, None
    return cls, cdo


def count_montages(cdo, prop):
    try:
        arr = cdo.get_editor_property(prop)
        return len(arr) if arr else 0
    except Exception:
        return -1


# ============================ 1. 准备蒙太奇 ============================
ensure_dir(MONTAGE_DIR)

attack_montage = create_montage(ATTACK_SEQ, "Attack_Combo_01_Montage")

dodge_montages = {}
for key, seq in DODGE_SEQS.items():
    m = create_montage(seq, f"Dodge_{key}_Montage")
    if m:
        dodge_montages[key] = m

# 方向顺序（与 UGA_PlayerDodge::PickDodgeMontage 严格对应）：
#   0=右(+X)  1=左(-X)  2=后(-Y)  3=前(+Y)
dodge_ordered = [dodge_montages.get("R"), dodge_montages.get("L"),
                 dodge_montages.get("B"), dodge_montages.get("F")]
if any(m is None for m in dodge_ordered):
    unreal.log_error("有闪避蒙太奇创建失败，已中止后续。请检查 Dodge_*_Anim 源序列路径。")
else:
    unreal.log_warning("4 个闪避蒙太奇就绪（顺序 右/左/后/前）。")

# ============================ 2. 填进蓝图子类 ============================
ga_classes = {}
for name, path in GA_PATHS.items():
    cls, cdo = load_ga_cdo(path, name)
    if cls is None:
        continue
    ga_classes[name] = cls
    if cdo is None:
        continue

    if name == "GA_NormalAttack" and attack_montage is not None:
        cdo.set_editor_property("combo_montages", [attack_montage])
        ASSET_LIB.save_asset(path)
        unreal.log(f"[OK] {name}.ComboMontages = [Attack_Combo_01_Montage]")
    elif name == "GA_Dodge":
        cdo.set_editor_property("directional_montages", dodge_ordered)
        cdo.set_editor_property("dodge_montage", dodge_montages.get("F"))  # 兜底单段
        ASSET_LIB.save_asset(path)
        unreal.log(f"[OK] {name}.DirectionalMontages = [R,L,B,F]（4 段）")

# ============================ 3. 接线玩家 BP ============================
need = ["GA_NormalAttack", "GA_AttackSkill", "GA_Defense", "GA_Parry", "GA_Dodge"]
if all(k in ga_classes for k in need):
    p_cdo = unreal.load_object(None, f"{PLAYER_PATH}.Default__BP_FightPlayer_C")
    if p_cdo is None:
        unreal.log_error("找不到 BP_FightPlayer 的 CDO，接线失败。")
    else:
        # 3a. DefaultAbilities 整体重指向 5 个蓝图子类
        p_cdo.set_editor_property("default_abilities", [ga_classes[k] for k in need])
        # 3b. 显式技能槽也指到蓝图子类（更稳，优先级高于扫描）
        p_cdo.set_editor_property("normal_attack_ability", ga_classes["GA_NormalAttack"])
        p_cdo.set_editor_property("attack_skill_ability",  ga_classes["GA_AttackSkill"])
        p_cdo.set_editor_property("defense_ability",       ga_classes["GA_Defense"])
        p_cdo.set_editor_property("parry_ability",         ga_classes["GA_Parry"])
        p_cdo.set_editor_property("flash_ability",         ga_classes["GA_Dodge"])
        ASSET_LIB.save_asset(PLAYER_PATH)
        unreal.log_warning("BP_FightPlayer 已接线：DefaultAbilities + 5 个显式技能槽 -> 蓝图子类。")
else:
    unreal.log_error("有 GA 蓝图未加载成功，未改动玩家 BP。")

# ============================ 4. 验证并打印真实状态 ============================
unreal.log_warning("========== 验证（读取刚写入的实际值）==========")
p_cdo = unreal.load_object(None, f"{PLAYER_PATH}.Default__BP_FightPlayer_C")
if p_cdo:
    da = p_cdo.get_editor_property("default_abilities")
    names = [c.get_name() if c else "None" for c in (da or [])]
    unreal.log_warning(f"DefaultAbilities({len(names)}): {names}")
    for slot in ["normal_attack_ability", "attack_skill_ability",
                 "defense_ability", "parry_ability", "flash_ability"]:
        v = p_cdo.get_editor_property(slot)
        unreal.log_warning(f"  {slot} = {v.get_name() if v else 'None'}")

for name, path in GA_PATHS.items():
    _, cdo = load_ga_cdo(path, name)
    if cdo is None:
        continue
    if name == "GA_NormalAttack":
        unreal.log_warning(f"{name}: ComboMontages={count_montages(cdo, 'combo_montages')}")
    elif name == "GA_Dodge":
        unreal.log_warning(f"{name}: DirectionalMontages={count_montages(cdo, 'directional_montages')}")

unreal.log_warning("FixCombatAbilityWiring 完成。直接 PIE：左键普攻、左Shift短按闪避。")
