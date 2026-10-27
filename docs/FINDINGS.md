# Findings: Life is Strange Remastered

## The game

- Steam app 1265920, build 11330282. Folder `LIS - Remastered`, project **LIS**, exe
  `LIS\Binaries\Win64\LiS-Win64-Shipping.exe` (PE timestamp 0x64711d71, not signed or protected,
  no PDB shipped). **Unreal Engine 4.23** (`++UE4+Release-4.23` in the exe).
- Config: `%LOCALAPPDATA%\LiS\Saved\Config\WindowsNoEditor\`. GameUserSettings class
  `D9GameUserSettings`.
- Paks: version 8, not encrypted, not compressed (`tools/pak8.py <pak> list|extract <glob> [out]`).

## Graphics API

- No `[/Script/WindowsTargetPlatform...]` section in DefaultEngine.ini, so the engine default:
  DirectX 11. The only shader cache is `Engine/GlobalShaderCache-PCD3D_SM5.bin` (material shaders
  are inline in the assets); in 4.23 SM5 also serves D3D12, and D3D12RHI is in the exe.
- `-dx12` on Steam's launch options works: D3D12Core.dll loads, no visual problems (user,
  2026-10-27).

## No ray tracing

DefaultEngine.ini has no `r.RayTracing` and sets `r.SkinCache.CompileShaders=False`; the global
shader cache has no ray tracing shaders. The project was never cooked for ray tracing, so the RT
cvars in the exe do nothing. SSGI, GTAO, sky atmosphere and hair strands came after 4.23.

## Settings the project ships

DefaultEngine.ini: `r.DefaultFeature.AntiAliasing=2` (TAA), `r.TemporalAASamples=4`,
`r.TemporalAACurrentFrameWeight=0.04`, `r.BasePassOutputsVelocity=True`, AO / auto exposure /
motion blur / lens flare off by default (`r.DefaultFeature.*`). No HDR option in the menu
(`bUseHDRDisplayOutput` in GameUserSettings.ini is Unreal's stock field).

## Console variables

From the exe's code (offline scans):
- `IConsoleManager::Singleton` RVA **0x355b080** (5 of the 6 references to `L"r.ScreenPercentage"` load it; the 6th loads nothing nearby).
- IConsoleManager: `+0x40` CallAllConsoleVariableSinks, `+0x48` RegisterConsoleVariableSink,
  `+0xa0` FindConsoleVariable (1778 call sites). 4.25 has two more slots before these (bool cvar
  registration), so every offset is 0x10 lower than in TrueColorsTweaks.
- IConsoleObject: IsVariableInt/Float/String (no IsVariableBool in 4.23; found at run time),
  AsVariableInt `+0x48`, AsVariableFloat `+0x50`.
- IConsoleVariable: Set `+0x70` (75 call sites right after `mov r8d, SetBy`), GetInt `+0x78`
  (result tested with `test eax`), GetFloat `+0x80` and GetString `+0x88` from the 4.23 interface
  order (no GetBool). The add-on checks GetFloat by reading r.ScreenPercentage before it sets
  anything.
- SetBy priorities: 0x08000000 (code, 40 sites) and 0x09000000 (console, 43 sites), the 4.2x
  enum. The add-on sets at 0x09.

## First run (2026-10-27, DirectX 12, menu all at its highest)

Layout check passed: singleton 5 of 5 matches, IsVariableInt at +0x30, r.MaxAnisotropy 8 and
r.ScreenPercentage 100 read through GetInt/GetFloat (so +0x78/+0x80 are right), data cvars keep
TConsoleVariableData at +0x38. The game-thread hook went in (CallAllConsoleVariableSinks RVA
0x7c8cc0).

The game's values in a scene (the 5 s and 60 s probes are identical):

| Setting | Game | Option |
|---|---|---|
| r.SSR.Quality / MaxRoughness | 4 / -1 (post-process volume's) | Cinematic already; only rougher surfaces offered |
| r.AmbientOcclusionLevels / MaxQuality | -1 (auto) / 100 | nothing to raise: no option |
| r.VolumetricFog.GridPixelSize / GridSizeZ | 4 / 128 | Cinematic already: no option |
| r.ViewDistanceScale | 2 | options 3x and 4x |
| r.MotionBlurQuality | 0, set by the game at console priority (0x09) | already off: no option |
| r.Shadow.MaxResolution / MaxCSMResolution | 2048 / 2048 | 4096 / 8192 |
| r.Shadow.DistanceScale, CSM.MaxCascades | 1, 10 | |
| r.SSS.Checkerboard / HalfRes / Quality | 2 (auto) / 0 / 1 | Full = Checkerboard 0 |
| r.Streaming.PoolSize | 3000 | 4/6/8 GB |
| r.MaxAnisotropy | 8 | 16x |
| r.TemporalAASamples | 4 (code, 0x08) | 8 |
| r.TranslucencyLightingVolumeDim | 64 | 128 |
| r.Tonemapper.Quality / Sharpen | 5 / 0.5 | |
| r.DepthOfFieldQuality, LensFlareQuality, SceneColorFringeQuality | 4, 3, 1 | |
| r.HDR.EnableHDROutput | 0 | HDR tab |

## HDR

Unreal's HDR (offered in 0.1.0, removed in 0.2.0): on DirectX 12, setting r.HDR.EnableHDROutput=1, OutputDevice 3|4, ColorGamut 2 on
the game thread makes the engine switch the swap chain to HDR10 PQ itself within about 0.2 s, and
back to sRGB when the option goes off (log 2026-10-27 14:11).

But it loses the game's look. Screenshots of the title screen (2026-10-27 14:17, SDR vs HDR,
the HDR one decoded from PQ and exposure-matched): colour balance per unit luminance R/G/B
1.38/0.91/0.81 in SDR vs 1.18/0.95/0.99 in HDR, mean saturation 0.54 vs 0.42, the darkest 2% of
the picture 3.4x brighter, mid-tones within 5%. The HDR path swaps the game's tone curve for
ACES and apparently skips its colour grading LUT (not confirmed in code).

Graded HDR (the HDR tab since 0.2.0): the game stays in SDR; its back buffer is
R10G10B10A2_UNORM anyway (ReShade.log). At each present, before ReShade's effects and overlay,
the add-on copies the frame and draws it back through src/hdr.hlsl (gamma 2.2 decode, paper
white, highlight stretch above 0.6 of SDR white towards the peak, Rec. 2020, PQ), sets the swap
chain to HDR10 with SetColorSpace1 and tells ReShade (effect_runtime::set_color_space). First
run 2026-10-27 14:30: pipeline built on DirectX 12 at 3840x2160, switching between modes clean,
the display reports 3700 nits (DXGI_OUTPUT_DESC1). The user's verdict: looks good. UI is in the
same image, so pure white text goes to the peak too.

## Preset costs

Measure preset costs (2026-10-27 14:12, RTX 5080, 4K native, DirectX 12, one scene): Game, Ultra+
and Cinematic all 6.41 ms / 156.0 fps over about 1250 frames each. A frame cap outside the game
(its FrameRateLimit is 0) holds 156 fps, and the GPU runs at about 50% (user), so both presets
cost no frames there. Their GPU cost under no cap isn't measured.
