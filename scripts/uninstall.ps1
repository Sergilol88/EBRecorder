$ErrorActionPreference = 'Stop'

if (Get-Process obs64 -ErrorAction SilentlyContinue) {
    Write-Host 'OBS Studio is running. Close OBS and run the uninstaller again.' -ForegroundColor Yellow
    exit 2
}

$Target = Join-Path $env:ProgramData 'obs-studio\plugins\eb-recorder'
if (Test-Path $Target) {
    Remove-Item $Target -Recurse -Force
    Write-Host "Removed plugin: $Target"
} else {
    Write-Host 'EB Recorder is not installed in ProgramData.'
}

# EB Recorder 0.3+ stores only its small settings.ini here through the standard
# OBS per-plugin config path. Remove our own folder as part of a full uninstall.
$ConfigTarget = Join-Path $env:APPDATA 'obs-studio\plugin_config\eb-recorder'
if (Test-Path $ConfigTarget) {
    Remove-Item $ConfigTarget -Recurse -Force
    Write-Host "Removed settings: $ConfigTarget"
}

Write-Host 'No registry entries, services or PATH changes were created by EB Recorder.'
