# Compare the speed of a candidate build with a baseline build in the 4-player ougi fight, on a Windows PC.
# The twin of tools/run/fight_compare.py (same rule, see its docstring): runs alternate baseline, candidate,
# baseline, candidate...; per run it prints the worst 10 s window (from the "[don] exit:" line), the process CPU
# over the whole run (% of one core, all threads) and the state hash; then PASS or FAIL.
# FAIL (exit code 1) when a run has no result, when the state hashes differ, or when the candidate's median worst
# window is lower than the baseline's median by more than the baseline's own spread (max - min).
#
# Single file, no modules, ASCII only. Example (the orchestrator runs it over SSH):
#   powershell -NoProfile -ExecutionPolicy Bypass -File fight_compare.ps1 `
#     -BaselineExe C:\DON\old\don.exe -BaselineUserDir C:\DON\user-old `
#     -CandidateExe C:\DON\new\don.exe -CandidateUserDir C:\DON\user-new `
#     -Script C:\DON\multiplayer_ougi.txt -Runs 3 -Work C:\DON\cmp
# The given user dirs are never run in: every run copies its build's folder to a temporary one and deletes it after
# (the game writes saves there; this is what tools/run/perf_fight.py --user-dir does), so runs do not accumulate state.
param(
    [Parameter(Mandatory = $true)][string]$BaselineExe,
    [Parameter(Mandatory = $true)][string]$BaselineUserDir,
    [Parameter(Mandatory = $true)][string]$CandidateExe,
    [Parameter(Mandatory = $true)][string]$CandidateUserDir,
    [Parameter(Mandatory = $true)][string]$Script,
    [int]$Runs = 3,
    [string]$Work = "$env:TEMP\fight_compare",
    [int]$FightEnd = 7800
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$env:DON_PLAY_MOVIES = '1'
$env:DON_TRACE_STATE = '60'
$env:SDL_AUDIODRIVER = 'dummy'

function Get-Median([double[]]$v) {
    $s = $v | Sort-Object
    $n = $s.Count
    if ($n % 2 -eq 1) { return [double]$s[($n - 1) / 2] }
    return ([double]$s[$n / 2 - 1] + [double]$s[$n / 2]) / 2
}

function Invoke-Run([string]$Exe, [string]$UserDir, [string]$Tag) {
    $shots = Join-Path $Work "$Tag-shots"
    $a = @('--headless', '--renderer', 'vulkan', '--sound', 'on', '--mute', '--players', '4',
           '--shots', $shots, '--input-script', $Script)
    $runDir = Join-Path $Work "$Tag-user"
    if (Test-Path $runDir) { Remove-Item -Recurse -Force $runDir }
    Copy-Item -Recurse -Force -Path $UserDir -Destination $runDir
    $env:DON_USER_DIR = $runDir
    $out = Join-Path $Work "$Tag.out"
    $err = Join-Path $Work "$Tag.err"
    $start = Get-Date
    $p = Start-Process -FilePath $Exe -ArgumentList $a -WorkingDirectory (Split-Path -Parent $Exe) `
        -NoNewWindow -Wait -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
    $wall = ((Get-Date) - $start).TotalSeconds
    $cpu = $null
    try { $cpu = 100.0 * $p.TotalProcessorTime.TotalSeconds / $wall } catch { }
    Remove-Item -Recurse -Force $runDir
    $lines = Get-Content -Path $err
    # Formats parsed below: '[don] state blank=' lines come from src/game/don_overrides_calls.cpp (DON_TRACE_STATE);
    # the '[don] exit: ... N frames, worst 10s window X fps' line comes from src/host/host.cpp (grep 'worst 10s window').
    $state = @($lines | Where-Object { $_ -match '^\[don\] state blank=' })
    $exit = @($lines | Where-Object { $_ -match '^\[don\] exit:' }) | Select-Object -Last 1
    $worst = $null
    $frames = 0
    if ($exit -match '(\d+) frames, worst 10s window ([\d.]+) fps') {
        $frames = [int]$Matches[1]
        $worst = [double]::Parse($Matches[2], [Globalization.CultureInfo]::InvariantCulture)
    }
    $sha = [Security.Cryptography.SHA256]::Create()
    $bytes = [Text.Encoding]::UTF8.GetBytes(($state -join "`n"))
    $hash = ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLower()
    if ($frames -lt $FightEnd) { $worst = $null }   # the fight end was not reached: no result
    return [pscustomobject]@{ Tag = $Tag; Worst = $worst; Cpu = $cpu; Hash = $hash }
}

function Show-Run($r) {
    $w = if ($null -eq $r.Worst) { 'n/a' } else { '{0:N1}' -f $r.Worst }
    $c = if ($null -eq $r.Cpu) { 'n/a' } else { '{0:N1}%' -f $r.Cpu }
    Write-Host ("{0,-14} worst 10 s window {1} fps  process CPU {2}  hash {3}" -f $r.Tag, $w, $c, $r.Hash.Substring(0, 16))
}

$base = @()
$cand = @()
for ($i = 1; $i -le $Runs; $i++) {
    $r = Invoke-Run $BaselineExe $BaselineUserDir "baseline-$i"
    Show-Run $r
    $base += $r
    $r = Invoke-Run $CandidateExe $CandidateUserDir "candidate-$i"
    Show-Run $r
    $cand += $r
}

$all = @($base) + @($cand)
$noResult = @($all | Where-Object { $null -eq $_.Worst })
if ($noResult.Count -gt 0) {
    Write-Host ("FAIL: {0} run(s) with no result ({1})" -f $noResult.Count, (($noResult | ForEach-Object { $_.Tag }) -join ', '))
    exit 1
}

$ok = $true
$hashes = @($all | ForEach-Object { $_.Hash } | Sort-Object -Unique)
if ($hashes.Count -ne 1) {
    $ok = $false
    Write-Host ("FAIL: state hashes differ ({0} different): not the same game" -f $hashes.Count)
}
$bw = [double[]]@($base | ForEach-Object { $_.Worst })
$cw = [double[]]@($cand | ForEach-Object { $_.Worst })
$bm = Get-Median $bw
$cm = Get-Median $cw
$bs = ($bw | Measure-Object -Maximum).Maximum - ($bw | Measure-Object -Minimum).Minimum
$cs = ($cw | Measure-Object -Maximum).Maximum - ($cw | Measure-Object -Minimum).Minimum
Write-Host ("baseline worst 10 s window: median {0:N1} fps, spread {1:N1}" -f $bm, $bs)
Write-Host ("candidate worst 10 s window: median {0:N1} fps, spread {1:N1}" -f $cm, $cs)
if ($cm -lt ($bm - $bs)) {
    $ok = $false
    Write-Host ("FAIL: candidate is slower: {0:N1} < {1:N1} - {2:N1} fps" -f $cm, $bm, $bs)
}
if ($ok) {
    Write-Host 'PASS: state hashes equal, candidate not slower than the baseline spread'
    exit 0
}
exit 1
