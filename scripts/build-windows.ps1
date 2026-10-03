param(
    [switch]$Clean,
    [switch]$FullClean
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$Root = Split-Path -Parent $PSScriptRoot
$ParentRoot = Split-Path -Parent $Root

$ObsVersion = '32.2.2'
$ObsZipName = "$ObsVersion.zip"
$ObsZipUrl = "https://github.com/obsproject/obs-studio/archive/refs/tags/$ObsVersion.zip"
$ObsZipSha256 = 'f15f001f1fa526405318835f44f9910046502f496ebc3a30d5296a5018b831aa'

$DepsVersion = '2026-07-15'
$ObsDepsZipName = "windows-deps-$DepsVersion-x64.zip"
$ObsDepsZipUrl = "https://github.com/obsproject/obs-deps/releases/download/$DepsVersion/$ObsDepsZipName"
$ObsDepsZipSha256 = '6f90e9598fa10cff5ad23cdcfae49b87868c07bf896b02cd464582b4ce2f2ba9'

$QtZipName = "windows-deps-qt6-$DepsVersion-x64.zip"
$QtZipUrl = "https://github.com/obsproject/obs-deps/releases/download/$DepsVersion/$QtZipName"
$QtZipSha256 = '7c7f985711d80467bdc1795b6592275a27d5b0e5a2c7a61db1f2c1d08d6a5579'

$CacheRoot = Join-Path $Root '.cache'
$DownloadDir = Join-Path $CacheRoot 'downloads'
$SourceDir = Join-Path $CacheRoot 'src'
$ObsSource = Join-Path $SourceDir "obs-studio-$ObsVersion"
$ObsBuild = Join-Path $CacheRoot 'obs-build-x64'
$ObsSdk = Join-Path $CacheRoot 'obs-sdk-x64'
$PluginBuild = Join-Path $Root 'build_x64'
$StagingDir = Join-Path $Root 'staging'
$DistDir = Join-Path $Root 'dist'

function Get-VS2026Environment {
    $VsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $VsWhere)) {
        throw "vswhere.exe was not found at '$VsWhere'. Visual Studio Installer is required."
    }

    $VSPath = (& $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
    $VSVersion = (& $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion).Trim()
    if (-not $VSPath -or -not $VSVersion) {
        throw 'Visual Studio with the Desktop development with C++ workload was not found.'
    }

    $VSMajor = [int]($VSVersion.Split('.')[0])
    if ($VSMajor -ne 18) {
        throw "This build package targets Visual Studio 2026 (18.x), but found Visual Studio $VSVersion at '$VSPath'."
    }

    $CMakeExe = Join-Path $VSPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (-not (Test-Path $CMakeExe)) {
        throw "Visual Studio's bundled CMake was not found at '$CMakeExe'."
    }

    $CMakeFirstLine = (& $CMakeExe --version | Select-Object -First 1)
    if ($CMakeFirstLine -notmatch 'cmake version\s+([0-9]+)\.([0-9]+)') {
        throw "Could not parse CMake version from: $CMakeFirstLine"
    }
    $CMakeMajor = [int]$Matches[1]
    $CMakeMinor = [int]$Matches[2]
    if (($CMakeMajor -lt 4) -or (($CMakeMajor -eq 4) -and ($CMakeMinor -lt 2))) {
        throw "CMake 4.2 or newer is required for Visual Studio 18 2026. Found: $CMakeFirstLine"
    }

    [PSCustomObject]@{
        VSPath = $VSPath
        VSVersion = $VSVersion
        CMakeExe = $CMakeExe
        CMakeVersionLine = $CMakeFirstLine
    }
}

