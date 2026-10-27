# Vigilante 8 Arcade recomp: one-shot setup. Run through Setup.cmd, or by the
# recomp-launcher installer as: setup.ps1 -Yes -Game <folder or package>.
#
# Does exactly what the README's "Step by step" section does, in order, and
# skips any step whose output already exists, so a rerun after a failure
# resumes. Details go to setup.log; the console gets one "[n/7]" line per step
# (the installer's progress bar reads them). It never downloads game data: you
# point it at your own copy.

param(
    [switch]$Yes,          # install missing tools without asking
    [string]$Game = '',    # extracted folder with default.xex, the XBLA package, or a folder holding it
    [switch]$Vr            # also build the OpenXR loader and use the VR branch of the SDK fork
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Log = Join-Path $Root 'setup.log'
$Data = Join-Path $Root 'extracted'
$Llvm = 'C:\Program Files\LLVM\bin'

# Pinned sources. Bump these together with the CHANGELOG.
# Flat builds stay on sp00nznet/rexglue-sdk. -Vr checks out this fork's VR
# commit, which adds the optional D3D12 bridge, and builds the OpenXR loader.
# If that commit is not on the fork yet, the same change is applied from
# tools\rexglue-vr-bridge.patch onto d50d599.
$Sdk = Join-Path $Root 'tools\rexglue-sdk'
$SdkRepo = 'https://github.com/sp00nznet/rexglue-sdk'
$SdkCommit = 'd50d599'   # sp00nznet/rexglue-sdk: upstream development@bd833a2 + our fixes
$SdkVrCommit = '82dbebf6a1da337d85ae93ae918835d8ea33e32b'  # mattymattmattmatt/rexglue-sdk cursor/openxr-vr-bridge-095c
if ($Vr) {
    $SdkRepo = 'https://github.com/mattymattmattmatt/rexglue-sdk'
    $SdkCommit = $SdkVrCommit
}
$SdkInstall = Join-Path $Sdk 'out\install\win-amd64'
$OpenXr = Join-Path $Root 'tools\openxr-sdk'
$OpenXrCommit = '24a1f2b'   # KhronosGroup/OpenXR-SDK release-1.1.65
$Xlive = Join-Path $Root 'tools\xlive'
$XliveCommit = '0632994'
$Tools360 = Join-Path $Root 'tools\360tools'
$Tools360Commit = '1b53928'

"=== setup $(Get-Date -Format s)" | Out-File $Log -Append -Encoding utf8

$script:n = 0
function Step($name) { $script:n++; Write-Host "[$script:n/7] $name" }
function Fail($msg) {
    Write-Host ''
    Write-Host "Setup stopped: $msg" -ForegroundColor Red
    Write-Host "Details are in $Log"
    exit 1
}
# Run a native command, logging its output; stop on a non-zero exit. Only the
# exit code counts: Windows PowerShell turns a native program's stderr (git's
# "Cloning into ...") into error records, which 'Stop' would treat as fatal.
function Run($what, [scriptblock]$cmd) {
    $ErrorActionPreference = 'Continue'
    & $cmd *>> $Log
    if ($LASTEXITCODE -ne 0) { Fail "$what failed (exit $LASTEXITCODE)." }
}
function Ask($q) { $Yes -or ((Read-Host "$q [y/N]") -match '^[yY]') }
function Clone($url, $dir, $commit) {
    if (-not (Test-Path (Join-Path $dir '.git'))) {
        Run "Cloning $url" { git -c core.longpaths=true clone $url $dir }
    } else {
        Run "Pointing $dir at $url" { git -C $dir remote set-url origin $url }
    }
    # A full SHA may live only on a side branch. GitHub will serve that object
    # directly; a short pin is fetched with the remote's default branch.
    if ($commit -match '^[0-9a-fA-F]{40}$') {
        Run "Checking out $commit" { git -C $dir fetch --quiet origin $commit; git -C $dir checkout --quiet $commit }
    } else {
        Run "Checking out $commit" { git -C $dir fetch --quiet origin; git -C $dir checkout --quiet $commit }
    }
}
# -Vr prefers the fork commit. Until that object is on the remote, check out
# the saintsrow pin and apply the bridge patch shipped in this repo. The
# result is the same tree as the commit.
function Checkout-VrSdk($url, $dir, $commit) {
    if (-not (Test-Path (Join-Path $dir '.git'))) {
        Run "Cloning $url" { git -c core.longpaths=true clone $url $dir }
    } else {
        Run "Pointing $dir at $url" { git -C $dir remote set-url origin $url }
    }
    $ErrorActionPreference = 'Continue'
    & git -C $dir fetch --quiet origin $commit *>> $Log
    $fetched = $LASTEXITCODE -eq 0
    $ErrorActionPreference = 'Stop'
    if ($fetched) {
        Run "Checking out $commit" { git -C $dir checkout -f --quiet $commit }
        return
    }
    Write-Host "   $commit is not on the fork. Using d50d599 plus tools\rexglue-vr-bridge.patch"
    "VR commit $commit was not on the fork; applying tools/rexglue-vr-bridge.patch onto d50d599" |
        Out-File $Log -Append -Encoding utf8
    Run "Checking out d50d599" { git -C $dir fetch --quiet origin; git -C $dir checkout -f --quiet d50d599 }
    Remove-Item (Join-Path $dir 'include\rex\graphics\vr_bridge.h') -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $dir 'src\core\vr_bridge.cpp') -Force -ErrorAction SilentlyContinue
    $patch = Join-Path $Root 'tools\rexglue-vr-bridge.patch'
    Run 'Applying the VR bridge patch' { git -C $dir apply $patch }
}
# An Xbox 360 content package starts with LIVE, PIRS or "CON ".
function Test-Package($file) {
    $fs = [IO.File]::OpenRead($file)
    try { $b = New-Object byte[] 4; $null = $fs.Read($b, 0, 4) } finally { $fs.Close() }
    [Text.Encoding]::ASCII.GetString($b) -in 'LIVE', 'PIRS', 'CON '
}

