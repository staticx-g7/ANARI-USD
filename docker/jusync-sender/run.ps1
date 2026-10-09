# Run the jusync-sender rig on this machine. Tries GPU passthrough first
# (Docker Desktop + WSL2 NVIDIA driver) and falls back to plain CPU.
#
#   .\docker\jusync-sender\run.ps1                  # looped live updates (default)
#   .\docker\jusync-sender\run.ps1 -OneShot         # single pass, then exit
#   .\docker\jusync-sender\run.ps1 -Interactive     # bash shell instead of sender
#
# Then point the UE JUSYNC spawner at tcp://localhost:5556 and open
# http://localhost:6080/vnc.html (password jusync) for the desktop + ParaView.
param(
    [string]$Tag = "dev",
    [string]$SenderBin = "anariTutorialUsdTime",
    [switch]$OneShot,
    [switch]$Interactive,
    [switch]$NoGpu
)
$ErrorActionPreference = "Stop"

docker version | Out-Null   # fail fast if the daemon is down

$envArgs = @("-e", "JUSYNC_LOOP=$(if ($OneShot) { '0' } else { '1' })",
             "-e", "JUSYNC_SENDER_BIN=$SenderBin",
             "-e", "JUSYNC_VNC_PASSWORD=jusync")
$removeOld = docker ps -aq -f name=jusync-sender
if ($removeOld) { docker rm -f $removeOld | Out-Null }

function Start-Sender([string[] $gpuArgs]) {
    $cmd = @("run", "-d", "--name", "jusync-sender",
             "-p", "5556:5556", "-p", "6080:6080", "-p", "5901:5901",
             "--shm-size", "1g") + $gpuArgs + $envArgs +
            @("jusync-sender:$Tag")
    if ($Interactive) { $cmd = ($cmd[0..($cmd.Count-2)]) + @("-it") + $cmd[-1] + @("bash") }
    docker @cmd
}

if (-not $NoGpu) {
    Write-Host "Trying with --gpus all ..." -ForegroundColor Cyan
    $id = Start-Sender @("--gpus", "all") 2>$null
    if ($LASTEXITCODE -eq 0 -and $id) {
        Write-Host "GPU mode: $id" -ForegroundColor Green
        $mode = "GPU"
    }
}
if (-not $id) {
    Write-Host "Falling back to CPU/software GL (llvmpipe) ..." -ForegroundColor Yellow
    $id = Start-Sender @()
    if ($LASTEXITCODE -ne 0) { throw "docker run failed" }
    $mode = "CPU"
}

Write-Host ""
Write-Host "jusync-sender ($mode mode) running: $id" -ForegroundColor Green
Write-Host "  ZMQ broker : tcp://localhost:5556   <- UE JUSYNC spawner endpoint"
Write-Host "  noVNC      : http://localhost:6080/vnc.html   (password: jusync)"
Write-Host "  VNC        : localhost:5901"
Write-Host "  logs       : docker logs -f jusync-sender"
if (-not $Interactive) {
    Write-Host ""
    Write-Host "First 15 lines of sender output:" -ForegroundColor Cyan
    Start-Sleep -Seconds 3
    docker logs jusync-sender 2>&1 | Select-Object -First 15
}
