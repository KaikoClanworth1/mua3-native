# Repeatable performance benchmark: a hidden, muted test instance standing in ST010 gameplay.
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\bench.ps1 -Tag baseline [-Runs 3] [-Seconds 120] [-Exe path]
# Each run boots fresh, drives into the game, lets it settle, measures, stops only its own instance. Results go to
# build\testinst\bench: <tag>_<n>.json, <tag>_<n>.png (check by eye that it really was gameplay) and results.csv.
# This PC always has some background load (about 34% CPU measured during a run), so "noisy" means well above that
# usual level (others > 45% CPU or > 30% GPU).
# Compare builds on the median fps of runs that aren't noisy, alternating the builds.
param([string]$Tag = 'run', [int]$Runs = 1, [int]$Seconds = 120, [int]$Settle = 60, [int]$Interval = 10, [string]$Exe = '')
$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot
$Inst = "$Root\build\testinst\inst.ps1"
$D = "$Root\build\testinst\inst1"
$Log = "$D\err.log"
$Out = "$Root\build\testinst\bench"
New-Item -ItemType Directory -Force $Out | Out-Null
if ($Exe) { $env:EXE = (Resolve-Path $Exe).Path } else { Remove-Item Env:EXE -ErrorAction SilentlyContinue }
$Logical = [Environment]::ProcessorCount

Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class BenchThread {
  [DllImport("kernel32.dll")] static extern IntPtr OpenThread(uint access, bool inherit, uint id);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  [DllImport("kernel32.dll")] static extern int GetThreadDescription(IntPtr h, out IntPtr name);
  [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr p);
  public static string Name(int id) {
    IntPtr h = OpenThread(0x0040, false, (uint)id); // QUERY_INFORMATION (the limited right is refused)
    if (h == IntPtr.Zero) return "";
    IntPtr p; string s = "";
    if (GetThreadDescription(h, out p) >= 0 && p != IntPtr.Zero) { s = Marshal.PtrToStringUni(p); LocalFree(p); }
    CloseHandle(h);
    return s;
  }
}
"@