function Test-Sha256([string]$Path, [string]$Expected) {
    if (-not (Test-Path $Path -PathType Leaf)) { return $false }
    $Actual = (Get-FileHash -Path $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    return $Actual -eq $Expected.ToLowerInvariant()
}

function Find-ReusableArchive([string]$FileName, [string]$ExpectedSha) {
    if (-not (Test-Path $ParentRoot)) { return $null }

    $Candidates = New-Object System.Collections.Generic.List[string]
    Get-ChildItem $ParentRoot -Directory -Filter 'EBRecorder-*' -ErrorAction SilentlyContinue | ForEach-Object {
        if ($_.FullName -ne $Root) {
            $Candidates.Add((Join-Path $_.FullName ".deps\$FileName"))
            $Candidates.Add((Join-Path $_.FullName ".cache\downloads\$FileName"))
            $Candidates.Add((Join-Path $_.FullName ".deps\obs-studio-$ObsVersion\.deps\$FileName"))
            $Candidates.Add((Join-Path $_.FullName ".cache\src\obs-studio-$ObsVersion\.deps\$FileName"))
        }
    }

    foreach ($Candidate in $Candidates) {
        if (Test-Sha256 $Candidate $ExpectedSha) {
            return $Candidate
        }
    }
    return $null
}

function Copy-Or-Link([string]$Source, [string]$Destination) {
    if (Test-Path $Destination) { Remove-Item $Destination -Force }
    $DestDir = Split-Path -Parent $Destination
    New-Item -ItemType Directory -Path $DestDir -Force | Out-Null

    try {
        New-Item -ItemType HardLink -Path $Destination -Target $Source -ErrorAction Stop | Out-Null
        Write-Host "Reused cached archive by hard link: $([IO.Path]::GetFileName($Destination))"
    } catch {
        Copy-Item $Source $Destination -Force
        Write-Host "Reused cached archive by copy: $([IO.Path]::GetFileName($Destination))"
    }
}

function Ensure-Archive([string]$FileName, [string]$Url, [string]$Sha256) {
    $Destination = Join-Path $DownloadDir $FileName
    New-Item -ItemType Directory -Path $DownloadDir -Force | Out-Null

    if (Test-Sha256 $Destination $Sha256) {
        Write-Host "Using cached archive: $FileName"
        return $Destination
    }
    if (Test-Path $Destination) {
        Write-Host "Cached archive has the wrong hash, deleting: $FileName" -ForegroundColor Yellow
        Remove-Item $Destination -Force
    }

    $Reusable = Find-ReusableArchive $FileName $Sha256
    if ($Reusable) {
        Write-Host "Found archive from an older EB Recorder build: $Reusable"
        Copy-Or-Link $Reusable $Destination
        return $Destination
    }

    $Temp = "$Destination.download"
    $MaxAttempts = 3
    for ($Attempt = 1; $Attempt -le $MaxAttempts; $Attempt++) {
        Remove-Item $Temp -Force -ErrorAction SilentlyContinue
        Write-Host "Downloading ($Attempt/$MaxAttempts): $Url"
        try {
            Invoke-WebRequest -Uri $Url -OutFile $Temp -UseBasicParsing
        } catch {
            Remove-Item $Temp -Force -ErrorAction SilentlyContinue
            if ($Attempt -eq $MaxAttempts) { throw }
            Write-Host "Download attempt failed; retrying..." -ForegroundColor Yellow
            Start-Sleep -Seconds 2
            continue
        }

        $ActualSha = (Get-FileHash -Path $Temp -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($ActualSha -eq $Sha256.ToLowerInvariant()) {
            Move-Item $Temp $Destination -Force
            Write-Host "Downloaded and verified: $FileName"
            return $Destination
        }

        $ActualSize = (Get-Item $Temp).Length
        Write-Host "SHA256 verification failed for $FileName on attempt $Attempt/$MaxAttempts." -ForegroundColor Yellow
        Write-Host "  expected: $Sha256"
        Write-Host "  actual:   $ActualSha"
        Write-Host "  size:     $ActualSize bytes"
        Remove-Item $Temp -Force -ErrorAction SilentlyContinue
        if ($Attempt -lt $MaxAttempts) { Start-Sleep -Seconds 2 }
    }

    throw "SHA256 verification failed for $FileName after $MaxAttempts attempts."
}

function Ensure-ObsSource([string]$ObsArchive) {
    $CMakeLists = Join-Path $ObsSource 'CMakeLists.txt'
    if (Test-Path $CMakeLists) {
        return
    }

    New-Item -ItemType Directory -Path $SourceDir -Force | Out-Null
    $TempExtract = Join-Path $CacheRoot 'obs-source-extract'
    Remove-Item $TempExtract -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Path $TempExtract -Force | Out-Null

    Write-Host "Extracting OBS Studio $ObsVersion sources..."
    Push-Location $TempExtract
    try {
        & $CMakeExe -E tar xf $ObsArchive
        if ($LASTEXITCODE -ne 0) { throw 'OBS source extraction failed.' }
    } finally {
        Pop-Location
    }
    $Extracted = Join-Path $TempExtract "obs-studio-$ObsVersion"
    if (-not (Test-Path (Join-Path $Extracted 'CMakeLists.txt'))) {
        throw "OBS source archive did not contain the expected obs-studio-$ObsVersion directory."
    }

    Remove-Item $ObsSource -Recurse -Force -ErrorAction SilentlyContinue
    Move-Item $Extracted $ObsSource
    Remove-Item $TempExtract -Recurse -Force -ErrorAction SilentlyContinue
}

function Patch-ObsForSdkBuild {
    $Architecture = Join-Path $ObsSource 'cmake\windows\architecture.cmake'
    if (-not (Test-Path $Architecture)) {
        throw "OBS architecture helper not found: $Architecture"
    }

    $Text = Get-Content $Architecture -Raw
    $Original = 'elseif(OBS_PARENT_ARCHITECTURE STREQUAL x64)'
    $Patched = 'elseif(FALSE AND OBS_PARENT_ARCHITECTURE STREQUAL x64) # EB Recorder SDK build: skip helper Win32 tree'

    if ($Text.Contains($Original)) {
        $Text = $Text.Replace($Original, $Patched)
        [IO.File]::WriteAllText($Architecture, $Text, (New-Object Text.UTF8Encoding($false)))
        Write-Host 'Patched OBS architecture helper: Win32 helper build disabled.'
    } elseif ($Text.Contains($Patched)) {
        Write-Host 'OBS architecture helper already patched.'
    } else {
        throw 'Could not locate the expected x64 helper-build branch in OBS architecture.cmake.'
    }
}

function Seed-ObsDependencyArchive([string]$Archive, [string]$FileName, [string]$Sha256) {
    $ObsDepsDir = Join-Path $ObsSource '.deps'
    New-Item -ItemType Directory -Path $ObsDepsDir -Force | Out-Null
    $Destination = Join-Path $ObsDepsDir $FileName

    if (Test-Sha256 $Destination $Sha256) {
        Write-Host "OBS dependency archive already seeded: $FileName"
        return
    }

    Copy-Or-Link $Archive $Destination
}

function Invoke-CMake([string[]]$Arguments, [string]$FailureMessage) {
    & $CMakeExe @Arguments
    if ($LASTEXITCODE -ne 0) { throw $FailureMessage }
}

$EnvInfo = Get-VS2026Environment
$CMakeExe = $EnvInfo.CMakeExe

Write-Host "Visual Studio: $($EnvInfo.VSVersion)"
Write-Host "Visual Studio path: $($EnvInfo.VSPath)"
Write-Host "CMake: $($EnvInfo.CMakeVersionLine)"
Write-Host "CMake path: $CMakeExe"
Write-Host ''
Write-Host 'EB Recorder r6.1 build strategy:'
Write-Host '  - no obs-plugintemplate bootstrap'
Write-Host '  - one OBS x64 SDK build only'
Write-Host '  - no nested Win32 build'
Write-Host '  - dependency archives are reused from older EB Recorder folders when possible'
Write-Host '  - OBS downloads each missing dependency at most once into its own .deps cache'
Write-Host ''

if ($FullClean) {
    Write-Host 'Full clean: removing all build caches and downloads...'
    Remove-Item $CacheRoot -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item $PluginBuild -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item $StagingDir -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item $DistDir -Recurse -Force -ErrorAction SilentlyContinue
} elseif ($Clean) {
    Write-Host 'Clean: removing generated build trees, keeping downloaded archives and extracted dependencies...'
    Remove-Item $ObsBuild -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item $ObsSdk -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item $PluginBuild -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item $StagingDir -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item $DistDir -Recurse -Force -ErrorAction SilentlyContinue
}

$ObsArchive = Ensure-Archive $ObsZipName $ObsZipUrl $ObsZipSha256
$ObsDepsArchive = Ensure-Archive $ObsDepsZipName $ObsDepsZipUrl $ObsDepsZipSha256
$QtArchive = Ensure-Archive $QtZipName $QtZipUrl $QtZipSha256

Ensure-ObsSource $ObsArchive
Patch-ObsForSdkBuild
Seed-ObsDependencyArchive $ObsDepsArchive $ObsDepsZipName $ObsDepsZipSha256
Seed-ObsDependencyArchive $QtArchive $QtZipName $QtZipSha256

$ObsDepsPrefix = Join-Path $ObsSource ".deps\obs-deps-$DepsVersion-x64"
$QtPrefix = Join-Path $ObsSource ".deps\obs-deps-qt6-$DepsVersion-x64"

if (-not ((Test-Path (Join-Path $ObsSdk 'lib\cmake\libobs')) -and (Test-Path (Join-Path $ObsSdk 'lib\cmake\obs-frontend-api')))) {
    Write-Host ''
    Write-Host 'Configuring minimal OBS x64 SDK...'
    $ObsConfigureArgs = @(
        '-S', $ObsSource,
        '-B', $ObsBuild,
        '-G', 'Visual Studio 18 2026',
        '-A', 'x64,version=10.0.26100.0',
        '-DOBS_CMAKE_VERSION:STRING=3.0.0',
        "-DOBS_VERSION_OVERRIDE:STRING=$ObsVersion",
        '-DENABLE_PLUGINS:BOOL=OFF',
        '-DENABLE_FRONTEND:BOOL=OFF',
        '-DENABLE_SCRIPTING:BOOL=OFF',
        '-DENABLE_BROWSER:BOOL=OFF',
        '-DCMAKE_SYSTEM_VERSION:STRING=10.0.26100'
    )
    Invoke-CMake $ObsConfigureArgs 'OBS SDK configure failed.'

    Write-Host ''
    Write-Host 'Building only obs-frontend-api + its libobs dependency (RelWithDebInfo)...'
    $ObsBuildArgs = @('--build', $ObsBuild, '--target', 'obs-frontend-api', '--config', 'RelWithDebInfo', '--parallel')
    Invoke-CMake $ObsBuildArgs 'OBS SDK build failed.'

    Write-Host ''
    Write-Host 'Installing OBS development files into the local SDK cache...'
    $ObsInstallArgs = @('--install', $ObsBuild, '--component', 'Development', '--config', 'RelWithDebInfo', '--prefix', $ObsSdk)
    Invoke-CMake $ObsInstallArgs 'OBS SDK install failed.'
} else {
    Write-Host 'Using existing local OBS SDK cache.'
}

if (-not (Test-Path $QtPrefix)) {
    throw "Qt dependency prefix was not created at '$QtPrefix'."
}
if (-not (Test-Path $ObsDepsPrefix)) {
    throw "OBS dependency prefix was not created at '$ObsDepsPrefix'."
}

$PrefixPath = "$ObsSdk;$ObsDepsPrefix;$QtPrefix"

Write-Host ''
Write-Host 'EB Recorder build package: r6.1' -ForegroundColor Cyan
Write-Host 'Configuring EB Recorder plugin...'
$PluginConfigureArgs = @(
    '-S', $Root,
    '-B', $PluginBuild,
    '-G', 'Visual Studio 18 2026',
    '-A', 'x64,version=10.0.26100.0',
    "-DCMAKE_PREFIX_PATH=$PrefixPath",
    '-DCMAKE_SYSTEM_VERSION:STRING=10.0.26100'
)
Invoke-CMake $PluginConfigureArgs 'EB Recorder configure failed.'

Write-Host 'Building EB Recorder...'
$PluginBuildArgs = @('--build', $PluginBuild, '--config', 'RelWithDebInfo', '--parallel')
Invoke-CMake $PluginBuildArgs 'EB Recorder build failed.'

Remove-Item $StagingDir -Recurse -Force -ErrorAction SilentlyContinue
Write-Host 'Creating staged plugin layout...'
$PluginInstallArgs = @('--install', $PluginBuild, '--config', 'RelWithDebInfo', '--prefix', $StagingDir)
Invoke-CMake $PluginInstallArgs 'EB Recorder install failed.'

$PluginStaging = Join-Path $StagingDir 'eb-recorder'
$BuiltDll = Join-Path $PluginStaging 'bin\64bit\eb-recorder.dll'
if (-not (Test-Path $BuiltDll)) {
    throw "Built DLL was not found at '$BuiltDll'."
}

Remove-Item $DistDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $DistDir -Force | Out-Null
Copy-Item $PluginStaging (Join-Path $DistDir 'eb-recorder') -Recurse -Force
Copy-Item (Join-Path $Root 'INSTALL.cmd') $DistDir -Force
Copy-Item (Join-Path $Root 'UNINSTALL.cmd') $DistDir -Force
Copy-Item (Join-Path $Root 'scripts\install.ps1') $DistDir -Force
Copy-Item (Join-Path $Root 'scripts\uninstall.ps1') $DistDir -Force

$ZipPath = Join-Path $Root 'EBRecorder-0.3.0-windows-x64.zip'
Remove-Item $ZipPath -Force -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $DistDir '*') -DestinationPath $ZipPath -CompressionLevel Optimal

Write-Host ''
Write-Host 'Build complete.' -ForegroundColor Green
Write-Host "Release archive: $ZipPath"
Write-Host 'Install by extracting the release archive and running INSTALL.cmd.'
