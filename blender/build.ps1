# Packages the LatLong Spout Blender extension into blender/dist/latlong_spout-<version>.zip.
# Downloads the SpoutGL wheels listed in blender_manifest.toml, then runs Blender's extension builder.
param(
    [string]$Blender = "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe"
)
$ErrorActionPreference = 'Stop'
$src = Join-Path $PSScriptRoot 'latlong_spout'
$wheels = Join-Path $src 'wheels'
$dist = Join-Path $PSScriptRoot 'dist'
New-Item -ItemType Directory -Force $wheels, $dist | Out-Null

$python = Get-ChildItem (Split-Path $Blender) -Recurse -Filter python.exe |
    Where-Object { $_.FullName -match '\\python\\bin\\' } | Select-Object -First 1
if (-not $python) { throw "Blender's bundled python.exe not found next to $Blender" }

foreach ($py in '311', '313') {
    & $python.FullName -m pip download 'SpoutGL==0.1.1' --only-binary=:all: --no-deps `
        --python-version $py --platform win_amd64 -d $wheels --quiet
    if ($LASTEXITCODE) { throw "pip download failed for cp$py" }
}

& $Blender --factory-startup --command extension build --source-dir $src --output-dir $dist
if ($LASTEXITCODE) { throw 'extension build failed' }
Get-ChildItem $dist -Filter *.zip
