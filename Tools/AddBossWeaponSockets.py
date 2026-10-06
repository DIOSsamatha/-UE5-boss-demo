# -*- coding: utf-8 -*-
"""
给 BOSS 的骨骼网格补上 WeaponStart / WeaponEnd 插槽（Paragon Khaimera）。

【为什么必须补】
DemoCharacterBase 的命中判定会在这两个插槽之间扫掠球体当作"刀锋"。
插槽缺失时会退化成"角色身前一整段(40~120cm)"的粗判线，表现就是
"看着砍中了不判、看着没碰到反而判"。当前日志：
    [Melee] BP_Boss_C_... 武器插槽解析：WeaponStart=缺失，WeaponEnd=缺失

【为什么用脚本而不是手工点】
走的是引擎公开接口（USkeletalMesh::AddSocket + SkeletalMeshEditorSubsystem::RenameSocket
+ USkeletalMeshSocket::SetSocketParent），全程在编辑器里执行：
  · 不需要重新编译 C++
  · 不需要重启编辑器
  · 不依赖手点 UI 的准确位置（骨骼名写死在下面，不会挂错骨头）

【运行方式】
编辑器 Output Log 右上角把模式切到 Python，输入下面这行回车：
    K:/selfgame/demo/Tools/AddBossWeaponSockets.py

【想调整命中范围】
改下面的 SOCKET_DEFS 即可 —— Khaimera 是徒手爪击，默认取
"手心(hand_r) → 中指尖(middle_03_r)"，这一段正好就是一条爪刃。
想让判定更长/更靠前，就给 WeaponEnd 的局部位移 X 加个正值再跑一次。
"""

import unreal

TAG = '[BossSocket] '

MESH_PATH = '/Game/ParagonKhaimera/Characters/Heroes/Khaimera/Meshes/Khaimera'

# (插槽名, 父骨骼, 附加局部位移(cm), 附加局部旋转(度))
SOCKET_DEFS = [
    ('WeaponStart', 'hand_r',      (0.0, 0.0, 0.0), (0.0, 0.0, 0.0)),
    ('WeaponEnd',   'middle_03_r', (0.0, 0.0, 0.0), (0.0, 0.0, 0.0)),
]


def note(msg):
    unreal.log_warning(TAG + str(msg))


def fail(msg):
    unreal.log_error(TAG + str(msg))


def socket_names(mesh):
    """当前网格上的插槽名列表（只读属性，读取是允许的）。"""
    try:
        return [str(s.get_editor_property('socket_name'))
                for s in mesh.get_editor_property('sockets')]
    except Exception as exc:  # noqa: BLE001
        fail('读取 sockets 失败：%r' % exc)
        return []


def rename_socket(mesh, old_name, new_name):
    """静态接口，优先在类上调用；个别版本只暴露在子系统实例上，做个兜底。"""
    old_n = unreal.Name(old_name)
    new_n = unreal.Name(new_name)
    try:
        return unreal.SkeletalMeshEditorSubsystem.rename_socket(mesh, old_n, new_n)
    except Exception:
        subsys = unreal.get_editor_subsystem(unreal.SkeletalMeshEditorSubsystem)
        return subsys.rename_socket(mesh, old_n, new_n)


def main():
    mesh = unreal.load_asset(MESH_PATH)
    if mesh is None:
        fail('找不到网格资产：' + MESH_PATH)
        return

    skel = mesh.get_editor_property('skeleton')
    note('目标网格：%s' % mesh.get_name())
    note('骨架：%s' % (skel.get_name() if skel else 'None'))
    note('现有插槽：%s' % socket_names(mesh))

    added = []
    for name, bone, loc, rot in SOCKET_DEFS:
        if name in socket_names(mesh):
            note('%s 已存在，跳过' % name)
            continue

        before = set(socket_names(mesh))

        # ---- 1) 造一个 socket 对象并挂到网格上 ----
        # ⚠ SocketName / BoneName 都是 VisibleAnywhere（Python 里只读），
        #   所以不能直接赋值 —— 先挂上去（引擎会自动命名 SocketN、父骨骼=根骨骼），
        #   再用公开接口把它改名、改父骨骼。
        sock = unreal.new_object(unreal.SkeletalMeshSocket, mesh)
        if sock is None:
            fail('%s 创建失败（unreal.new_object 返回 None）' % name)
            continue

        mesh.add_socket(sock, False)

        fresh = [n for n in socket_names(mesh) if n not in before]
        if not fresh:
            fail('%s 挂载失败（add_socket 没生效；注意上面 LogSkeletalMesh 的报错）' % name)
            continue
        auto_name = fresh[0]

        # ---- 2) 指定父骨骼（会校验骨骼是否存在）----
        sock.set_socket_parent(mesh, unreal.Name(bone))
        bound = str(sock.get_editor_property('bone_name'))
        if bound != bone:
            fail('%s 父骨骼设置失败：想要 %s，实际 %s（骨骼名写错了？）' % (name, bone, bound))
            continue

        # ---- 3) 改成 C++ 认的名字 ----
        if not rename_socket(mesh, auto_name, name):
            fail('%s 改名失败（%s → %s）' % (name, auto_name, name))
            continue

        # ---- 4) 可选的附加位移 / 旋转 ----
        if any(abs(v) > 1e-4 for v in loc) or any(abs(v) > 1e-4 for v in rot):
            sock.set_socket_local_transform(unreal.Transform(
                location=unreal.Vector(loc[0], loc[1], loc[2]),
                rotation=unreal.Rotator(rot[0], rot[1], rot[2]),
                scale=unreal.Vector(1.0, 1.0, 1.0),
            ))

        added.append(name)
        note('已添加：%s  →  骨骼 %s' % (name, bone))

    if added:
        try:
            unreal.EditorAssetLibrary.save_loaded_asset(mesh)
            note('已保存网格资产')
        except Exception as exc:  # noqa: BLE001
            fail('保存失败，请手动 Ctrl+S 保存：%r' % exc)

    final = socket_names(mesh)
    note('最终插槽列表：%s' % final)

    missing = [n for n, _, _, _ in SOCKET_DEFS if n not in final]
    if missing:
        fail('仍然缺失：%s' % missing)
        note('手工兜底：打开 %s → 切到 Skeleton 标签 → 右键 hand_r → Add Socket' % MESH_PATH)
    else:
        note('完成。直接 PIE 即可 —— 日志里 [Melee] BP_Boss 那行应变成 '
             'WeaponStart=找到，WeaponEnd=找到')


main()