# --- 1. Prerequisites ---------------------------------------------------------
Step 'Checking prerequisites'
if (Test-Path $Llvm) { $env:PATH = "$Llvm;$env:PATH" }
$need = @(
    @{ exe = 'git';    id = 'Git.Git';            why = 'fetches the ReXGlue SDK';     size = '~60 MB' },
    @{ exe = 'cmake';  id = 'Kitware.CMake';      why = 'configures the build';        size = '~35 MB' },
    @{ exe = 'ninja';  id = 'Ninja-build.Ninja';  why = 'runs the build';              size = '~1 MB' },
    @{ exe = 'clang';  id = 'LLVM.LLVM';          why = 'compiles everything';         size = '~400 MB' },
    @{ exe = 'py';     id = 'Python.Python.3.12'; why = 'unpacks the XBLA package';    size = '~30 MB' }
)
$installed = $false
foreach ($t in $need) {
    if (Get-Command $t.exe -ErrorAction SilentlyContinue) { continue }
    Write-Host "   Missing: $($t.exe) -- $($t.why), $($t.size) via winget ($($t.id))."
    if (-not (Ask '   Install it now?')) { Fail "$($t.exe) is required. Install it, open a new window, and run Setup.cmd again." }
    Run "Installing $($t.exe)" { winget install --id $t.id -e --accept-package-agreements --accept-source-agreements }
    $installed = $true
}
if ($installed) {
    # Pick up the new tools without a new window.
    $env:PATH = [Environment]::GetEnvironmentVariable('PATH', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('PATH', 'User')
    if (Test-Path $Llvm) { $env:PATH = "$Llvm;$env:PATH" }
}
# clang needs the Windows SDK and MSVC libraries from Visual Studio or its Build Tools.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere) -or -not (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)) {
    Write-Host '   Missing: Visual Studio 2022 Build Tools with "Desktop development with C++" (~3 GB).'
    if (-not (Ask '   Install it now?')) { Fail 'The MSVC libraries and Windows SDK are required.' }
    Run 'Installing VS Build Tools' { winget install --id Microsoft.VisualStudio.2022.BuildTools -e --accept-package-agreements --accept-source-agreements --override '--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended' }
}

# --- 2. Game data -------------------------------------------------------------
Step 'Game data'
if (-not (Test-Path (Join-Path $Data 'default.xex'))) {
    $src = $Game
    if (-not $src) {
        if ($env:RECOMP_LAUNCHER) { Fail 'No game given (-Game).' }
        $src = Read-Host '   Path to your Vigilante 8 Arcade package, or a folder holding it or default.xex'
    }
    $src = $src.Trim('"')
    if (-not (Test-Path $src)) { Fail "'$src' doesn't exist." }
    $src = (Resolve-Path $src).Path
    if (Test-Path (Join-Path $src 'default.xex')) {
        New-Item -ItemType Directory -Force $Data | Out-Null
        Copy-Item -Recurse -Force "$src\*" $Data
    } else {
        $pkg = if (Test-Path $src -PathType Leaf) { $src } else {
            Get-ChildItem $src -File -Recurse -Depth 5 | Where-Object { $_.Length -gt 50MB } |
                Where-Object { Test-Package $_.FullName } | Select-Object -First 1 -ExpandProperty FullName
        }
        if (-not $pkg -or -not (Test-Package $pkg)) { Fail "No default.xex or Xbox 360 package found in '$src'." }
        Write-Host "   Unpacking $pkg"
        Clone 'https://github.com/sp00nznet/360tools' $Tools360 $Tools360Commit
        Run 'Unpacking the package' { py -I "$Tools360\tools\extract_stfs.py" $pkg $Data }
    }
    if (-not (Test-Path (Join-Path $Data 'default.xex'))) { Fail 'extracted\default.xex is missing. Is that the Vigilante 8 Arcade package?' }
}
Write-Host "   $Data"