function Invoke-Inst([string[]]$ArgList, [int]$TimeoutSec) {
    # Never piped: the game would inherit the pipe and the wait would never end.
    $all = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$Inst`"") + $ArgList
    $p = Start-Process powershell -ArgumentList $all -WindowStyle Hidden -PassThru
    if (-not $p.WaitForExit($TimeoutSec * 1000)) { $p.Kill(); return $false }
    return $true
}
function Read-Log {
    $fs = [IO.File]::Open($Log, 'Open', 'Read', 'ReadWrite')
    try { (New-Object IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Dispose() }
}
function Get-InstProcess {
    $id = [int](Get-Content "$D\pid" -Raw).Trim()
    $p = Get-Process -Id $id -ErrorAction SilentlyContinue
    if ($p -and $p.Path -and $p.Path.StartsWith($D, [StringComparison]::OrdinalIgnoreCase)) { $p } else { $null }
}
function Get-ThreadTimes($p) {
    $t = @{}
    $p.Refresh()
    foreach ($th in $p.Threads) { try { $t[[int]$th.Id] = $th.TotalProcessorTime.TotalMilliseconds } catch {} }
    $t
}
function Get-Median([double[]]$v) { if (-not $v.Count) { return 0 }; $s = $v | Sort-Object; $s[[int][Math]::Floor($s.Count / 2)] }

function Invoke-Run([int]$N) {
    $name = "${Tag}_$N"
    Write-Output "[$name] boot $(Get-Date -Format HH:mm:ss)"
    if (-not (Invoke-Inst @('start', '1') 180)) { throw 'start timed out' }
    $inGame = $false
    for ($k = 0; $k -lt 2 -and -not $inGame; $k++) { $inGame = Invoke-Inst @('togame', '1') 420 } # togame sometimes stalls; a second try works
    if (-not $inGame) { [void](Invoke-Inst @('stop', '1') 60); throw 'never reached the game' }
    Start-Sleep $Settle

    $p = Get-InstProcess
    if (-not $p) { throw 'instance died before measuring' }
    $instId = $p.Id
    $startLen = (Read-Log).Length
    $t0 = Get-Date
    $cpu0 = $p.TotalProcessorTime.TotalMilliseconds
    $th0 = Get-ThreadTimes $p
    $samples = New-Object Collections.ArrayList
    $counters = @(
        "\Processor(_Total)\% Processor Time",
        "\GPU Engine(pid_${instId}_*engtype_3D)\Utilization Percentage",
        "\GPU Process Memory(pid_${instId}_*)\Dedicated Usage",
        "\GPU Process Memory(pid_${instId}_*)\Shared Usage")
    $prevCpu = $cpu0; $prevT = $t0
    while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
        Start-Sleep $Interval
        $p.Refresh()
        if ($p.HasExited) { throw 'instance died while measuring' }
        $now = Get-Date; $cpu = $p.TotalProcessorTime.TotalMilliseconds
        $selfPct = ($cpu - $prevCpu) / ($now - $prevT).TotalMilliseconds * 100 / $Logical
        $prevCpu = $cpu; $prevT = $now
        $c = (Get-Counter -Counter $counters -ErrorAction SilentlyContinue).CounterSamples
        $total = ($c | Where-Object { $_.Path -match 'processor\(_total\)' } | Measure-Object CookedValue -Sum).Sum
        $gpu3d = ($c | Where-Object { $_.Path -match 'engtype_3d' } | Measure-Object CookedValue -Sum).Sum
        $ded = ($c | Where-Object { $_.Path -match 'dedicated usage' } | Measure-Object CookedValue -Sum).Sum
        $sha = ($c | Where-Object { $_.Path -match 'shared usage' } | Measure-Object CookedValue -Sum).Sum
        $smi = (& nvidia-smi --query-gpu=utilization.gpu,memory.used,clocks.gr --format=csv,noheader,nounits) -split ',\s*'
        [void]$samples.Add([pscustomobject]@{
            cpu = $selfPct; other_cpu = [Math]::Max(0, $total - $selfPct); gpu3d = $gpu3d; card = [double]$smi[0]
            other_gpu = [Math]::Max(0, [double]$smi[0] - $gpu3d); vram_mb = $ded / 1MB; shared_mb = $sha / 1MB
            card_mem_mb = [double]$smi[1]; clock = [double]$smi[2]; commit_gb = $p.PagedMemorySize64 / 1GB; ws_gb = $p.WorkingSet64 / 1GB
        })
    }
    $elapsedMs = ((Get-Date) - $t0).TotalMilliseconds
    $th1 = Get-ThreadTimes $p
    $threads = foreach ($id in $th1.Keys) {
        $base = 0.0; if ($th0.ContainsKey($id)) { $base = $th0[$id] }
        $pct = ($th1[$id] - $base) / $elapsedMs * 100 # of one core
        if ($pct -ge 1) { [pscustomobject]@{ name = [BenchThread]::Name($id); core_pct = [Math]::Round($pct, 1) } }
    }
    $threads = @($threads | Sort-Object core_pct -Descending | Select-Object -First 12)
    $text = Read-Log
    [void](Invoke-Inst @('shot', '1', "bench_$name") 60)
    $png = "$D\screenshots\live\bench_$name.png"
    if (Test-Path $png) { Copy-Item $png "$Out\$name.png" -Force }
    [void](Invoke-Inst @('stop', '1') 60)

    $fps = New-Object Collections.ArrayList; $p50 = New-Object Collections.ArrayList; $p99 = New-Object Collections.ArrayList; $max = New-Object Collections.ArrayList; $busy = New-Object Collections.ArrayList
    foreach ($l in ($text.Substring($startLen) -split "`n")) {
        if ($l -match 'Presenting: ([\d.]+) fps \(native\) gpu busy (\d+)%') {
            [void]$fps.Add([double]$Matches[1]); [void]$busy.Add([double]$Matches[2])
            # builds before the frame-time figures report 0 for them
            if ($l -match 'frame ms p50 ([\d.]+) p99 ([\d.]+) max ([\d.]+)') { [void]$p50.Add([double]$Matches[1]); [void]$p99.Add([double]$Matches[2]); [void]$max.Add([double]$Matches[3]) }
        }
    }
    if (-not $fps.Count) { throw 'no Presenting lines measured' }
    $bad = @(($text -split "`n") | Where-Object { $_ -match 'Panic|has no recompiled version' }).Count
    function Avg($prop) { [Math]::Round(($samples | Measure-Object $prop -Average).Average, 1) }
    $otherCpu = Avg 'other_cpu'; $otherGpu = Avg 'other_gpu'
    $r = [ordered]@{
        tag = $Tag; run = $N; when = $t0.ToString('s'); exe_bytes = (Get-Item "$D\bin\mua3.exe").Length
        fps_median = (Get-Median $fps); fps_avg = [Math]::Round(($fps | Measure-Object -Average).Average, 1); fps_min_5s = ($fps | Measure-Object -Minimum).Minimum; fps_max_5s = ($fps | Measure-Object -Maximum).Maximum
        ms_p50 = (Get-Median $p50); ms_p99 = (Get-Median $p99); ms_worst = ($max | Measure-Object -Maximum).Maximum
        cpu_pct_of_pc = Avg 'cpu'; cpu_cores_used = [Math]::Round((Avg 'cpu') * $Logical / 100, 2); gpu_thread_busy_pct = (Get-Median $busy)
        gpu3d_pct = Avg 'gpu3d'; gpu_clock_mhz = Avg 'clock'; vram_mb = Avg 'vram_mb'; shared_gpu_mb = Avg 'shared_mb'
        commit_gb = [Math]::Round((Avg 'commit_gb'), 2); working_set_gb = [Math]::Round((Avg 'ws_gb'), 2)
        other_cpu_pct = $otherCpu; other_gpu_pct = $otherGpu; noisy = ($otherCpu -gt 45 -or $otherGpu -gt 30); panics_or_missing = $bad
        threads = $threads; fps_windows = $fps
    }
    $r | ConvertTo-Json -Depth 4 | Set-Content "$Out\$name.json" -Encoding utf8
    $csv = "$Out\results.csv"
    if (-not (Test-Path $csv)) { Set-Content $csv 'tag,run,when,fps_median,fps_avg,fps_min_5s,ms_p50,ms_p99,ms_worst,cpu_cores,gpu_thread_busy,gpu3d,vram_mb,commit_gb,ws_gb,other_cpu,other_gpu,noisy,bad' -Encoding ascii }
    Add-Content $csv (@($Tag, $N, $r.when, $r.fps_median, $r.fps_avg, $r.fps_min_5s, $r.ms_p50, $r.ms_p99, $r.ms_worst, $r.cpu_cores_used, $r.gpu_thread_busy_pct, $r.gpu3d_pct, $r.vram_mb, $r.commit_gb, $r.working_set_gb, $otherCpu, $otherGpu, $r.noisy, $bad) -join ',') -Encoding ascii
    Write-Output "[$name] fps median $($r.fps_median) avg $($r.fps_avg) (5s min $($r.fps_min_5s)) ms p50 $($r.ms_p50) p99 $($r.ms_p99) worst $($r.ms_worst) | cpu $($r.cpu_cores_used) cores, gpu thread $($r.gpu_thread_busy_pct)%, card 3D $($r.gpu3d_pct)%, vram $($r.vram_mb) MB, commit $($r.commit_gb) GB | others cpu $otherCpu% gpu $otherGpu% noisy=$($r.noisy) bad=$bad"
    Write-Output ('   threads: ' + (($threads | ForEach-Object { "$($_.name)=$($_.core_pct)" }) -join ', '))
}

for ($n = 1; $n -le $Runs; $n++) {
    try { Invoke-Run $n } catch { Write-Output "[${Tag}_$n] FAILED: $_"; [void](Invoke-Inst @('stop', '1') 60) }
}
Write-Output 'bench done'
