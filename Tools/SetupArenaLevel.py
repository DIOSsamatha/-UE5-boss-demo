# -*- coding: utf-8 -*-
"""
用 Fab 的竞技场资源搭一张新的战斗关卡：/Game/Fab/FightArena

背景（为什么需要脚本搭，而不是手工拖）：
    用户把 Fab 的 "Boss Fight Arena (PVP Arena)" 下载进了 Content/Fab/，
    但那套资源里【只有 StaticMesh + 材质 + 贴图，没有关卡】，所以游戏里还是空旷的
    FightGround（一个 89km 的 Landscape + 天空球）。
    要把它变成真正的竞技场，必须新建一张关卡把网格摆进去。

这张关卡会放：
    · 竞技场 StaticMeshActor（按包围盒自动缩放 + 居中 + 落地）
    · 兜底地板（大 Plane，防止竞技场碰撞缺失导致掉出世界）
    · DirectionalLight / SkyLight / SkyAtmosphere / ExponentialHeightFog
    · PlayerStart（在竞技场中心，让玩家从中心开局被 BOSS 追）
    · 一个 unbound PostProcessVolume（Boss 的红光/格挡光靠 Bloom 才有表现力）

不放 BOSS：已确认 BP_Boss 由 BP_FightGameMode 运行时 SpawnActor 生成
    （证据：BP_Boss 只被 BP_FightGameMode.uasset 引用，FightGround.umap 里没有它）。

跑法（不需要开编辑器）：
    "K:/UE_5.7/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" "K:/selfgame/demo/demo.uproject" \
        -run=pythonscript -script="K:/selfgame/demo/Tools/SetupArenaLevel.py" \
        -unattended -NoLiveCoding -NoSound -NullRHI
"""
import unreal

ARENA_MESH = ("/Game/Fab/Boss_Fight_Arena_PVP_Arena/boss_fight_arenapvp_arena/"
              "StaticMeshes/boss_fight_arenapvp_arena")
LEVEL_DIR = "/Game/Fab"
LEVEL_NAME = "FightArena"
LEVEL_PATH = "%s/%s" % (LEVEL_DIR, LEVEL_NAME)
OUT = r"K:/selfgame/demo/Saved/setup_arena_level.txt"

# 竞技场原始包围盒 567m × 1440m × 417m，对 1v1 BOSS 战来说大得离谱
# （玩家移速 6m/s，跑个对角要 4 分钟）。缩到 0.1 → 57m × 144m，正好是一场 BOSS 战的尺度。
ARENA_SCALE = 0.1

_lines = []


def emit(s):
    _lines.append(str(s))
    unreal.log(s)


def save():
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(_lines))


def step(label, fn):
    """每一步都容错。

    UE5.7 的 EditorLevelLibrary API 变动不小（上一版就因为
    get_current_level_name 不存在而 AttributeError，整张关卡半途而废），
    所以这里统一包一层：单步失败只记日志，不影响后续。
    """
    try:
        return fn()
    except Exception as exc:
        emit("[arena] ⚠ 步骤失败「%s」：%s: %s" % (label, type(exc).__name__, exc))
        return None


def spawn(cls, loc, rot=(0.0, 0.0, 0.0), label=None):
    a = unreal.EditorLevelLibrary.spawn_actor_from_class(
        cls, unreal.Vector(*loc), unreal.Rotator(*rot))
    if a and label:
        a.set_actor_label(label)
    return a


def set_prop(obj, prop, value, label=""):
    try:
        obj.set_editor_property(prop, value)
        return True
    except Exception as exc:
        emit("[arena]   ⚠ 属性 %s%s 设置失败：%s" % (label, prop, exc))
        return False


