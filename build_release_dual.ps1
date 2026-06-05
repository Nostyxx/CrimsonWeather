$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$project = Join-Path $repoRoot 'CrimsonWeatherReshade\CrimsonWeatherReshade.vcxproj'
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe'

function Get-SanitizedEnvironmentMap {
    $envMap = @{}
    $names = @(
        'ALLUSERSPROFILE',
        'APPDATA',
        'CommonProgramFiles',
        'CommonProgramW6432',
        'COMPUTERNAME',
        'ComSpec',
        'LOCALAPPDATA',
        'NUMBER_OF_PROCESSORS',
        'OS',
        'PATHEXT',
        'PROCESSOR_ARCHITECTURE',
        'ProgramData',
        'ProgramFiles',
        'ProgramFiles(x86)',
        'ProgramW6432',
        'PSModulePath',
        'PUBLIC',
        'SystemDrive',
        'SystemRoot',
        'TEMP',
        'TMP',
        'USERDOMAIN',
        'USERNAME',
        'USERPROFILE',
        'windir'
    )

    foreach ($name in $names) {
        $value = [Environment]::GetEnvironmentVariable($name, 'Process')
        if ($null -ne $value -and -not $envMap.ContainsKey($name)) {
            $envMap[$name] = $value
        }
    }

    $pathValue = [Environment]::GetEnvironmentVariable('Path', 'Process')
    if ($null -eq $pathValue) {
        $machinePath = [Environment]::GetEnvironmentVariable('Path', 'Machine')
        $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
        $pathValue = (($machinePath, $userPath) | Where-Object { $_ }) -join ';'
    }
    $envMap['Path'] = $pathValue

    return $envMap
}

function Invoke-SanitizedMSBuild {
    param(
        [string]$Arguments
    )

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $msbuild
    $psi.Arguments = $Arguments
    $psi.WorkingDirectory = $repoRoot
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true

    $envTarget = $psi.Environment
    if ($null -eq $envTarget) {
        $envTarget = $psi.EnvironmentVariables
    }
    $envTarget.Clear()

    foreach ($entry in (Get-SanitizedEnvironmentMap).GetEnumerator()) {
        [void]$envTarget.Add($entry.Key, $entry.Value)
    }

    $proc = [System.Diagnostics.Process]::Start($psi)
    $stdout = $proc.StandardOutput.ReadToEnd()
    $stderr = $proc.StandardError.ReadToEnd()
    $proc.WaitForExit()

    if ($stdout) {
        Write-Output $stdout
    }
    if ($stderr) {
        Write-Output $stderr
    }
    if ($proc.ExitCode -ne 0) {
        throw "MSBuild failed with exit code $($proc.ExitCode)."
    }
}

Invoke-SanitizedMSBuild ('"' + $project + '" /t:Rebuild /p:Configuration=Release /p:Platform=x64 /m')
Invoke-SanitizedMSBuild ('"' + $project + '" /t:Rebuild /p:Configuration=Release /p:Platform=x64 /p:CWBuildFlavor=WindOnly /m')
Invoke-SanitizedMSBuild ('"' + $project + '" /t:Rebuild /p:Configuration=Release /p:Platform=x64 /p:CWBuildFlavor=Dev /m')

$releaseDir = Join-Path $repoRoot 'CrimsonWeatherReshade\x64\Release'
$legacyWindOnlyAddon = Join-Path $releaseDir 'Crimson Weather (Wind only).addon64'
if (Test-Path -LiteralPath $legacyWindOnlyAddon) {
    Remove-Item -LiteralPath $legacyWindOnlyAddon -Force
}
