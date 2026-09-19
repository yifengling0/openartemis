param(
    [int]$Jobs = 8,
    [switch]$ConfigureOnly
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir = Join-Path $root 'build_sdl2'
$mingwBin = 'C:\msys64\mingw64\bin'
$usrBin = 'C:\msys64\usr\bin'
$cmakeExe = Join-Path $mingwBin 'cmake.exe'
$makeExe = Join-Path $mingwBin 'mingw32-make.exe'

if (-not (Test-Path $cmakeExe)) { throw "MSYS2 MinGW cmake not found: $cmakeExe" }
if (-not (Test-Path $makeExe)) { throw "mingw32-make not found: $makeExe" }

$env:PATH = "$mingwBin;$usrBin;" + $env:PATH
$env:PKG_CONFIG = Join-Path $mingwBin 'pkg-config.exe'
$env:PKG_CONFIG_PATH = 'C:\msys64\mingw64\lib\pkgconfig'

New-Item -ItemType Directory -Path $buildDir -Force | Out-Null

Write-Host "Configuring OpenArtemis Windows SDL2..." -ForegroundColor Yellow
& $cmakeExe -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DOA_USE_SDL2=ON -S $root -B $buildDir
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
if ($ConfigureOnly) { exit 0 }

Write-Host "Building openartemis.exe..." -ForegroundColor Yellow
& $makeExe -C $buildDir -j $Jobs
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

$exe = Join-Path $buildDir 'openartemis.exe'
if (-not (Test-Path $exe)) {
    $exe = Join-Path $buildDir 'src\app\openartemis.exe'
}
if (-not (Test-Path $exe)) { throw "openartemis.exe not found after build" }
Write-Host "Built: $exe" -ForegroundColor Green

$copyScript = Join-Path $root 'copy-mingw-runtime-dlls.py'
$python = Join-Path $root '..\..\\.venv\Scripts\python.exe'
if (-not (Test-Path $python)) { $python = 'python' }
Write-Host "Copying MinGW runtime DLLs next to the exe..." -ForegroundColor Yellow
& $python $copyScript --exe $exe --mingw-bin $mingwBin
if ($LASTEXITCODE -ne 0) { throw "Runtime DLL copy failed" }
