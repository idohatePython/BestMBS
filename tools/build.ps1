param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [string]$QtRoot = $env:QT_ROOT,
    [string]$MinGWRoot = $env:MINGW_ROOT,
    [string]$BoostRoot = $env:BOOST_ROOT,
    [string]$SqliteRoot = $env:MBS_SQLITE_ROOT,
    [string]$VtkRoot = "",
    [switch]$Package,
    [ValidateRange(1, 16)]
    [int]$Jobs = 1,
    [string[]]$Targets = @(),
    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $VtkRoot) { $VtkRoot = Join-Path $projectRoot "third_party\vtk" }
$vtkConfig = Join-Path $VtkRoot "install\lib\cmake\vtk-9.5"

if (-not $QtRoot) { throw "Set QT_ROOT or pass -QtRoot (for example C:\Qt\6.11.2\mingw_64)." }
if (-not $MinGWRoot) { throw "Set MINGW_ROOT or pass -MinGWRoot (for example C:\Qt\Tools\mingw1310_64)." }
if (-not (Test-Path -LiteralPath (Join-Path $QtRoot "bin\qmake.exe"))) { throw "QtRoot is not a Qt MinGW kit: $QtRoot" }
if (-not (Test-Path -LiteralPath (Join-Path $MinGWRoot "bin\g++.exe"))) { throw "MinGWRoot is invalid: $MinGWRoot" }
if (-not (Test-Path -LiteralPath (Join-Path $vtkConfig "vtk-config.cmake"))) {
    throw "VTK SDK is missing. Run tools\bootstrap_vtk.ps1 first."
}

$mingwBin = Join-Path $MinGWRoot "bin"
$qtBin = Join-Path $QtRoot "bin"
$env:PATH = "$mingwBin;$qtBin;$env:PATH"
$buildDir = Join-Path $projectRoot "build\windows-mingw-$($Configuration.ToLowerInvariant())"
$configure = @(
    "-S", $projectRoot, "-B", $buildDir, "-G", "MinGW Makefiles",
    "-DCMAKE_BUILD_TYPE=$Configuration",
    "-DCMAKE_C_COMPILER=$(Join-Path $mingwBin 'gcc.exe')",
    "-DCMAKE_CXX_COMPILER=$(Join-Path $mingwBin 'g++.exe')",
    "-DCMAKE_MAKE_PROGRAM=$(Join-Path $mingwBin 'mingw32-make.exe')",
    "-DCMAKE_PREFIX_PATH=$QtRoot",
    "-DMBS_VTK_DIR=$vtkConfig",
    "-DMBS_BUILD_GUI=ON",
    "-DMBS_BUILD_TESTS=ON"
)
if ($BoostRoot) { $configure += "-DBOOST_ROOT=$BoostRoot" }
if ($SqliteRoot) { $configure += "-DMBS_SQLITE_ROOT=$SqliteRoot" }

cmake @configure
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed with exit code $LASTEXITCODE" }
if ($Targets.Count -gt 0) {
    cmake --build $buildDir --parallel $Jobs --target @Targets
}
else {
    cmake --build $buildDir --parallel $Jobs
}
if ($LASTEXITCODE -ne 0) { throw "CMake build failed with exit code $LASTEXITCODE" }

if (-not $SkipTests) {
    ctest --test-dir $buildDir --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "CTest failed with exit code $LASTEXITCODE" }
}

if ($Package) {
    $outRoot = [IO.Path]::GetFullPath((Join-Path $projectRoot "out"))
    $packageRoot = [IO.Path]::GetFullPath((Join-Path $outRoot "MBS-4.0-$($Configuration.ToLowerInvariant())"))
    if ([IO.Path]::GetDirectoryName($packageRoot) -ne $outRoot) {
        throw "Refusing to clean a package directory outside out: $packageRoot"
    }
    if (Test-Path -LiteralPath $packageRoot) { Remove-Item -LiteralPath $packageRoot -Recurse -Force }
    cmake --install $buildDir --prefix $packageRoot
    if ($LASTEXITCODE -ne 0) { throw "CMake install failed with exit code $LASTEXITCODE" }
    Write-Host "Runnable package: $packageRoot\bin\mbs-gui.exe"
}
