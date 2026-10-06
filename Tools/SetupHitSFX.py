# 程序化生成命中音效（免外部素材）：SC_PlayerHit（普通命中）/ SC_PlayerHeavyHit（破防/暴击）
# 产物：/Game/FightPlayer/SFX/SC_PlayerHit + SC_PlayerHeavyHit
# 与 C++ 的兜底加载路径严格对应（FightPlayerCharacter::BeginPlay）：
#   /Game/FightPlayer/SFX/SC_PlayerHit.SC_PlayerHit
#   /Game/FightPlayer/SFX/SC_PlayerHeavyHit.SC_PlayerHeavyHit
#
# 声音设计（打击感里声音占一半，轻/重必须有可盲听的质感差）：
#   普通命中：短促"拍击" = 白噪爆点(τ≈25ms) + 低频肉感 thump(170→120Hz 扫频, τ≈60ms) + 3ms 起振 click
#   重击：    沉重"砸实" = 更深的 thump(110→70Hz, τ≈110ms) + 更大的噪声体 + 一点金属泛音(2.2kHz)
# 运行方式（编辑器关着，无头）：
#   UnrealEditor-Cmd.exe demo.uproject -ExecutePythonScript=Tools/SetupHitSFX.py -unattended -NullRHI ...

import unreal
import os
import struct
import math
import random
import wave

SR = 44100
OUT_DIR = os.path.dirname(__file__)


def synth_hit(big=False):
    """合成一记打击音，返回 int16 单声道采样列表。big=False 普通命中 / True 重击。"""
    dur = 0.28 if big else 0.16
    n = int(SR * dur)
    samples = [0.0] * n
    rnd = random.Random(20261001 if not big else 5150)

    # --- 1) 白噪爆点：命中瞬间的"拍" ---
    noise_tau = 0.11 if big else 0.025
    noise_gain = 0.55 if big else 0.42
    lp = 0.0
    for i in range(n):
        t = i / SR
        env = math.exp(-t / noise_tau)
        # 一阶低通软化刺耳的高频（普通命中滤得更多 → 更"脆"；重击少滤 → 更"炸"）
        alpha = 0.35 if big else 0.18
        lp += alpha * (rnd.uniform(-1, 1) - lp)
        samples[i] += lp * env * noise_gain

    # --- 2) 低频 thump：肉感/分量（扫频下坠 = 打击物的"塌陷"感）---
    f0, f1 = (110.0, 70.0) if big else (170.0, 120.0)
    thump_tau = 0.11 if big else 0.06
    thump_gain = 0.85 if big else 0.62
    phase = 0.0
    for i in range(n):
        t = i / SR
        f = f1 + (f0 - f1) * math.exp(-t / (thump_tau * 0.6))   # 指数下扫
        phase += 2.0 * math.pi * f / SR
        env = math.exp(-t / thump_tau)
        # 轻微过载让低频更"糊"更沉（软削波）
        s = math.sin(phase) * env * thump_gain
        samples[i] += max(-0.8, min(0.8, s * 1.3)) * 0.9

    # --- 3) 起振 click：前 3ms 的瞬态，负责"刀刃接触"的锐度 ---
    click_n = int(SR * 0.003)
    click_gain = 0.5 if big else 0.45
    for i in range(min(click_n, n)):
        env = 1.0 - i / click_n
        samples[i] += (rnd.uniform(-1, 1)) * env * click_gain

    # --- 4) 重击专属：金属泛音（2.2kHz 快衰减），加"铛"的高频棱角 ---
    if big:
        ring_gain = 0.12
        ring_tau = 0.05
        for i in range(n):
            t = i / SR
            samples[i] += math.sin(2.0 * math.pi * 2200.0 * t) * math.exp(-t / ring_tau) * ring_gain

    # --- 归一化 + 转 int16 ---
    peak = max(1e-6, max(abs(s) for s in samples))
    norm = (0.82 if not big else 0.92) / peak
    return [int(max(-32768, min(32767, s * norm * 32767))) for s in samples]


def write_wav(path, samples):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(struct.pack("<%dh" % len(samples), *samples))


def import_sound(wav_path, dest_name):
    atools = unreal.AssetToolsHelpers.get_asset_tools()
    task = unreal.AssetImportTask()
    task.filename = wav_path
    task.destination_path = "/Game/FightPlayer/SFX"
    task.destination_name = dest_name
    task.automated = True
    task.save = True
    task.replace_existing = True
    atools.import_asset_tasks([task])
    obj = unreal.load_object(None, "/Game/FightPlayer/SFX/%s.%s" % (dest_name, dest_name))
    unreal.log("[SFX][%s] %s -> %s" % ("OK" if obj else "FAIL", dest_name, obj))


hit = synth_hit(False)
heavy = synth_hit(True)
p1 = os.path.join(OUT_DIR, "_SC_PlayerHit.wav")
p2 = os.path.join(OUT_DIR, "_SC_PlayerHeavyHit.wav")
write_wav(p1, hit)
write_wav(p2, heavy)
import_sound(p1, "SC_PlayerHit")
import_sound(p2, "SC_PlayerHeavyHit")
os.remove(p1)
os.remove(p2)
unreal.log("[SFX] done: hit=%.3fs(%d samples) heavy=%.3fs(%d samples)" % (len(hit)/SR, len(hit), len(heavy)/SR, len(heavy)))
