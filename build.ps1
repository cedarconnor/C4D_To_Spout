# Builds the C4D to Spout plugin.
#   1. Extracts the C4D SDK from the installed Cinema 4D (once) into external/c4d_sdk_2026.
#   2. Builds the static SpoutDX lib (plugin/spoutdx_lib) into build/spoutdx.
#   3. Configures the C4D SDK with plugin/custom_paths.txt and builds the module.
# Output: external/c4d_sdk_2026/_build_x64_v143/bin/<Config>/plugins/c4d_to_spout
param(
	[ValidateSet("Release", "Debug")] [string]$Config = "Release",
	[string]$C4DInstall = "C:\Program Files\Maxon Cinema 4D 2026"
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$sdk = Join-Path $root "external\c4d_sdk_2026"
$generator = "Visual Studio 17 2022"

if (-not (Test-Path (Join-Path $sdk "CMakeLists.txt"))) {
	Write-Host "Extracting C4D SDK from $C4DInstall\sdk.zip"
	Expand-Archive (Join-Path $C4DInstall "sdk.zip") -DestinationPath $sdk -Force
}

if (-not (Test-Path (Join-Path $root "external\Spout2\SPOUTSDK"))) {
	git -C $root submodule update --init external/Spout2
}

# SpoutDX static lib (/MD, matching the C4D SDK).
$spoutBuild = Join-Path $root "build\spoutdx"
cmake -S (Join-Path $root "plugin\spoutdx_lib") -B $spoutBuild -G $generator -A x64
if ($LASTEXITCODE) { throw "SpoutDX configure failed" }
cmake --build $spoutBuild --config $Config
if ($LASTEXITCODE) { throw "SpoutDX build failed" }

# C4D SDK + our module.
$sdkBuild = Join-Path $sdk "_build_x64_v143"
cmake -S $sdk -B $sdkBuild -G $generator -A "x64,version=10.0.22621.0" -T v143 `
	"-DMAXON_SDK_CUSTOM_PATHS_FILE=$(Join-Path $root 'plugin\custom_paths.txt')" `
	"-DMAXON_C4D_EXECUTABLE=$(Join-Path $C4DInstall 'Cinema 4D.exe')"
if ($LASTEXITCODE) { throw "C4D SDK configure failed" }
cmake --build $sdkBuild --config $Config --target c4d_to_spout
if ($LASTEXITCODE) { throw "Plugin build failed" }

Write-Host "Built: $(Join-Path $sdkBuild "bin\$Config\plugins\c4d_to_spout")"
