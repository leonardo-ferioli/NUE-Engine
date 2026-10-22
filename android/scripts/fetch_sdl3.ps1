# Fetches the SDL3 source tree used by the Android build (Java classes + libSDL3.so) into nue_engine/android/.deps/SDL3.
# Needs git.   Usage:  powershell -File scripts/fetch_sdl3.ps1 [-Tag release-3.4.18]
param([string]$Tag = "release-3.4.18")
$target = Join-Path $PSScriptRoot "..\.deps\SDL3"
if (Test-Path (Join-Path $target "CMakeLists.txt")) { Write-Host "SDL3 already present at $target"; exit 0 }
New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
git clone --depth 1 --branch $Tag https://github.com/libsdl-org/SDL.git $target
if ($LASTEXITCODE -ne 0) { Write-Error "git clone failed"; exit 1 }
Write-Host "SDL3 $Tag ready in $target"
