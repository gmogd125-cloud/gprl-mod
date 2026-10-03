# Build + install the GPRL Geode mod using the portable MSVC toolchain on D:
# (same pattern as D:\GeodeMods\frame-perfect-counter\build.ps1). geode build installs the
# .geode into the game's mods folder automatically.
$ErrorActionPreference = 'Continue'
. 'D:\GeodeMods\_msvc\msvc-env.ps1'
$env:GEODE_SDK = 'C:\Users\gacue\Desktop\GeodeSDK'
$env:CPM_SOURCE_CACHE = 'D:\GeodeMods\_cpm'
Set-Location $PSScriptRoot
geode build --ninja --config RelWithDebInfo
# propagate the build result (a failed compile used to exit 0)
exit $LASTEXITCODE
