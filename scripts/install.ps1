$ErrorActionPreference = 'Stop'

$PluginVersion = '0.3.1'
$BuildRevision = 'r6.3'

if (Get-Process obs64 -ErrorAction SilentlyContinue) {
    Write-Host 'OBS Studio is running. Close OBS and run the installer again.' -ForegroundColor Yellow
    exit 2
}

$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Source = Join-Path $Here 'eb-recorder'
if (-not (Test-Path $Source -PathType Container)) {
    throw "Plugin folder not found: $Source"
}

# Use the isolated ProgramData plugin layout validated with OBS Studio 32.2.2:
#   %ProgramData%\obs-studio\plugins\eb-recorder\bin\64bit\eb-recorder.dll
#   %ProgramData%\obs-studio\plugins\eb-recorder\data\...
$SourceDll = Join-Path $Source 'bin\64bit\eb-recorder.dll'
$SourceData = Join-Path $Source 'data'
$SourceEnLocale = Join-Path $SourceData 'locale\en-US.ini'
$SourceRuLocale = Join-Path $SourceData 'locale\ru-RU.ini'
$SourceVersion = Join-Path $Source 'VERSION.txt'

foreach ($Required in @($SourceDll, $SourceEnLocale, $SourceRuLocale)) {
    if (-not (Test-Path $Required -PathType Leaf)) {
        throw "Release package is incomplete. Required file not found: $Required"
    }
}

$PluginRoot = Join-Path $env:ProgramData 'obs-studio\plugins'
$Target = Join-Path $PluginRoot 'eb-recorder'
$NewTarget = "$Target.__new"
$BackupTarget = "$Target.__old"
New-Item -ItemType Directory -Path $PluginRoot -Force | Out-Null

$Updating = Test-Path $Target
$OldVersionFile = Join-Path $Target 'VERSION.txt'
$OldVersion = if (Test-Path $OldVersionFile -PathType Leaf) {
    (Get-Content $OldVersionFile -Raw).Trim()
} elseif ($Updating) {
    '0.1.0 or earlier/unknown'
} else {
    $null
}

# Stage the complete release folder exactly as produced by CMake. This deliberately
# replaces any broken r4.2/r4.2.1 flat-layout installation as well as older legacy
# installations, so stale DLL copies cannot survive the update.
Remove-Item $NewTarget -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item $BackupTarget -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $NewTarget -Force | Out-Null
Copy-Item (Join-Path $Source '*') $NewTarget -Recurse -Force

if (-not (Test-Path (Join-Path $NewTarget 'VERSION.txt') -PathType Leaf)) {
    Set-Content -Path (Join-Path $NewTarget 'VERSION.txt') -Value $PluginVersion -Encoding ASCII
}

$InstalledDll = Join-Path $NewTarget 'bin\64bit\eb-recorder.dll'
$InstalledEnLocale = Join-Path $NewTarget 'data\locale\en-US.ini'
$InstalledRuLocale = Join-Path $NewTarget 'data\locale\ru-RU.ini'
foreach ($Required in @($InstalledDll, $InstalledEnLocale, $InstalledRuLocale)) {
    if (-not (Test-Path $Required -PathType Leaf)) {
        Remove-Item $NewTarget -Recurse -Force -ErrorAction SilentlyContinue
        throw "Staged installation failed validation. Required file not found: $Required"
    }
}

try {
    if ($Updating) {
        Move-Item $Target $BackupTarget
    }

    Move-Item $NewTarget $Target

    # Validate the final paths before deleting the rollback copy.
    $FinalDll = Join-Path $Target 'bin\64bit\eb-recorder.dll'
    $FinalEnLocale = Join-Path $Target 'data\locale\en-US.ini'
    $FinalRuLocale = Join-Path $Target 'data\locale\ru-RU.ini'
    foreach ($Required in @($FinalDll, $FinalEnLocale, $FinalRuLocale)) {
        if (-not (Test-Path $Required -PathType Leaf)) {
            throw "Final installation failed validation. Required file not found: $Required"
        }
    }

    # A flat-layout DLL from r4.2/r4.2.1 must not exist in the new installation.
    $UnexpectedFlatDll = Join-Path $Target 'eb-recorder.dll'
    if (Test-Path $UnexpectedFlatDll -PathType Leaf) {
        throw "Unexpected flat-layout DLL remained after installation: $UnexpectedFlatDll"
    }

    Remove-Item $BackupTarget -Recurse -Force -ErrorAction SilentlyContinue
} catch {
    # If the new target was already swapped into place, remove it before restoring
    # the previous installation. This makes the directory swap rollback-safe.
    if (Test-Path $Target) {
        Remove-Item $Target -Recurse -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path $BackupTarget) {
        Move-Item $BackupTarget $Target
    }
    Remove-Item $NewTarget -Recurse -Force -ErrorAction SilentlyContinue
    throw
}

if ($Updating) {
    Write-Host "Updated EB Recorder: $OldVersion -> $PluginVersion ($BuildRevision)" -ForegroundColor Green
} else {
    Write-Host "Installed EB Recorder $PluginVersion ($BuildRevision)" -ForegroundColor Green
}
Write-Host 'Layout: ProgramData plugin layout (bin\64bit)'
Write-Host "Plugin folder: $Target"
Write-Host "DLL: $(Join-Path $Target 'bin\64bit\eb-recorder.dll')"
Write-Host "Locale: $(Join-Path $Target 'data\locale')"
Write-Host 'The previous eb-recorder folder was replaced completely; no stale DLL from an older build remains.'
Write-Host 'Removal is simple: close OBS and run UNINSTALL.cmd. It removes the plugin and its small OBS plugin_config settings folder.'
