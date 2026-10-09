# Build the jusync-sender image from the ANARI-USD repo root.
# First build compiles the ANARI SDK + OpenUSD 25.11 + this repo: expect
# 30-90 min and ~10 GB of image. Later builds reuse the layer cache.
#
#   .\docker\jusync-sender\build.ps1
#   .\docker\jusync-sender\build.ps1 -Tag mytag -NoCache
param(
    [string]$Tag = "dev",
    [switch]$NoCache
)
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Write-Host "Building jusync-sender:$Tag from $repo" -ForegroundColor Cyan
$dockerArgs = @("build", "-f", "docker/jusync-sender/Dockerfile", "-t", "jusync-sender:$Tag")
if ($NoCache) { $dockerArgs += "--no-cache" }
$dockerArgs += "."
Push-Location $repo
try { & docker @dockerArgs; if ($LASTEXITCODE -ne 0) { throw "docker build failed" } }
finally { Pop-Location }
Write-Host "OK: jusync-sender:$Tag" -ForegroundColor Green
