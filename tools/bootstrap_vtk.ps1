param(
    [string]$QtRoot = $env:QT_ROOT,
    [string]$MinGWRoot = $env:MINGW_ROOT,
    [ValidateRange(1, 16)]
    [int]$Jobs = 8
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$vtkRoot = Join-Path $projectRoot "third_party\vtk"
$archive = Join-Path $vtkRoot "VTK-9.5.2.tar.gz"
$sourceRoot = Join-Path $vtkRoot "source"
$source = Join-Path $sourceRoot "VTK-9.5.2"
$expectedHash = "CEE64B98D270FF7302DAF1EF13458DFF5D5AC1ECB45D47723835F7F7D562C989"
$mingwBin = if ($MinGWRoot) { Join-Path $MinGWRoot "bin" } else { "" }
$qtBin = if ($QtRoot) { Join-Path $QtRoot "bin" } else { "" }
if (-not $QtRoot -or -not (Test-Path -LiteralPath (Join-Path $qtBin "qmake.exe"))) {
    throw "Set QT_ROOT or pass -QtRoot (for example C:\Qt\6.11.2\mingw_64)."
}
if (-not $MinGWRoot -or -not (Test-Path -LiteralPath (Join-Path $mingwBin "g++.exe"))) {
    throw "Set MINGW_ROOT or pass -MinGWRoot (for example C:\Qt\Tools\mingw1310_64)."
}
$env:PATH = "$mingwBin;$qtBin;$env:PATH"

New-Item -ItemType Directory -Force -Path $vtkRoot, $sourceRoot | Out-Null
if (-not (Test-Path -LiteralPath $archive)) {
    curl.exe -L --fail --retry 3 --output $archive `
        "https://www.vtk.org/files/release/9.5/VTK-9.5.2.tar.gz"
    if ($LASTEXITCODE -ne 0) { throw "VTK download failed with exit code $LASTEXITCODE" }
}
if ((Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash -ne $expectedHash) {
    throw "VTK archive SHA-256 does not match the approved source archive"
}
if (-not (Test-Path -LiteralPath (Join-Path $source "CMakeLists.txt"))) {
    Push-Location $sourceRoot
    try {
        cmake -E tar xzf $archive
        if ($LASTEXITCODE -ne 0) { throw "VTK extraction failed with exit code $LASTEXITCODE" }
    }
    finally {
        Pop-Location
    }
}

$tiffHeader = Join-Path $source "ThirdParty\tiff\vtktiff\libtiff\tiffiop.h"
$text = [IO.File]::ReadAllText($tiffHeader)
$oldHandlers = @"
    vtktiff_EXPORT TIFFErrorHandler _TIFFwarningHandler;
    vtktiff_EXPORT TIFFErrorHandler _TIFFerrorHandler;
    vtktiff_EXPORT TIFFErrorHandlerExt _TIFFwarningHandlerExt;
    vtktiff_EXPORT TIFFErrorHandlerExt _TIFFerrorHandlerExt;
"@
$newHandlers = @"
#ifdef _WIN32
    extern vtktiff_EXPORT TIFFErrorHandler _TIFFwarningHandler;
    extern vtktiff_EXPORT TIFFErrorHandler _TIFFerrorHandler;
    extern vtktiff_EXPORT TIFFErrorHandlerExt _TIFFwarningHandlerExt;
    extern vtktiff_EXPORT TIFFErrorHandlerExt _TIFFerrorHandlerExt;
#else
    vtktiff_EXPORT TIFFErrorHandler _TIFFwarningHandler;
    vtktiff_EXPORT TIFFErrorHandler _TIFFerrorHandler;
    vtktiff_EXPORT TIFFErrorHandlerExt _TIFFwarningHandlerExt;
    vtktiff_EXPORT TIFFErrorHandlerExt _TIFFerrorHandlerExt;
#endif
"@
$oldCodecs = "    vtktiff_EXPORT const TIFFCodec _TIFFBuiltinCODECS[];"
$newCodecs = @"
#ifdef _WIN32
    extern vtktiff_EXPORT const TIFFCodec _TIFFBuiltinCODECS[];
#else
    vtktiff_EXPORT const TIFFCodec _TIFFBuiltinCODECS[];
#endif
"@
if ($text.Contains($oldHandlers)) { $text = $text.Replace($oldHandlers, $newHandlers) }
if ($text.Contains($oldCodecs)) { $text = $text.Replace($oldCodecs, $newCodecs) }
if (-not $text.Contains("extern vtktiff_EXPORT TIFFErrorHandler _TIFFwarningHandler")) {
    throw "The VTK MinGW compatibility patch could not be applied"
}
[IO.File]::WriteAllText($tiffHeader, $text, [Text.UTF8Encoding]::new($false))

# MinGW does not place inline members of this explicitly instantiated template
# in the DLL import library. Allow consuming VTK modules to instantiate those
# inline members locally while preserving the upstream behavior elsewhere.
$structuredPointHeader = Join-Path $source "Common\Core\vtkStructuredPointArray.h"
$structuredPointText = [IO.File]::ReadAllText($structuredPointHeader)
$structuredPointText = $structuredPointText.Replace(
    '#elif defined(VTK_USE_EXTERN_TEMPLATE)',
    '#elif defined(VTK_USE_EXTERN_TEMPLATE) && !defined(__MINGW32__)')
if (-not $structuredPointText.Contains(
        '#elif defined(VTK_USE_EXTERN_TEMPLATE) && !defined(__MINGW32__)')) {
    throw "The VTK structured-point MinGW compatibility patch could not be applied"
}
[IO.File]::WriteAllText(
    $structuredPointHeader, $structuredPointText, [Text.UTF8Encoding]::new($false))

subst V: $vtkRoot
if ($LASTEXITCODE -ne 0) { throw "Unable to create the temporary V: build mapping" }
try {
    $configure = @(
        "-S", "V:/source/VTK-9.5.2", "-B", "V:/build", "-G", "MinGW Makefiles",
        "-DCMAKE_BUILD_TYPE:STRING=Release",
        "-DCMAKE_CXX_COMPILER:FILEPATH=$(Join-Path $mingwBin 'g++.exe')",
        "-DCMAKE_C_COMPILER:FILEPATH=$(Join-Path $mingwBin 'gcc.exe')",
        "-DCMAKE_MAKE_PROGRAM:FILEPATH=$(Join-Path $mingwBin 'mingw32-make.exe')",
        "-DCMAKE_PREFIX_PATH:PATH=$QtRoot",
        "-DCMAKE_INSTALL_PREFIX:PATH=V:/install",
        "-DBUILD_SHARED_LIBS:BOOL=ON", "-DVTK_BUILD_TESTING:STRING=OFF",
        "-DVTK_BUILD_EXAMPLES:BOOL=OFF", "-DVTK_BUILD_DOCUMENTATION:BOOL=OFF",
        "-DVTK_BUILD_SPHINX_DOCUMENTATION:BOOL=OFF", "-DVTK_WRAP_PYTHON:BOOL=OFF",
        "-DVTK_WRAP_JAVA:BOOL=OFF", "-DVTK_QT_VERSION:STRING=6",
        "-DVTK_GROUP_ENABLE_StandAlone:STRING=DONT_WANT",
        "-DVTK_GROUP_ENABLE_Rendering:STRING=DONT_WANT",
        "-DVTK_GROUP_ENABLE_Imaging:STRING=DONT_WANT",
        "-DVTK_GROUP_ENABLE_Views:STRING=DONT_WANT",
        "-DVTK_GROUP_ENABLE_Web:STRING=DONT_WANT",
        "-DVTK_GROUP_ENABLE_Qt:STRING=DONT_WANT",
        "-DVTK_MODULE_ENABLE_VTK_GUISupportQt:STRING=YES",
        "-DVTK_MODULE_ENABLE_VTK_IOGeometry:STRING=YES",
        "-DVTK_MODULE_ENABLE_VTK_IOLegacy:STRING=YES",
        "-DVTK_MODULE_ENABLE_VTK_IOXML:STRING=YES",
        "-DVTK_MODULE_ENABLE_VTK_FiltersSources:STRING=YES",
        "-DVTK_MODULE_ENABLE_VTK_FiltersGeometry:STRING=YES",
        "-DVTK_MODULE_ENABLE_VTK_RenderingAnnotation:STRING=YES"
    )
    cmake @configure
    if ($LASTEXITCODE -ne 0) { throw "VTK configure failed with exit code $LASTEXITCODE" }
    cmake --build "V:/build" --parallel $Jobs
    if ($LASTEXITCODE -ne 0) { throw "VTK build failed with exit code $LASTEXITCODE" }
    cmake --install "V:/build"
    if ($LASTEXITCODE -ne 0) { throw "VTK install failed with exit code $LASTEXITCODE" }
}
finally {
    subst V: /D | Out-Null
}
