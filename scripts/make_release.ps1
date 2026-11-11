# Packages the release: the setup (MUA3Setup.exe), the recompiler and the port's prebuilt libraries (sdk/), so players
# build the game on their own PC from their own files. It contains none of the game: no recompiled code (nxrecomp*.lib,
# recomp_full/), no game data, no keys, and the setup is built without the icon made from the game (the setup makes
# the game's icon on the player's PC, from their own files).
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\make_release.ps1 -Version 0.2.0 [-Zip]
param([string]$Version = '0.2.0', [switch]$Zip)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'build'
$cfg = 'RelWithDebInfo'
$name = "MUA3Native-v$Version-win64"
$out = Join-Path $build "release\$name"

function Build([string[]]$Targets) {
    & cmake --build $build --config $cfg --target $Targets -- /m:1 /p:CL_MPCount=4 /v:m /nologo | Select-String 'error|\.vcxproj ->'
    if ($LASTEXITCODE) { throw "build failed: $Targets" }
}

# the setup without the game's icon, the tools, and the libraries mua3 links
& cmake -S $root -B $build -DMUA3_GAME_ICON=OFF | Out-Null
try {
    Build @('MUA3Setup', 'recomp', 'nxcore', 'platform', 'nxloader', 'imgui')
    Build @('mua3') # (only for main.obj / alloc_trace.obj: the exe itself is never packaged)
} finally {
    & cmake -S $root -B $build -DMUA3_GAME_ICON=ON | Out-Null
}

Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
foreach ($d in 'sdk\include\core\cpu\recomp', 'sdk\lib', 'sdk\obj', 'sdk\recomp', 'sdk\tools') { New-Item -ItemType Directory -Force (Join-Path $out $d) | Out-Null }
$bin = Join-Path $build "bin\$cfg"
Copy-Item "$bin\MUA3Setup.exe" $out
Copy-Item "$bin\recomp.exe" "$out\sdk\tools"
Copy-Item "$root\src\core\cpu\recomp\recomp_rt.h" "$out\sdk\include\core\cpu\recomp"
$libs = @(
    "src\$cfg\nxcore.lib", "src\$cfg\platform.lib", "src\$cfg\nxloader.lib", "$cfg\imgui.lib",
    "external\SDL\$cfg\SDL3-static.lib", "external\dynarmic\externals\fmt\$cfg\fmt.lib")
foreach ($l in $libs) { Copy-Item (Join-Path $build $l) "$out\sdk\lib" }
foreach ($o in 'main.obj', 'alloc_trace.obj') { Copy-Item (Join-Path $build "src\mua3.dir\$cfg\$o") "$out\sdk\obj" }
# function lists for the recompiler: coverage seen in game runs (prof*, missing*) and the functions this compiler
# version needs built unoptimised (noopt) or left out (skip). Addresses only.
$work = Join-Path $build 'recomp_work'
Get-ChildItem $work -File | Where-Object { $_.Name -match '^(prof\d+|missing\d+|noopt|skip|nogm|hugeopt)\.txt$' } | Copy-Item -Destination "$out\sdk\recomp"
@"
MUA3 Native v$Version
=====================

A native Windows port of Marvel Ultimate Alliance 3: The Black Order, built on your PC from your own copy.
This package contains no part of the game.

1. Install Microsoft's free "Build Tools for Visual Studio 2026" (workload: Desktop development with C++) if you
   don't have Visual Studio 2026 already. The game is compiled on your PC with it.
2. Run MUA3Setup.exe. It finds your base game, the 4.0.1 update and any DLC (.nsp / .xci) and your prod.keys
   (or Browse / drop them on the window), asks where to install, and shows the space needed.
3. Press "Build the game": it installs the game data, translates and compiles the game and prepares its
   shaders (setup shows how long it takes on your PC; it stays usable meanwhile).
4. Choose a desktop shortcut and a Start-menu entry, then Play. Every setting is in the game
   (title screen > SYSTEM > GRAPHICS). Run MUA3Setup.exe again, or use Windows Settings > Apps, to update,
   repair or uninstall.

https://github.com/KaikoClanworth1/mua3-native
"@ | Set-Content "$out\READ ME FIRST.txt" -Encoding utf8

# nothing of the game may be in here
$bad = Get-ChildItem $out -Recurse -File | Where-Object { $_.Name -match 'nxrecomp|recomp_\d+|\.ico$|\.nso$|\.nca$|\.keys$|^main\.bin$' -or $_.FullName -match '\\game\\|recomp_full' }
if ($bad) { throw "game material in the package: $($bad.FullName -join ', ')" }
$size = (Get-ChildItem $out -Recurse -File | Measure-Object Length -Sum).Sum
"packaged $out ($([math]::Round($size / 1MB)) MB, $((Get-ChildItem $out -Recurse -File).Count) files)"
if ($Zip) {
    $zipPath = "$out.zip"
    Remove-Item $zipPath -ErrorAction SilentlyContinue
    Compress-Archive -Path $out -DestinationPath $zipPath -CompressionLevel Optimal
    "zip $zipPath ($([math]::Round((Get-Item $zipPath).Length / 1MB)) MB)"
}
