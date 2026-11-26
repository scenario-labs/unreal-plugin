# Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.
<#
Runs Install-Windows.bat as a person would, against a stand-in Unreal Engine
whose RunUAT.bat records its arguments and lays out a package instead of
compiling. Every path holds spaces. Windows only: CI's installer job runs it on
a GitHub-hosted runner, which has Visual Studio but no Unreal Engine.

Usage (Windows PowerShell 5.1): powershell -File tools\tests\installer_windows.ps1
#>
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$Repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$Work = Join-Path ([System.IO.Path]::GetTempPath()) 'Scenario installer test'
if (Test-Path -LiteralPath $Work) { Remove-Item -LiteralPath $Work -Recurse -Force }

# A stand-in Unreal 5.8, listed the way the Epic Games Launcher lists its installs.
$Engine = Join-Path $Work 'Epic Games\UE_5.8'
$BatchFiles = Join-Path $Engine 'Engine\Build\BatchFiles'
New-Item -ItemType Directory -Force -Path $BatchFiles | Out-Null
Set-Content -LiteralPath (Join-Path $Engine 'Engine\Build\Build.version') -Encoding ASCII -Value '{ "MajorVersion": 5, "MinorVersion": 8, "PatchVersion": 0 }'
# Unreal 5.8 installs the Toolset Registry; section 7 removes it, as in Unreal 5.6 and 5.7.
$Registry = Join-Path $Engine 'Engine\Plugins\Experimental\ToolsetRegistry'
New-Item -ItemType Directory -Force -Path $Registry | Out-Null
$Stub = @'
@echo off
setlocal EnableExtensions
set "plugin="
set "package="
:next
if "%~1"=="" goto build
set "arg=%~1"
>>"%SCENARIO_STUB_LOG%" echo(%arg%
if /i "%arg:~0,8%"=="-Plugin=" set "plugin=%arg:~8%"
if /i "%arg:~0,9%"=="-Package=" set "package=%arg:~9%"
shift
goto next
:build
echo [1/2] Compile [x64] Module.ScenarioCore.cpp
echo [2/2] Link [x64] UnrealEditor-ScenarioCore.dll
if not defined SCENARIO_STUB_EXIT goto package
echo Module.ScenarioCore.cpp: error C2065: stand_in: undeclared identifier
exit /b %SCENARIO_STUB_EXIT%
:package
for %%P in ("%plugin%") do set "source=%%~dpP"
xcopy "%source%." "%package%\" /e /i /q /y >nul
mkdir "%package%\Binaries\Win64" 2>nul
echo stand-in>"%package%\Binaries\Win64\UnrealEditor-ScenarioCore.dll"
echo BUILD SUCCESSFUL
exit /b 0
'@
Set-Content -LiteralPath (Join-Path $BatchFiles 'RunUAT.bat') -Encoding ASCII -Value $Stub
$LauncherList = Join-Path $Work 'LauncherInstalled.dat'
$Listed = @{ InstallationList = @(@{ InstallLocation = $Engine; AppName = 'UE_5.8'; AppVersion = '5.8.0' }) }
Set-Content -LiteralPath $LauncherList -Encoding ASCII -Value ($Listed | ConvertTo-Json -Depth 4)
$env:SCENARIO_LAUNCHER_LIST = $LauncherList
$env:SCENARIO_STUB_LOG = Join-Path $Work 'runuat-arguments.txt'

function New-Project([string]$Folder, [string]$Association) {
    $dir = Join-Path $Work $Folder
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $file = Join-Path $dir ((Split-Path -Leaf $dir).Replace(' ', '') + '.uproject')
    Set-Content -LiteralPath $file -Encoding ASCII -Value ('{ "FileVersion": 3, "EngineAssociation": "' + $Association + '", "Category": "", "Description": "" }')
    return $file
}

# Install-Windows.bat through cmd, as Explorer starts it, with its output kept for the checks.
function Invoke-Installer([string]$Arguments) {
    $out = Join-Path $Work 'installer-output.txt'
    $line = '/d /s /c ""{0}" {1} > "{2}" 2>&1"' -f (Join-Path $Repo 'Install-Windows.bat'), $Arguments, $out
    $process = Start-Process -FilePath $env:ComSpec -ArgumentList $line -NoNewWindow -Wait -PassThru
    $text = Get-Content -LiteralPath $out -Raw
    Write-Host $text
    return [pscustomobject]@{ Code = $process.ExitCode; Output = $text }
}

# The Optional field of the Toolset Registry reference in a Scenario.uplugin.
function Get-RegistryOptional([string]$File) {
    $descriptor = Get-Content -LiteralPath $File -Raw | ConvertFrom-Json
    return @($descriptor.Plugins | Where-Object { $_.Name -eq 'ToolsetRegistry' })[0].Optional
}

$script:Failures = 0
function Check([bool]$Condition, [string]$Name) {
    if ($Condition) {
        Write-Host "ok    $Name"
    } else {
        Write-Host "FAIL  $Name" -ForegroundColor Red
        $script:Failures++
    }
}

$Project = New-Project 'My Projects\Space Game' '5.8'
$ProjectDir = Split-Path -Parent $Project
$Installed = Join-Path $ProjectDir 'Plugins\Scenario'
$PluginFile = Join-Path $Repo 'Plugins\Scenario\Scenario.uplugin'

Write-Host '--- 1. A first install, the engine found through the Launcher list'
$run = Invoke-Installer ('-Project "{0}" -Yes' -f $Project)
Check ($run.Code -eq 0) 'the installer succeeds'
Check (Test-Path -LiteralPath (Join-Path $Installed 'Scenario.uplugin')) 'the plugin is in Plugins\Scenario'
Check (Test-Path -LiteralPath (Join-Path $Installed 'Binaries\Win64\UnrealEditor-ScenarioCore.dll')) 'with its Windows binaries'
Check ($run.Output -match '\[2/2\] Link') 'build progress is shown'
$Recorded = @(Get-Content -LiteralPath $env:SCENARIO_STUB_LOG)
Check ($Recorded[0] -eq 'BuildPlugin') 'RunUAT runs BuildPlugin'
Check ($Recorded -contains "-Plugin=$PluginFile") 'the plugin path reaches RunUAT whole, spaces included'
Check ($Recorded -contains '-TargetPlatforms=Win64') 'for Win64'
Check (@(Get-ChildItem -LiteralPath (Join-Path $ProjectDir 'Saved\Scenario') -Filter 'Install-*.log').Count -eq 1) 'the build log is in Saved\Scenario'
Check ((Get-RegistryOptional (Join-Path $Installed 'Scenario.uplugin')) -eq $false) 'the installed plugin requires the Toolset Registry, which the engine has'
Check ((Get-RegistryOptional $PluginFile) -eq $true) 'the source keeps it optional'

Write-Host '--- 2. Installing again keeps the previous version'
Start-Sleep -Seconds 2
Set-Content -LiteralPath (Join-Path $Installed 'marker.txt') -Value 'previous'
$run = Invoke-Installer ('-Project "{0}" -Yes' -f $Project)
Check ($run.Code -eq 0) 'the second install succeeds'
$Backups = @(Get-ChildItem -LiteralPath (Join-Path $ProjectDir 'Saved\Scenario') -Directory -Filter 'PreviousPlugin-*')
Check ($Backups.Count -eq 1 -and (Test-Path -LiteralPath (Join-Path $Backups[0].FullName 'marker.txt'))) 'the previous version is moved to Saved\Scenario'
Check (-not (Test-Path -LiteralPath (Join-Path $Installed 'marker.txt'))) 'and replaced'

Write-Host '--- 3. A failed build leaves the installed plugin alone'
Start-Sleep -Seconds 2
$env:SCENARIO_STUB_EXIT = '6'
$run = Invoke-Installer ('-Project "{0}" -Yes' -f $Project)
Remove-Item Env:\SCENARIO_STUB_EXIT
Check ($run.Code -ne 0) 'the installer reports the failure'
Check ($run.Output -match 'error C2065') 'and shows the first errors'
Check ($run.Output -match 'Nothing was installed') 'and says nothing was installed'
Check (Test-Path -LiteralPath (Join-Path $Installed 'Scenario.uplugin')) 'the installed plugin is still there'
Check (@(Get-ChildItem -LiteralPath (Join-Path $ProjectDir 'Saved\Scenario') -Directory -Filter 'PreviousPlugin-*').Count -eq 1) 'and was not moved'

Write-Host '--- 4. A project on an Unreal version that is not installed'
$Other = New-Project 'Old Game' '5.5'
$run = Invoke-Installer ('-Project "{0}" -Yes' -f $Other)
Check ($run.Code -ne 0) 'the installer stops'
Check ($run.Output -match 'Unreal 5\.5, which is not installed') 'and names the version'
Check (-not (Test-Path -LiteralPath (Join-Path (Split-Path -Parent $Other) 'Plugins'))) 'nothing is installed'

Write-Host '--- 5. An Unreal folder given with -EngineRoot, for a project that names none'
$Loose = New-Project 'Loose Game' ''
$run = Invoke-Installer ('-Project "{0}" -EngineRoot "{1}" -Yes' -f $Loose, $Engine)
Check ($run.Code -eq 0) 'the installer succeeds'
Check (Test-Path -LiteralPath (Join-Path (Split-Path -Parent $Loose) 'Plugins\Scenario\Scenario.uplugin')) 'the plugin is installed'

Write-Host '--- 6. A project that names no Unreal, with two installed: -Yes refuses to choose'
$Older = Join-Path $Work 'Epic Games\UE_5.7'
New-Item -ItemType Directory -Force -Path (Join-Path $Older 'Engine\Build') | Out-Null
Set-Content -LiteralPath (Join-Path $Older 'Engine\Build\Build.version') -Encoding ASCII -Value '{ "MajorVersion": 5, "MinorVersion": 7, "PatchVersion": 0 }'
$Both = @{ InstallationList = @($Listed.InstallationList[0], @{ InstallLocation = $Older; AppName = 'UE_5.7'; AppVersion = '5.7.0' }) }
Set-Content -LiteralPath $LauncherList -Encoding ASCII -Value ($Both | ConvertTo-Json -Depth 4)
$Unnamed = New-Project 'Unnamed Game' ''
$run = Invoke-Installer ('-Project "{0}" -Yes' -f $Unnamed)
Set-Content -LiteralPath $LauncherList -Encoding ASCII -Value ($Listed | ConvertTo-Json -Depth 4)
Check ($run.Code -ne 0 -and $run.Output -match '-EngineRoot <an Unreal folder>') 'the installer asks for -EngineRoot'
Check (-not (Test-Path -LiteralPath (Join-Path (Split-Path -Parent $Unnamed) 'Plugins'))) 'nothing is installed'

Write-Host '--- 7. Without -Project, -Yes refuses to guess'
$run = Invoke-Installer '-Yes'
Check ($run.Code -ne 0 -and $run.Output -match '-Project is needed') 'the installer asks for -Project'

Write-Host '--- 7. An Unreal without the Toolset Registry, as 5.6 and 5.7'
Start-Sleep -Seconds 2
Remove-Item -LiteralPath $Registry -Recurse -Force
$run = Invoke-Installer ('-Project "{0}" -EngineRoot "{1}" -Yes' -f $Loose, $Engine)
Check ($run.Code -eq 0) 'the installer succeeds'
Check ((Get-RegistryOptional (Join-Path (Split-Path -Parent $Loose) 'Plugins\Scenario\Scenario.uplugin')) -eq $true) 'the installed plugin keeps the Toolset Registry optional'

if ($script:Failures -gt 0) {
    Write-Host "$($script:Failures) check(s) failed." -ForegroundColor Red
    exit 1
}
Write-Host 'All installer checks passed.'
exit 0
