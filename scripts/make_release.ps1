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
$name = "MUA3Native-v$Version-Windows-x64" # (system and processor in the name: launchers pick the file by them)
$out = Join-Path $build "release\$name"

function Build([string[]]$Targets) {
    & cmake --build $build --config $cfg --target $Targets -- /m:1 /p:CL_MPCount=4 /v:m /nologo | Select-String 'error|\.vcxproj ->'
    if ($LASTEXITCODE) { throw "build failed: $Targets" }
}

# The build must not see a recompiled library: nxcore links nxrecomp PUBLIC, so with MUA3_RECOMP_DIR pointing at a
# nxrecomp_full.lib every exe that links nxcore (MUA3Setup too) would carry the game's code.
$cache = Join-Path $build 'CMakeCache.txt'
if (Test-Path $cache) {
    $rd = (Select-String $cache -Pattern '^MUA3_RECOMP_DIR:PATH=(.*)$').Matches | ForEach-Object { $_.Groups[1].Value }
    if ($rd -and (Test-Path (Join-Path $rd 'nxrecomp_full.lib'))) { throw "MUA3_RECOMP_DIR ($rd) holds nxrecomp_full.lib: release builds must use a build folder without the recompiled game" }
    if ($rd -and (Test-Path (Join-Path $rd 'recomp_table.cpp'))) { throw "MUA3_RECOMP_DIR ($rd) holds generated game code" }
}
# the setup without the game's icon, the tools, and the libraries mua3 links. Without NVIDIA's DLSS SDK too
# (MUA3_DLSS=OFF): it is proprietary, and the packaged libraries must link on a PC that does not have it.
& cmake -S $root -B $build -DMUA3_GAME_ICON=OFF "-DMUA3_VERSION_NAME=v$Version" -DMUA3_DLSS=OFF | Out-Null
try {
    Build @('MUA3Setup', 'recomp', 'nxcore', 'platform', 'nxloader', 'imgui')
    Build @('mua3') # (only for main.obj / alloc_trace.obj: the exe itself is never packaged)
} finally {
    & cmake -S $root -B $build -DMUA3_GAME_ICON=ON "-DMUA3_VERSION_NAME=" -DMUA3_DLSS=ON | Out-Null # (dev builds: git describe again)
}

Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
foreach ($d in 'sdk\include\core\cpu\recomp', 'sdk\lib', 'sdk\obj', 'sdk\recomp', 'sdk\tools') { New-Item -ItemType Directory -Force (Join-Path $out $d) | Out-Null }
$bin = Join-Path $build "bin\$cfg"
Copy-Item "$bin\MUA3Setup.exe" $out
# RecompLauncher: sets the game up from the player's files and shows its settings. prepare.version is this release's
# version: RecompLauncher re-runs prepare (rebuilding the game) only when it changes, and every release changes what
# is built on the player's PC (runtime, recompiler lists). Without it, updated installs kept their old game (#10).
$rl = Get-Content "$root\recomplauncher.json" -Raw
$rl = [regex]::Replace($rl, '("prepare"\s*:\s*\{)(\s*"version"\s*:\s*"[^"]*"\s*,)?', "`$1`n    `"version`": `"$Version`",")
if ($rl -notmatch "`"version`": `"$([regex]::Escape($Version))`"") { throw 'recomplauncher.json: prepare.version not set' }
[IO.File]::WriteAllText((Join-Path $out 'recomplauncher.json'), $rl, (New-Object Text.UTF8Encoding($false)))
Copy-Item "$bin\recomp.exe" "$out\sdk\tools"
Copy-Item "$root\src\core\cpu\recomp\recomp_rt.h" "$out\sdk\include\core\cpu\recomp"
$libs = @(
    "src\$cfg\nxcore.lib", "src\$cfg\platform.lib", "src\$cfg\nxloader.lib", "$cfg\imgui.lib",
    "external\SDL\$cfg\SDL3-static.lib", "external\dynarmic\externals\fmt\$cfg\fmt.lib")
foreach ($l in $libs) { Copy-Item (Join-Path $build $l) "$out\sdk\lib" }
# every source of the mua3 target, read from src/CMakeLists.txt so a new one can't be left out (v0.3.1 missed
# debug_chapter.obj: issue #2). The player's setup links every .obj in sdk\obj.
$m = [regex]::Match((Get-Content "$root\src\CMakeLists.txt" -Raw), 'add_executable\(mua3\s+([^)]*)\)')
if (-not $m.Success) { throw 'add_executable(mua3 ...) not found in src/CMakeLists.txt' }
foreach ($s in ($m.Groups[1].Value -split '\s+' | Where-Object { $_ -like '*.cpp' })) {
    $o = [IO.Path]::GetFileNameWithoutExtension($s) + '.obj'
    Copy-Item (Join-Path $build "src\mua3.dir\$cfg\$o") "$out\sdk\obj" -ErrorAction Stop
}
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
   don't have Visual Studio 2026 already. The game is compiled on your PC with it. Also tick the individual
   component "C++ Clang Compiler for Windows" (recommended): the build takes about 15-30 min instead of hours and
   the game runs a little faster. Without it the setup builds with Microsoft's compiler.
2. Run MUA3Setup.exe. It finds your base game, the 4.0.1 update and any DLC (.nsp / .xci) and your prod.keys
   (or Browse / drop them on the window), asks where to install, and shows the space needed.
3. Press "Build the game": it installs the game data, translates and compiles the game and prepares its
   shaders (setup shows how long it takes on your PC; it stays usable meanwhile).
4. Choose a desktop shortcut and a Start-menu entry, then Play. Every setting is in the game
   (title screen > SYSTEM > GRAPHICS). Run MUA3Setup.exe again, or use Windows Settings > Apps, to update,
   repair or uninstall.
5. Mods made for the Switch version (retextures, data and UI files) go in the "mods" folder of the installed game
   (mods\README.txt there explains it). Update, repair and uninstall leave that folder alone.

https://github.com/KaikoClanworth1/mua3-native
"@ | Set-Content "$out\READ ME FIRST.txt" -Encoding utf8

# nothing of the game may be in here
$bad = Get-ChildItem $out -Recurse -File | Where-Object { $_.Name -match 'nxrecomp|recomp_\d+|\.ico$|\.nso$|\.nca$|\.keys$|^main\.bin$' -or $_.FullName -match '\\game\\|recomp_full' }
if ($bad) { throw "game material in the package: $($bad.FullName -join ', ')" }
# NVIDIA's DLSS SDK (third_party/dlss: headers, nvsdk_ngx*.lib, nvngx_dlss.dll) is never packaged, nor linked into the
# packaged libraries.
$nv = Get-ChildItem $out -Recurse -File | Where-Object { $_.Name -match '^(nvngx|nvsdk)' -or $_.FullName -match '\\third_party\\dlss\\' }
if ($nv) { throw "NVIDIA DLSS SDK files in the package: $($nv.FullName -join ', ')" }
$nvlinked = Get-ChildItem $out -Recurse -File -Include *.lib, *.obj, *.exe | Where-Object { Select-String $_.FullName -Pattern 'NVSDK_NGX_D3D11_Init' -SimpleMatch -Quiet }
if ($nvlinked) { throw "DLSS (NVIDIA's NGX) is built into packaged files: $($nvlinked.FullName -join ', ')" }
# and none linked into the setup: its PDB names every library it linked
$pdb = Join-Path $bin 'MUA3Setup.pdb'
if ((Test-Path $pdb) -and (Select-String $pdb -Pattern 'nxrecomp_full|recomp_full\\|recomp_\d+\.obj' -SimpleMatch:$false -Quiet)) { throw 'MUA3Setup linked the recompiled game code' }
if ((Get-Item "$out\MUA3Setup.exe").Length -gt 64MB) { throw 'MUA3Setup.exe is far larger than expected (recompiled game code linked in?)' }
$size = (Get-ChildItem $out -Recurse -File | Measure-Object Length -Sum).Sum
"packaged $out ($([math]::Round($size / 1MB)) MB, $((Get-ChildItem $out -Recurse -File).Count) files)"
if ($Zip) {
    $zipPath = "$out.zip"
    Remove-Item $zipPath -ErrorAction SilentlyContinue
    Compress-Archive -Path $out -DestinationPath $zipPath -CompressionLevel Optimal
    "zip $zipPath ($([math]::Round((Get-Item $zipPath).Length / 1MB)) MB)"
}
