function Find-CrimsonWeatherMSBuild {
    [CmdletBinding()]
    param([string]$PathOverride = $env:CW_MSBUILD_PATH)

    if (-not [string]::IsNullOrWhiteSpace($PathOverride)) {
        if (-not (Test-Path -LiteralPath $PathOverride -PathType Leaf)) {
            throw "MSBuild override does not exist: $PathOverride"
        }
        return (Resolve-Path -LiteralPath $PathOverride).ProviderPath
    }

    $installerRoot = [Environment]::GetFolderPath('ProgramFilesX86')
    $vswhere = Join-Path $installerRoot 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'Visual Studio discovery is unavailable. Install Visual Studio 2026 / Build Tools with C++ tools, or supply -MSBuildPath (CW_MSBUILD_PATH).'
    }

    $candidates = @(& $vswhere -latest -products '*' -version '[18.0,)' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -find 'MSBuild\Current\Bin\amd64\MSBuild.exe')
    if ($LASTEXITCODE -ne 0) {
        throw "Visual Studio discovery failed with exit code $LASTEXITCODE. Supply -MSBuildPath to select MSBuild explicitly."
    }
    foreach ($candidate in $candidates) {
        if (-not [string]::IsNullOrWhiteSpace($candidate) -and
            (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $candidate).ProviderPath
        }
    }
    throw 'No suitable MSBuild was found. Install Visual Studio 2026 / Build Tools with C++ tools and v145, or supply -MSBuildPath (CW_MSBUILD_PATH).'
}
