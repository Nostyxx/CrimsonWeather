[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$programFilesX86 = [Environment]::GetFolderPath('ProgramFilesX86')
$vswhere = Join-Path $programFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw 'Visual Studio discovery is unavailable; install the C++ Build Tools to run the protocol harness.'
}

$devCommand = @(& $vswhere -latest -products '*' -version '[18.0,)' `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -find 'Common7\Tools\VsDevCmd.bat') | Select-Object -First 1
if ([string]::IsNullOrWhiteSpace($devCommand) -or -not (Test-Path -LiteralPath $devCommand -PathType Leaf)) {
    throw 'Visual Studio 2026 / Build Tools developer command was not found.'
}

$environmentLines = & cmd.exe /d /s /c ('"{0}" -arch=amd64 -no_logo && set' -f $devCommand)
if ($LASTEXITCODE -ne 0) {
    throw "Visual Studio environment setup failed with exit code $LASTEXITCODE."
}
foreach ($line in $environmentLines) {
    if ($line -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
}

$outputDirectory = Join-Path ([System.IO.Path]::GetTempPath()) 'CrimsonWeather-community-protocol-tests'
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$executable = Join-Path $outputDirectory 'community_protocol_harness.exe'
$includeRoot = Join-Path $repositoryRoot 'CrimsonWeatherReshade'
$compileArguments = @(
    '/nologo', '/std:c++20', '/EHsc', '/W4', '/DNOMINMAX',
    "/I$includeRoot", "/I$(Join-Path $includeRoot 'include')",
    "/Fo$outputDirectory\", "/Fe$executable",
    (Join-Path $repositoryRoot 'tests\cpp\community_protocol_harness.cpp'),
    (Join-Path $includeRoot 'community\community_protocol.cpp')
)
& cl.exe @compileArguments
if ($LASTEXITCODE -ne 0) {
    throw "Community protocol harness compilation failed with exit code $LASTEXITCODE."
}

& $executable
if ($LASTEXITCODE -ne 0) {
    throw "Community protocol harness failed with exit code $LASTEXITCODE."
}