def main():
    emit("[arena] ===== 建立竞技场关卡 %s =====" % LEVEL_PATH)

    # ---------- 0. 清理同名旧版（脚本要能重复跑）----------
    if step("检查旧关卡", lambda: unreal.EditorAssetLibrary.does_asset_exist(LEVEL_PATH)):
        step("删除旧关卡", lambda: unreal.EditorAssetLibrary.delete_asset(LEVEL_PATH))

    step("新建关卡", lambda: unreal.EditorLevelLibrary.new_level(LEVEL_PATH))
    step("读世界名", lambda: emit("[arena] 当前世界 = %s" %
         unreal.EditorLevelLibrary.get_editor_world().get_name()))

    # ---------- 1. 竞技场网格 ----------
    mesh = step("加载竞技场网格", lambda: unreal.load_asset(ARENA_MESH))
    if mesh is None:
        emit("[arena] ✗ 加载竞技场网格失败：%s（关卡已建但没场地）" % ARENA_MESH)
        step("保存关卡", lambda: unreal.EditorLevelLibrary.save_current_level())
        save()
        return

    b = step("取包围盒", lambda: mesh.get_bounds())
    if b:
        ox, oy, oz = b.origin.x, b.origin.y, b.origin.z
        ex, ey, ez = b.box_extent.x, b.box_extent.y, b.box_extent.z
        emit("[arena] 网格包围盒 origin=(%.0f,%.0f,%.0f) extent=(%.0f,%.0f,%.0f)" % (
            ox, oy, oz, ex, ey, ez))

        sma = step("生成 StaticMeshActor", lambda: spawn(
            unreal.StaticMeshActor, (0, 0, 0), (0, 0, 0), "Arena_StaticMesh"))
        if sma is not None:
            step("挂网格", lambda: sma.static_mesh_component.set_static_mesh(mesh))
            step("设为静态", lambda: sma.static_mesh_component.set_mobility(
                unreal.ComponentMobility.STATIC))

            # 关键：网格原点 Y 偏了 450m，直接摆会让竞技场整体跑到地图外。
            # 让【缩放后的包围盒中心】落在世界原点、且【最低点】贴到 Z=0。
            loc = unreal.Vector(
                -ox * ARENA_SCALE,                        # X 居中
                -oy * ARENA_SCALE,                        # Y 居中（原始偏 450m）
                -(oz * ARENA_SCALE) + (ez * ARENA_SCALE),  # 最低点落 Z=0 = 地面
            )
            # ⚠ UE5.7 的 set_actor_location 绑定签名是 (new_location, sweep, teleport)，
            #   其中【sweep 是必填的】：
            #     传 1 个 → "required argument 'sweep' (pos 2) not found"
            #     传 4 个 → "takes at most 3 arguments (4 given)"
            #   这两次都让竞技场停在 (0,0,0) 没居中，玩家会出生在场地外，所以必须传够 3 个。
            step("摆位置", lambda: sma.set_actor_location(loc, False, False))
            step("设缩放", lambda: sma.set_actor_scale3d(
                unreal.Vector(ARENA_SCALE, ARENA_SCALE, ARENA_SCALE)))
            emit("[arena] 竞技场摆放完成：scale=%.3f 位置=(%.0f, %.0f, %.0f)" % (
                ARENA_SCALE, loc.x, loc.y, loc.z))
            emit("[arena] 缩放后地面尺寸 ≈ %.1f m × %.1f m" % (
                2 * ex * ARENA_SCALE / 100.0, 2 * ey * ARENA_SCALE / 100.0))

    # ---------- 2. 兜底地板（必须！）----------
    #
    # ⚠⚠ 实测教训：Fab 这份竞技场网格【没有碰撞】。
    #   第一版关卡跑起来后 BOSS 脚骨 Z 从 -329 一路掉到 -3274（无头日志里逐帧可见），
    #   也就是它穿过地面无限下坠 —— 玩家会跟着一起掉，整局不可玩。
    #   所以必须有一块【覆盖全场】的兜底地板。
    #   放在 Z = -30（竞技场地面是 Z=0）：万一 Fab 网格日后带上了碰撞，角色仍然站在
    #   Z=0 的正式地面上；没有碰撞时也只掉 30cm，斜俯视几乎看不出。
    def _floor():
        plane = unreal.load_asset("/Engine/BasicShapes/Plane.Plane")
        if not plane:
            emit("[arena] ✗ 兜底地板跳过（Plane 加载失败）→ 角色会掉出世界")
            return
        f = spawn(unreal.StaticMeshActor, (0, 0, -30), (0, 0, 0), "Arena_FallbackFloor")
        if not f:
            emit("[arena] ✗ 兜底地板生成失败 → 角色会掉出世界")
            return
        c = f.static_mesh_component
        c.set_static_mesh(plane)
        c.set_mobility(unreal.ComponentMobility.STATIC)
        # Plane 是 100cm 见方；竞技场 57m × 144m，这里给 700m × 1500m 的富余
        c.set_world_scale3d(unreal.Vector(700.0, 1500.0, 1.0))
        emit("[arena] 兜底地板：700m × 1500m @ Z=-30（Fab 网格无碰撞，靠它托住角色）")
    step("兜底地板", _floor)

    # 出生点抬到 Z=200：PlayerStart 的 IsSpawnAllowed 会在玩家胶囊尺寸下做
    # EncroachingBlockingGeometry 检查，放在地面上极易被判为"被占据"→
    # ChoosePlayerStart 返回 null → GameMode 退回 (0,0,0) 生成 → 那里也被占 → 整局没有玩家。
    # 抬高 200cm 后玩家自然落到兜底地板（Z=-30），不会悬空。
    def _pick_start():
        ok = spawn(unreal.PlayerStart, (0, 0, 200), (0, 90, 0), "PlayerStart")
        emit("[arena] PlayerStart @ (0,0,200)：%s" % ("已放" if ok else "失败"))
    step("PlayerStart", _pick_start)

    # ---------- 3.5 BOSS 出生点 ----------
    #
    # ⚠⚠ BOSS 是【关卡里放置】的，不是 GameMode 生成的 —— 新关卡不放它就一只都没有。
    #   证据：在 `Content/__ExternalActors__/FightGround/**` 里找到唯一引用 BP_Boss 的
    #   外部 Actor 文件 `B/68/5UTGC8022HPKBIBNF4WP80.uasset`，
    #   其内部路径写得很清楚：
    #     /Game/FightGround.FightGround:PersistentLevel.BP_Boss_C_UAID_50EBF6938B27D10303_1970846783
    #   （UAID 前缀 = 关卡序列化放置的实例；运行时 SpawnActor 生成的名字是 BP_Boss_C_0）
    def _boss():
        bp = step("加载 BP_Boss", lambda: unreal.load_asset("/Game/Boss/System/BP_Boss"))
        cls = None
        if bp is not None:
            cls = step("取生成类", lambda: bp.generated_class())
        if cls is None:
            emit("[arena] ✗ 拿不到 BP_Boss 的生成类 → 关卡里不会有 BOSS")
            return
        # 放在玩家对面：玩家在场地中心，BOSS 在 +Y 侧 25m 处，面向玩家（-Y 方向 → yaw 180）
        ok = spawn(cls, (0.0, 2500.0, 200.0), (0.0, 180.0, 0.0), "BP_Boss")
        emit("[arena] BOSS 出生点 @ (0, 2500, 200) 朝向玩家：%s" % ("已放" if ok else "失败"))
    step("BOSS 出生点", _boss)

    # ---------- 3. 光照 / 大气 ----------
    def _sun():
        s = spawn(unreal.DirectionalLight, (0, 0, 3000), (-52, -38, 0), "Sun")
        if not s:
            return
        lc = s.light_component
        # UE5 里 DirectionalLight 默认 3.14 lux，室外偏暗；斜俯视 BOSS 战给 8
        set_prop(lc, "intensity", 8.0, "Sun.")
        set_prop(lc, "temperature", 5600.0, "Sun.")
        set_prop(lc, "use_temperature", True, "Sun.")
        set_prop(lc, "cast_shadows", True, "Sun.")
        emit("[arena] DirectionalLight：强度 8.0 lux / 5600K / 投影开 / 旋转(-52,-38)")
    step("DirectionalLight", _sun)

    def _skylight():
        s = spawn(unreal.SkyLight, (0, 0, 0), (0, 0, 0), "SkyLight")
        if not s:
            return
        kc = s.light_component
        set_prop(kc, "intensity", 1.4, "SkyLight.")
        # 实时捕获：关卡里没有 SkyCapture，静态捕获会得到全黑的环境光
        set_prop(kc, "real_time_capture", True, "SkyLight.")
        emit("[arena] SkyLight：强度 1.4，实时捕获（无 SkyCapture 时静态捕获会全黑）")
    step("SkyLight", _skylight)

    step("SkyAtmosphere", lambda: emit("[arena] SkyAtmosphere：%s" % (
        "已放" if spawn(unreal.SkyAtmosphere, (0, 0, 0), (0, 0, 0), "SkyAtmosphere") else "失败")))

    def _fog():
        f = spawn(unreal.ExponentialHeightFog, (0, 0, 0), (0, 0, 0), "ExponentialHeightFog")
        if not f:
            return
        # ⚠ FogDensity / FogHeightFalloff 挂在【组件】上，不是 Actor 上。
        #   设错地方会报 "Failed to find property 'fog_density' for attribute on
        #   'ExponentialHeightFog'"，雾就会保持默认（几乎不可见）。
        comp = step("取 Fog 组件", lambda: f.get_editor_property("component"))
        if comp is None:
            return
        ok1 = set_prop(comp, "fog_density", 0.012, "FogComp.")
        set_prop(comp, "fog_height_falloff", 0.18, "FogComp.")
        set_prop(comp, "fog_height_offset", 300.0, "FogComp.")
        emit("[arena] ExponentialHeightFog：%s" % ("密度 0.012" if ok1 else "参数未生效"))
    step("ExponentialHeightFog", _fog)

    # ---------- 4. 后期（Boss 的红光 / 格挡光靠 Bloom 才有表现力）----------
    def _ppv():
        v = spawn(unreal.PostProcessVolume, (0, 0, 0), (0, 0, 0), "PPV_Unbound")
        if not v:
            return
        set_prop(v, "unbound", True, "PPV.")
        settings = step("读 PPV settings", lambda: v.get_editor_property("settings"))
        if settings is None:
            return
        # ⚠ UE5 的 PostProcessSettings 里，Bloom / AutoExposure 各自的子属性名
        #   和旧版文档不一样（bloom→可能是 bloom_* 系列），所以这里逐个试。
        #   失败不致命：默认后期也能跑，只是 BOSS 的红光/格挡光少了 Bloom 辉光。
        for group, prop, val in (
            ("bloom", "intensity", 0.9),
            ("bloom", "threshold", 0.85),
            ("auto_exposure", "auto_exposure_method", 0),
            ("auto_exposure", "exposure_compensation", 0.0),
        ):
            g = step("读 %s 组" % group, lambda g=group: settings.get_editor_property(g))
            if g is not None:
                set_prop(g, prop, val, "%s." % group)
        emit("[arena] PostProcessVolume：unbound（Bloom/曝光参数如上报错则保持默认）")
    step("PostProcessVolume", _ppv)

    # ---------- 6. 保存 + 自检 ----------
    step("保存关卡", lambda: emit("[arena] 保存 = %s" %
         unreal.EditorLevelLibrary.save_current_level()))

    def _verify():
        actors = unreal.EditorLevelLibrary.get_all_level_actors()
        emit("[arena] ===== 成关卡自检：%d 个 Actor =====" % len(actors))
        for a in actors:
            try:
                loc = a.get_actor_location()
                emit("[arena]   %-34s @ (%8.0f, %8.0f, %8.0f)  %s" % (
                    a.get_class().get_name(), loc.x, loc.y, loc.z, a.get_actor_label()))
            except Exception:
                pass
    step("自检", _verify)

    save()


main()
