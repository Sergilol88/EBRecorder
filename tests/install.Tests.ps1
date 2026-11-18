$ErrorActionPreference = 'Stop'

# Exercise the real installer using real temporary directories. Only OBS process
# discovery and selected directory renames are intercepted; ProgramData is scoped
# to this test process and restored even if an assertion fails.
$Installer = Join-Path (Split-Path -Parent $PSScriptRoot) 'scripts\install.ps1'
$TestRoot = Join-Path ([IO.Path]::GetTempPath()) ('eb-recorder-install-tests-' + [guid]::NewGuid())
$OriginalProgramData = $env:ProgramData
$global:EBRecorderInstallTestState = @{ MoveCount = 0; FailedMoves = @() }

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Get-Process { param([string]$Name) return $null }

function Move-Item {
    param(
        [Parameter(Position = 0)][string]$Path,
        [Parameter(Position = 1)][string]$Destination,
        [string]$LiteralPath
    )
    $global:EBRecorderInstallTestState.MoveCount++
    if ($global:EBRecorderInstallTestState.FailedMoves -contains $global:EBRecorderInstallTestState.MoveCount) {
        throw "Injected rename failure $($global:EBRecorderInstallTestState.MoveCount)"
    }
    $MoveSource = if ($LiteralPath) { $LiteralPath } else { $Path }
    Microsoft.PowerShell.Management\Move-Item -LiteralPath $MoveSource -Destination $Destination
}

function Test-InstallScenario(
    [string]$Name, [bool]$Existing, [int[]]$FailMoves,
    [bool]$LegacyFile = $false, [bool]$ExistingBackup = $false
) {
    $Scenario = Join-Path $TestRoot $Name
    $Package = Join-Path $Scenario 'package'
    $env:ProgramData = Join-Path $Scenario 'programdata'
    $Target = Join-Path $env:ProgramData 'obs-studio\plugins\eb-recorder'
    $Backup = "$Target.__old"
    $Source = Join-Path $Package 'eb-recorder'
    New-Item -ItemType Directory -Path (Join-Path $Source 'data\locale') -Force | Out-Null
    Copy-Item -LiteralPath $Installer -Destination (Join-Path $Package 'install.ps1')
    Set-Content -LiteralPath (Join-Path $Source 'eb-recorder.dll') -Value 'new plugin'
    Set-Content -LiteralPath (Join-Path $Source 'data\locale\en-US.ini') -Value 'English'
    Set-Content -LiteralPath (Join-Path $Source 'data\locale\ru-RU.ini') -Value 'Russian'
    if ($LegacyFile) {
        New-Item -ItemType Directory -Path (Join-Path $Source 'bin\64bit') -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $Source 'bin\64bit\eb-recorder.dll') -Value 'legacy'
    }
    if ($Existing) {
        New-Item -ItemType Directory -Path $Target -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $Target 'previous.txt') -Value 'previous installation'
    }
    if ($ExistingBackup) {
        New-Item -ItemType Directory -Path $Backup -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $Backup 'recovery.txt') -Value 'saved backup'
    }
    $global:EBRecorderInstallTestState.MoveCount = 0
    $global:EBRecorderInstallTestState.FailedMoves = $FailMoves
    $Failure = $null
    try { & (Join-Path $Package 'install.ps1') | Out-Null } catch { $Failure = $_ }
    $ShouldFail = $FailMoves.Count -gt 0 -or $LegacyFile -or $ExistingBackup
    Assert-True (($null -ne $Failure) -eq $ShouldFail) "$Name : unexpected installation outcome: $Failure"

    if ($ExistingBackup) {
        Assert-True (Test-Path -LiteralPath (Join-Path $Backup 'recovery.txt')) "$Name : recovery backup was deleted"
        Assert-True (Test-Path -LiteralPath (Join-Path $Target 'previous.txt')) "$Name : original installation was changed"
    } elseif ($FailMoves -contains 3) {
        Assert-True (Test-Path -LiteralPath (Join-Path $Backup 'previous.txt')) "$Name : rollback failure lost the backup"
        Assert-True ($Failure.ToString().Contains('Rollback failed')) "$Name : rollback failure was not reported"
    } elseif ($ShouldFail -and $Existing) {
        Assert-True (Test-Path -LiteralPath (Join-Path $Target 'previous.txt')) "$Name : previous installation was lost"
        Assert-True (-not (Test-Path -LiteralPath $Backup)) "$Name : backup was not restored"
    } elseif ($ShouldFail) {
        Assert-True (-not (Test-Path -LiteralPath $Target)) "$Name : failed fresh install left a target"
    } else {
        Assert-True ((Get-Content -LiteralPath (Join-Path $Target 'eb-recorder.dll') -Raw).Trim() -eq 'new plugin') "$Name : new plugin is missing"
        Assert-True (-not (Test-Path -LiteralPath (Join-Path $Target 'previous.txt'))) "$Name : stale installation survived"
        Assert-True (-not (Test-Path -LiteralPath $Backup)) "$Name : obsolete backup survived"
    }
    Write-Host "PASS: $Name"
}

try {
    Test-InstallScenario 'fresh-install' $false @()
    Test-InstallScenario 'update' $true @()
    Test-InstallScenario 'backup-rename-fails' $true @(1)
    Test-InstallScenario 'new-rename-fails' $true @(2)
    Test-InstallScenario 'validation-fails' $true @() $true
    Test-InstallScenario 'fresh-rename-fails' $false @(1)
    Test-InstallScenario 'rollback-rename-fails' $true @(2, 3)
    Test-InstallScenario 'existing-recovery-backup' $true @() $false $true
} finally {
    $env:ProgramData = $OriginalProgramData
    $ResolvedTestRoot = [IO.Path]::GetFullPath($TestRoot)
    $TempPrefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $ResolvedTestRoot.StartsWith($TempPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean a test directory outside TEMP: $ResolvedTestRoot"
    }
    Remove-Item -LiteralPath $ResolvedTestRoot -Recurse -Force -ErrorAction SilentlyContinue
}
