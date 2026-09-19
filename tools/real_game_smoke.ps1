# Real-game smoke: run each local Artemis project headless for a fixed frame
# budget and fail on the signals that mean "this game does not work".
#
# Why a script and not a ctest entry: the projects are commercial packages that
# never ship with the repo (docs/TESTING.md §2), so this stays a manual QA
# gate with the paths passed in — the same shape as the OA_TEST_*_PFS tests,
# but for a whole local library at once.
#
#   .\tools\real_game_smoke.ps1 -Game 'D:\Games\Pomelo\gzsq\root.pfs', `
#       'D:\Games\Pomelo\tmny31\root.pfs' -Frames 300
#
# Exit code 0 = every game booted clean; 1 = at least one failure (printed).
param(
    [Parameter(Mandatory = $true)]
    [string[]]$Game,

    [int]$Frames = 300,

    [string]$Binary = '',

    [string]$SaveRoot = '',

    [switch]$Profile,

    [switch]$KeepSaveRoot
)

$ErrorActionPreference = 'Stop'

function Resolve-RepoRoot {
    return Split-Path -Parent $PSScriptRoot
}

function Resolve-SmokeBinary {
    param([string]$Requested, [string]$RepoRoot)
    if ($Requested) { return (Resolve-Path -LiteralPath $Requested).Path }
    $candidates = @(
        (Join-Path $RepoRoot 'build_sdl2/src/app/openartemis.exe'),
        (Join-Path $RepoRoot 'build_sdl2/src/app/openartemis'),
        (Join-Path $RepoRoot 'build/default/src/app/openartemis'),
        (Join-Path $RepoRoot 'build/default/src/app/openartemis.exe')
    )
    foreach ($c in $candidates) {
        if (Test-Path -LiteralPath $c) { return (Resolve-Path -LiteralPath $c).Path }
    }
    throw "openartemis host not found under $RepoRoot; build it or pass -Binary."
}

# Signals that always mean a broken run. `[app] win exception ...` lines are
# NOT in this list: the Windows host prints that marker for internally caught
# exceptions (Lua errors the runtime tolerates), so a healthy boot emits them.
$failPatterns = @(
    'tick error',
    'label not found',
    'calllua error',
    'terminate called',
    'filesystem error',
    'assertion failed',
    'cannot open project',
    'no project given'
)

$repoRoot = Resolve-RepoRoot
$exe = Resolve-SmokeBinary -Requested $Binary -RepoRoot $repoRoot
if (-not $SaveRoot) {
    $SaveRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("oa_smoke_" + (Get-Date -Format 'yyyyMMdd_HHmmss'))
}
New-Item -ItemType Directory -Force -Path $SaveRoot | Out-Null

$results = @()
foreach ($game in $Game) {
    if (-not (Test-Path -LiteralPath $game)) {
        $results += [pscustomobject]@{ Game = $game; Result = 'MISSING'; Frames = 0; Note = 'path not found' }
        continue
    }
    $name = Split-Path -Leaf (Split-Path -Parent $game)
    $gameSave = Join-Path $SaveRoot $name
    New-Item -ItemType Directory -Force -Path $gameSave | Out-Null

    $env:OA_SAVE_ROOT = $gameSave
    if ($Profile) { $env:OA_PROFILE = '1' } else { Remove-Item Env:OA_PROFILE -ErrorAction SilentlyContinue }
    $log = & $exe --headless --frames $Frames $game 2>&1 | Out-String

    if ($Profile) {
        $logFile = Join-Path $gameSave 'profile.log'
        Set-Content -LiteralPath $logFile -Value $log -Encoding UTF8
        $lines = ($log -split "`n") | Where-Object { $_ -match '^\[prof\]' }
        if ($lines) {
            Write-Host "----- $name profile -----"
            $lines | Select-Object -Last 3 | ForEach-Object { Write-Host $_ }
        }
    }

    $hit = $null
    foreach ($p in $failPatterns) {
        if ($log -match [regex]::Escape($p)) { $hit = $p; break }
    }
    $cleanEnd = ($log -match '\[app\] bye \(frames=')
    $status = if ($hit) { 'FAIL' } elseif (-not $cleanEnd) { 'NO-EXIT' } else { 'OK' }
    $note = if ($hit) { $hit } elseif (-not $cleanEnd) { 'no clean [app] bye' } else { '' }

    $results += [pscustomobject]@{ Game = $name; Result = $status; Frames = $Frames; Note = $note }
    if ($status -ne 'OK') {
        Write-Host "----- $name output -----"
        Write-Host $log
    }
}

$results | Format-Table -AutoSize
$failed = @($results | Where-Object { $_.Result -ne 'OK' })
if (-not $KeepSaveRoot -and $SaveRoot -like '*oa_smoke_*') {
    Remove-Item -LiteralPath $SaveRoot -Recurse -Force -ErrorAction SilentlyContinue
} else {
    Write-Host "save roots kept under $SaveRoot"
}

if ($failed.Count -gt 0) {
    Write-Host ("real_game_smoke: {0}/{1} game(s) failed" -f $failed.Count, $results.Count)
    exit 1
}
Write-Host ("real_game_smoke: {0}/{0} game(s) booted clean" -f $results.Count)
exit 0
