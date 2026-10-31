# GFX inventory — seed (written by the orchestrator, 2026-10-30)

The GFX seat continues from here. Facts below were measured on this machine, not assumed.

## Hardware

| | |
|---|---|
| GPU | **NVIDIA GeForce RTX 3060 Ti**, driver 32.0.16.1742, 8 GB |
| iGPU | Intel UHD Graphics 770 (present, not the target) |
| CPU | **Intel Core i7-12700** — 12 cores / 20 threads |
| RAM | 31.8 GB |
| Display | **1920×1080 @ 144 Hz** (Generic PnP Monitor) |

**Consequence for "1440p":** there is no 1440p panel attached. 1440p on this machine means either
DSR/DLDSR supersampling on the 1080p output (sharpness win, real GPU cost) or in-game resolution
scaling. That is a decision the seat must make **with measurements**, not by preference.

## Config files (measured paths)

```
C:\Users\Administrator\Documents\Assassin's Creed IV Black Flag\Assassin4.ini        6407 B
C:\Users\Administrator\Documents\Assassin's Creed IV Black Flag\GFXSettings.AC4BFSP.xml 5626 B
```

`GamerProfile.xml` does **not** exist on this install — it is `Assassin4.ini` here.

## Current graphics keys (`[Graphics_DX11]` in Assassin4.ini, verbatim)

```
AdapterVendorID=4318      AdapterDeviceID=9353
DisplayWidth=1920         DisplayHeight=1080
RefreshRate=60            DisplayableRefreshRate=60        <-- panel is 144 Hz
AntiAliasingMode=1        VSync=1
EnvironmentQuality=3      TextureQuality=2
ShadowQuality=2           ReflectionQuality=2
GodRays=1                 MotionBlur=1
SSAO=1                    UseVolumetricFog=1
ApexParticles=0
```

Two immediate observations the seat must test, not assume:

1. **`RefreshRate=60` on a 144 Hz panel.** Even a modest frame target is currently capped at 60.
2. **`EnvironmentQuality=3` is presumably the menu maximum** — the LOD/draw-distance knob. The
   question is whether the engine accepts **4+** or rejects/clamps it, and what it does to frame
   time. That is the single most likely lever for the "higher LOD distance" requirement.

## Standing rules for this seat

- No engine RE, no code injection, no binary patching, no ASI.
- **Never touch `AC4Coop.asi` or `dinput8.dll`** — that loader belongs to the co-op mod.
- Snapshot every config file before changing it; record key / old / new / measured effect.
- Never call the `question` tool: a seat has no human. Measure or mark *unproven*.
- Report through the bus: `py ops\tools\bus.py report GFX DONE "…"`.

## PASS 0 remaining (do first)

1. Baseline FPS + **frametimes** at 1080p / 60 Hz current settings (game benchmark or PresentMon).
2. Baseline at 1080p / 144 Hz (change `RefreshRate` + `DisplayableRefreshRate`, VSync off or fast).
3. Baseline with DSR 1440p on the 1080p panel.
4. Write the numbers into this file under a "## Baseline" heading before proposing changes.
