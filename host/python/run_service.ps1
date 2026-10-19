param(
    [string]$Config = "config.local.json"
)

$ErrorActionPreference = "Stop"
$python = "C:\Users\jerry\AppData\Local\Programs\Python\Python312\python.exe"
$app = Join-Path $PSScriptRoot "app.py"
$configPath = Join-Path $PSScriptRoot $Config
$log = Join-Path $PSScriptRoot "data\service_supervisor.log"

New-Item -ItemType Directory -Force (Split-Path $log) | Out-Null

while ($true) {
    $stamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
    Add-Content -Path $log -Value "[$stamp] starting Python service"
    & $python $app --config $configPath *>> $log
    $stamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
    Add-Content -Path $log -Value "[$stamp] Python service exited; restarting in 2 s"
    Start-Sleep -Seconds 2
}
