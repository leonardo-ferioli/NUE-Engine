# Builds and runs the Windows backend tests for every supported configuration.
#   pwsh build_and_test.ps1                      # clang x64 + clang x86 (the game launcher is x86)
#   pwsh build_and_test.ps1 -Configs msvc-x64    # one configuration
# Configurations: clang-x64, clang-x86, msvc-x64, msvc-x86. MSVC ones use the Visual Studio Build Tools found by vswhere.
param(
    [string[]]$Configs = @('clang-x64', 'clang-x86'),
    [string]$BuildRoot = ''
)
$ErrorActionPreference = 'Stop'
$Configs = @($Configs | ForEach-Object { $_ -split ',' })
$src = Split-Path -Parent $PSScriptRoot
if (-not $BuildRoot) { $BuildRoot = Join-Path $env:TEMP 'nue_win_build' }

function Enter-VsEnvironment([string]$arch) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products '*' -property installationPath
    $bat = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
    $vars = cmd /c "`"$bat`" $arch >nul && set"
    foreach ($line in $vars) { if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] } }
}

$failed = @()
foreach ($cfg in $Configs) {
    $compiler, $arch = $cfg -split '-'
    $dir = Join-Path $BuildRoot $cfg
    Write-Host "=== $cfg -> $dir"
    $saved = @{}; foreach ($k in 'INCLUDE', 'LIB', 'LIBPATH', 'PATH') { $saved[$k] = [Environment]::GetEnvironmentVariable($k) }
    try {
        if ($compiler -eq 'msvc') {
            Enter-VsEnvironment $(if ($arch -eq 'x86') { 'x86' } else { 'x64' })
            & cmake -S $src -B $dir -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl
        } else {
            $flags = if ($arch -eq 'x86') { '-m32 -msse2' } else { '' }
            & cmake -S $src -B $dir -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ "-DCMAKE_CXX_FLAGS=$flags"
        }
        if ($LASTEXITCODE) { throw "configure failed" }
        & cmake --build $dir
        if ($LASTEXITCODE) { throw "build failed" }
        & ctest --test-dir $dir --output-on-failure
        if ($LASTEXITCODE) { throw "tests failed" }
    } catch { Write-Host "FAILED $cfg : $_"; $failed += $cfg }
    finally { foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) } }
}
if ($failed) { Write-Host "Failed: $($failed -join ', ')"; exit 1 }
Write-Host 'All configurations passed.'
