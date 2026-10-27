# LiSRemasteredTweaks 1.0.0

Graphics add-on for **Life is Strange Remastered** (PC, Steam), for ReShade with add-on support.
Raises the game's lighting, shadow and detail settings beyond what its menu offers and adds HDR
output, live, from an in-game overlay.

- **HDR**: the game's menu has no HDR option. The HDR tab keeps Deck Nine's colour grade: the
  game renders as usual and the add-on turns the finished picture into HDR10, stretching only
  the brightest parts, with **Peak brightness** and **Paper white** sliders (and a button that
  uses the peak Windows reports for your display). Windows HDR must be on. Tested on DirectX 12.
  Unreal's own HDR output isn't offered: it drops the grade (cooler, paler, lifted shadows).
- **Lighting**: reflections on rougher surfaces, finer lighting on translucent effects. The
  menu's highest setting already maxes reflection quality, ambient occlusion and volumetric fog.
- **Shadows**: 4096 / 8192 shadow maps, longer shadow distance, contact-hardening shadows,
  full-resolution character shadows.
- **Characters and detail**: full skin shading, most detailed characters and models, more texture
  memory, 16x filtering, longer draw distance, 8 anti-aliasing samples (Deck Nine set them to 4).
- **Post-processing**: film grain, lens flares, depth of field, chromatic aberration, sharpening
  (the game already turns motion blur off).
- **Quality presets**: *Ultra+* and *Cinematic*. On an RTX 5080 at 4K with a 156 fps cap both
  cost no frames (the GPU stayed at about 50%). **Measure preset costs** and
  **Measure each option** check them on your PC.
- **F8** switches every tweak off and on, to compare.

Every option starts at the game's own value. No ray tracing: the game was built without it, so the
ray tracing shaders aren't in its files.

## Install

1. Install ReShade 6.8 or later with full add-on support for `LiS-Win64-Shipping.exe`, in
   `LIS - Remastered\LIS\Binaries\Win64`.
2. Copy `LiSRemasteredTweaks.addon64` into that folder.
3. Optional: add `-dx12` to the game's Steam launch options (tested: runs fine). Open ReShade
   (Home key): the LiSRemasteredTweaks window opens beside it, and you can move and resize it.
   If you close it, turn it back on from the add-on's entry in ReShade's Add-ons tab.

Settings are saved in `ReShade.ini` under `[LiSRemasteredTweaks]`. Log: `LiSRemasteredTweaks.log`
next to the game's exe (and `-probe.txt`, every setting's value). Tested with Steam build 11330282.

## Build

`.\build.ps1` (Visual Studio 2022 C++ tools); `.\build.ps1 -Install` copies the add-on into the game
folder (game closed).

## How it works

Ported from TrueColorsTweaks (UE 4.25). Everything goes through the game's console variables, set
on the game thread (from the console manager's once-per-frame sinks call), at console priority;
letting go puts the game's value and priority back. HDR is a full-screen pass the add-on draws at
each present, before ReShade's effects and overlay (`src/hdr.hlsl`, compiled with the Windows SDK's
`fxc` by `build.ps1`). `docs/FINDINGS.md` has the UE 4.23 layout, what the game ships and the HDR
measurements.
