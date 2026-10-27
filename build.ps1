# Builds bin\LiSRemasteredTweaks.addon64 with MSVC (Visual Studio 2022+ with C++ tools).
# MSVC is required: ReShade is built with it, and MinGW returns API structs differently.
# Usage: .\build.ps1 [-Install] [-Package] [-GameBin <folder of LiS-Win64-Shipping.exe>]
#   -Install  copies the add-on next to the game's exe (game closed).
#   -Package  makes bin\release\LiSRemasteredTweaks-<version>.zip: the add-on, README, LICENSE
#             and THIRD_PARTY_NOTICES.
param(
    [switch]$Install,
    [switch]$Package,
    [string]$GameBin = "E:\SteamLibrary\steamapps\common\LIS - Remastered\LIS\Binaries\Win64"
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$exe = "LiS-Win64-Shipping.exe"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$env:PATH = "$(Split-Path $vswhere);$env:PATH"  # vcvars64.bat runs vswhere by name
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio with the C++ x64 tools not found." }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"

$obj = "$root\bin\obj"
New-Item -ItemType Directory -Force $obj | Out-Null
$addon = "$root\bin\LiSRemasteredTweaks.addon64"

# ReShade add-on headers in third_party\reshade match ReShade 6.8.0. The graded-HDR shaders
# (src\hdr.hlsl) are compiled to DXBC headers in bin\obj with the Windows SDK's fxc.
$cmd = @(
    "call `"$vcvars`" >nul",
    "fxc /nologo /O3 /T vs_5_0 /E main_vs /Vn kHdrVs /Fh `"$obj\hdr_vs.h`" `"$root\src\hdr.hlsl`" >nul",
    "fxc /nologo /O3 /T ps_5_0 /E main_ps /Vn kHdrPs /Fh `"$obj\hdr_ps.h`" `"$root\src\hdr.hlsl`" >nul",
    ("cl /nologo /std:c++17 /O2 /EHsc /MT /W3 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX " +
     "/I `"$root\third_party\imgui`" /I `"$root\third_party\reshade\include`" /I `"$obj`" " +
     "/Fo`"$obj\\`" `"$root\src\main.cpp`" user32.lib " +
     "/link /DLL /OUT:`"$addon`" /PDB:`"$obj\LiSRemasteredTweaks.pdb`" /DEBUG")
) -join " && "
cmd /c $cmd
if ($LASTEXITCODE) { throw "build failed" }
Write-Host "Built $addon"

if ($Install) {
    if (-not (Test-Path "$GameBin\$exe")) { throw "$exe not found in $GameBin (pass -GameBin)" }
    if (Get-Process ([IO.Path]::GetFileNameWithoutExtension($exe)) -ErrorAction SilentlyContinue) { throw "Close the game first." }
    Copy-Item $addon $GameBin -Force
    Write-Host "Installed to $GameBin"
}

if ($Package) {
    $version = [regex]::Match((Get-Content "$root\src\version.h" -Raw), 'DT_VERSION "([^"]+)"').Groups[1].Value
    $name = "LiSRemasteredTweaks-$version"
    $stage = "$root\bin\release\$name"
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    New-Item -ItemType Directory -Force $stage | Out-Null
    Copy-Item $addon, "$root\README.md", "$root\LICENSE", "$root\THIRD_PARTY_NOTICES.txt" $stage
    $zip = "$root\bin\release\$name.zip"
    if (Test-Path $zip) { Remove-Item -Force $zip }
    Compress-Archive -Path "$stage\*" -DestinationPath $zip
    Write-Host "Packaged $zip"
    Get-ChildItem $stage -File | ForEach-Object { "{0,-36} {1,10:N0}" -f $_.Name, $_.Length }
}
