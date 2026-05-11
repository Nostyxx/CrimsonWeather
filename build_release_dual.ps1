$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$project = Join-Path $repoRoot 'CrimsonWeatherReshade\CrimsonWeatherReshade.vcxproj'
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe'

function Get-SanitizedEnvironmentMap {
    $envMap = @{}
    $pathValue = $null

    foreach ($item in Get-ChildItem Env:) {
        if ($item.Name -ieq 'PATH') {
            if ($null -eq $pathValue -or $item.Name -ceq 'Path') {
                $pathValue = $item.Value
            }
            continue
        }

        if (-not $envMap.ContainsKey($item.Name)) {
            $envMap[$item.Name] = $item.Value
        }
    }

    if ($null -ne $pathValue) {
        $envMap['Path'] = $pathValue
    }

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
    $psi.EnvironmentVariables.Clear()

    foreach ($entry in (Get-SanitizedEnvironmentMap).GetEnumerator()) {
        [void]$psi.EnvironmentVariables.Add($entry.Key, $entry.Value)
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

$releaseDir = Join-Path $repoRoot 'CrimsonWeatherReshade\x64\Release'
$legacyWindOnlyAddon = Join-Path $releaseDir 'Crimson Weather (Wind only).addon64'
if (Test-Path -LiteralPath $legacyWindOnlyAddon) {
    Remove-Item -LiteralPath $legacyWindOnlyAddon -Force
}
