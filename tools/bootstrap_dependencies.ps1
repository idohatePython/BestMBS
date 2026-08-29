param(
    [string]$ThirdPartyRoot = "",
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $ThirdPartyRoot) {
    $ThirdPartyRoot = Join-Path $projectRoot "third_party"
}
$ThirdPartyRoot = [IO.Path]::GetFullPath($ThirdPartyRoot)
New-Item -ItemType Directory -Path $ThirdPartyRoot -Force | Out-Null

function Invoke-Git([string[]]$Arguments) {
    & git @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "git failed: git $($Arguments -join ' ')"
    }
}

function Get-PinnedRepository(
    [string]$Name,
    [string]$Url,
    [string]$Revision
) {
    $destination = Join-Path $ThirdPartyRoot $Name
    if ((Test-Path -LiteralPath $destination) -and $Force) {
        $resolvedRoot = [IO.Path]::GetFullPath($ThirdPartyRoot)
        $resolvedDestination = [IO.Path]::GetFullPath($destination)
        if ([IO.Path]::GetDirectoryName($resolvedDestination) -ne $resolvedRoot) {
            throw "Refusing to remove a dependency outside third_party: $resolvedDestination"
        }
        Remove-Item -LiteralPath $resolvedDestination -Recurse -Force
    }
    if (-not (Test-Path -LiteralPath (Join-Path $destination ".git"))) {
        Invoke-Git @("clone", "--filter=blob:none", $Url, $destination)
    }
    Invoke-Git @("-C", $destination, "fetch", "--depth", "1", "origin", $Revision)
    Invoke-Git @("-C", $destination, "checkout", "--detach", "FETCH_HEAD")
}

Get-PinnedRepository "manifold" "https://github.com/elalish/manifold.git" `
    "11235e6b8ebea2dbed8aec4285685aafd3d95667"
Get-PinnedRepository "tetgen" "https://github.com/libigl/tetgen.git" `
    "e05aca7df74e3f531bc35733ed87d36d437266c5"
Get-PinnedRepository "cgal" "https://github.com/CGAL/cgal.git" `
    "cefe3007d59cf4292a09da4fa8a35f38478a4e0b"
Get-PinnedRepository "eigen" "https://gitlab.com/libeigen/eigen.git" "3.4.0"

$tetgenRoot = Join-Path $ThirdPartyRoot "tetgen"
$tetgenPatch = Join-Path $projectRoot "patches\tetgen\0001-keep-tet10-connectivity-alive.patch"
& git -C $tetgenRoot apply --check $tetgenPatch 2>$null
if ($LASTEXITCODE -eq 0) {
    Invoke-Git @("-C", $tetgenRoot, "apply", $tetgenPatch)
}
else {
    & git -C $tetgenRoot apply --reverse --check $tetgenPatch 2>$null
    if ($LASTEXITCODE -ne 0) {
        throw "TetGen compatibility patch does not match the pinned source revision."
    }
}

Write-Host "Dependencies prepared under $ThirdPartyRoot"
Write-Host "Run tools\bootstrap_vtk.ps1 separately to build the VTK SDK."