# --- 3. ReXGlue SDK -----------------------------------------------------------
Step $(if ($Vr) { 'ReXGlue SDK, VR branch (the first build takes 10-20 minutes)' } else { 'ReXGlue SDK (the first build takes 10-20 minutes)' })
$sdkStamp = Join-Path $SdkInstall 'commit.txt'
if (-not ((Test-Path $sdkStamp) -and ((Get-Content $sdkStamp) -eq $SdkCommit))) {
    if ($Vr) { Checkout-VrSdk $SdkRepo $Sdk $SdkCommit } else { Clone $SdkRepo $Sdk $SdkCommit }
    Run 'Fetching SDK submodules' { git -C $Sdk -c core.longpaths=true submodule update --init --recursive }
    Push-Location $Sdk
    try {
        # Forward slashes: CMake reads the backslashes in C:\Program Files\... as escapes.
        $rc = ($Llvm -replace '\\', '/') + '/llvm-rc.exe'
        Run 'Configuring the SDK' { cmake --preset win-amd64 "-DCMAKE_RC_COMPILER=$rc" }
        Run 'Building the SDK' { cmake --build out/build/win-amd64 --config Release }
        Run 'Installing the SDK' { cmake --install out/build/win-amd64 --config Release --prefix out/install/win-amd64 }
    } finally { Pop-Location }
    Set-Content $sdkStamp $SdkCommit
}
if ($Vr) {
    Write-Host '   OpenXR loader (Khronos OpenXR-SDK 1.1.65)'
    $loader = Get-ChildItem $OpenXr -Recurse -Filter openxr_loader.dll -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\build\\' } | Select-Object -First 1
    if (-not $loader) {
        if (-not (Test-Path (Join-Path $OpenXr '.git'))) {
            Run 'Cloning the OpenXR SDK' {
                git -c core.longpaths=true clone --branch release-1.1.65 --depth 1 https://github.com/KhronosGroup/OpenXR-SDK $OpenXr
            }
        }
        Run 'Checking out the OpenXR SDK' { git -C $OpenXr checkout --quiet $OpenXrCommit }
        $build = Join-Path $OpenXr 'build'
        Run 'Configuring the OpenXR loader' {
            cmake -S $OpenXr -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DDYNAMIC_LOADER=ON -DBUILD_TESTS=OFF -DBUILD_CONFORMANCE_TESTS=OFF -DBUILD_API_LAYERS=OFF -DBUILD_SDK_TESTS=OFF
        }
        Run 'Building the OpenXR loader' { cmake --build $build }
    }
}

# --- 4. xlive relay client ----------------------------------------------------
Step 'xlive relay client'
Clone 'https://github.com/sp00nznet/xlive' $Xlive $XliveCommit

# --- 5. Codegen ---------------------------------------------------------------
Step 'Recompiling default.xex to C++ (codegen)'
Push-Location (Join-Path $Root 'project')
try {
    Run 'Codegen' { & "$SdkInstall\bin\rexglue.exe" codegen vig8_manifest.toml }

    # --- 6. Build -------------------------------------------------------------
    Step 'Building the game (about 5 minutes the first time)'
    if (-not (Test-Path 'build\build.ninja')) { Run 'Configuring the game' { cmake --preset win-amd64 } }
    Run 'Building the game' { cmake --build build }
} finally { Pop-Location }

# --- 7. Launcher --------------------------------------------------------------
Step 'Writing Play.cmd'
"@echo off`r`nstart `"`" `"%~dp0project\build\vig8.exe`" %*`r`n" |
    Set-Content -Path (Join-Path $Root 'Play.cmd') -Encoding ascii -NoNewline
if ($Vr) {
    $dll = Get-ChildItem $OpenXr -Recurse -Filter openxr_loader.dll -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\build\\' } | Select-Object -First 1
    if (-not $dll) { Fail 'openxr_loader.dll was not built. See setup.log.' }
    Copy-Item $dll.FullName (Join-Path $Root 'project\build\openxr_loader.dll') -Force
    "@echo off`r`nstart `"`" `"%~dp0project\build\vig8.exe`" --vr %*`r`n" |
        Set-Content -Path (Join-Path $Root 'PlayVR.cmd') -Encoding ascii -NoNewline
}
Write-Host ''
if ($Vr) {
    Write-Host 'Done. Double-click PlayVR.cmd (or Play.cmd --vr) with the headset runtime already running.' -ForegroundColor Green
} else {
    Write-Host 'Done. Double-click Play.cmd to start the game.' -ForegroundColor Green
}
