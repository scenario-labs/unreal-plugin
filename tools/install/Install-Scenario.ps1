# Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.
<#
.SYNOPSIS
Compiles the Scenario plugin for an installed Unreal Engine and installs it into a project.

.DESCRIPTION
Install-Windows.bat at the top of the download starts this script, so people can
double-click it. It checks that Visual Studio's C++ tools are installed, asks for
the project, finds the Unreal version that project uses, compiles the plugin with
Unreal's RunUAT BuildPlugin and copies the result to <Project>\Plugins\Scenario.
A plugin already there is moved to <Project>\Saved\Scenario\ first, never deleted.
The full build log is written to <Project>\Saved\Scenario\ as well.

Runs on Windows PowerShell 5.1, which every Windows 10 and 11 has. Keep this file
ASCII: Windows PowerShell reads a file without a byte order mark as ANSI.

.PARAMETER Project
The project's .uproject file. Without it, a file dialog asks for it.

.PARAMETER EngineRoot
An Unreal Engine folder, such as "C:\Program Files\Epic Games\UE_5.8". Without it,
the installer uses the Unreal version the project names, or asks.

.PARAMETER Yes
Ask nothing: when a choice is needed, fail instead. For scripts and tests.

.EXAMPLE
Install-Windows.bat -Project "D:\Games\MyGame\MyGame.uproject"
#>
[CmdletBinding()]
param(
    [string]$Project,
    [string]$EngineRoot,
    [switch]$Yes
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$Supported = @('5.6', '5.7', '5.8')
$RepoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$PluginFile = Join-Path $RepoRoot 'Plugins\Scenario\Scenario.uplugin'
$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$VisualStudioGuide = 'https://dev.epicgames.com/documentation/en-us/unreal-engine/setting-up-visual-studio-development-environment-for-cplusplus-projects-in-unreal-engine'
$Troubleshooting = 'https://github.com/scenario-labs/unreal-plugin/blob/main/docs/TROUBLESHOOTING.md'

function Say([string]$Text) { Write-Host $Text }

function Step([string]$Text) {
    Write-Host ''
    Write-Host $Text -ForegroundColor Cyan
}

function Fail([string]$Text) {
    Write-Host ''
    Write-Host $Text -ForegroundColor Red
    Write-Host 'Nothing was installed.'
    exit 1
}

function Read-Json([string]$Path) {
    try {
        return (Get-Content -LiteralPath $Path -Raw -ErrorAction Stop | ConvertFrom-Json)
    } catch {
        return $null
    }
}

# A property of a JSON or registry object, or $null (strict mode refuses a missing one).
function Get-Field($Object, [string]$Name) {
    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function New-Engine([string]$Root) {
    if (-not $Root) { return $null }
    $full = $Root.Trim().TrimEnd('\', '/')
    $runUat = Join-Path $full 'Engine\Build\BatchFiles\RunUAT.bat'
    if (-not (Test-Path -LiteralPath $runUat -PathType Leaf)) { return $null }
    $build = Read-Json (Join-Path $full 'Engine\Build\Build.version')
    $version = '{0}.{1}' -f (Get-Field $build 'MajorVersion'), (Get-Field $build 'MinorVersion')
    return [pscustomobject]@{ Root = $full; Version = $version; RunUat = $runUat }
}

# Unreal folders registered by the Epic Games Launcher, the registry and source builds, plus the default folder.
function Find-Engines {
    $roots = New-Object System.Collections.Generic.List[string]
    # SCENARIO_LAUNCHER_LIST stands in for the Launcher's list in tests.
    $list = $env:SCENARIO_LAUNCHER_LIST
    if (-not $list) { $list = Join-Path $env:ProgramData 'Epic\UnrealEngineLauncher\LauncherInstalled.dat' }
    $launcher = Read-Json $list
    foreach ($item in @(Get-Field $launcher 'InstallationList')) {
        if ([string](Get-Field $item 'AppName') -match '^UE_\d+\.\d+$') { $roots.Add([string](Get-Field $item 'InstallLocation')) }
    }
    foreach ($key in @(Get-ChildItem -Path 'HKLM:\SOFTWARE\EpicGames\Unreal Engine' -ErrorAction SilentlyContinue)) {
        $roots.Add([string](Get-Field (Get-ItemProperty -LiteralPath $key.PSPath -ErrorAction SilentlyContinue) 'InstalledDirectory'))
    }
    $builds = Get-ItemProperty -Path 'HKCU:\SOFTWARE\Epic Games\Unreal Engine\Builds' -ErrorAction SilentlyContinue
    if ($null -ne $builds) {
        foreach ($property in $builds.PSObject.Properties) {
            if ($property.Name -notmatch '^PS(Path|ParentPath|ChildName|Drive|Provider)$') { $roots.Add([string]$property.Value) }
        }
    }
    if ($env:ProgramFiles) {
        foreach ($folder in @(Get-ChildItem -Path (Join-Path $env:ProgramFiles 'Epic Games') -Directory -Filter 'UE_*' -ErrorAction SilentlyContinue)) {
            $roots.Add($folder.FullName)
        }
    }
    $seen = @{}
    $engines = @()
    foreach ($root in $roots) {
        $engine = New-Engine $root
        if ($null -eq $engine -or $seen.ContainsKey($engine.Root.ToLowerInvariant())) { continue }
        $seen[$engine.Root.ToLowerInvariant()] = $true
        $engines += $engine
    }
    return $engines
}

# The source build a project names by its id in the registry, or $null.
function Find-SourceBuild([string]$Id) {
    $builds = Get-ItemProperty -Path 'HKCU:\SOFTWARE\Epic Games\Unreal Engine\Builds' -ErrorAction SilentlyContinue
    return New-Engine ([string](Get-Field $builds $Id))
}

function Test-CppTools {
    $programFiles = ${env:ProgramFiles(x86)}
    if (-not $programFiles) { $programFiles = $env:ProgramFiles }
    $vswhere = Join-Path $programFiles 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) { return $false }
    $found = & $vswhere -latest -products '*' -requires 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64' -property installationPath
    return [bool]$found
}

function Select-ProjectFile {
    Add-Type -AssemblyName System.Windows.Forms
    $owner = New-Object System.Windows.Forms.Form
    $owner.TopMost = $true
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = 'Choose your Unreal project (.uproject)'
    $dialog.Filter = 'Unreal project (*.uproject)|*.uproject'
    $projects = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Unreal Projects'
    if (Test-Path -LiteralPath $projects) { $dialog.InitialDirectory = $projects }
    try {
        if ($dialog.ShowDialog($owner) -ne [System.Windows.Forms.DialogResult]::OK) { return $null }
        return $dialog.FileName
    } finally {
        $owner.Dispose()
    }
}

function Select-Engine($Engines) {
    if ($Engines.Count -eq 1) { return $Engines[0] }
    $sorted = @($Engines | Sort-Object { [version]$_.Version } -Descending)
    if ($Yes) { return $sorted[0] }
    Say 'Several Unreal versions are installed:'
    for ($index = 0; $index -lt $sorted.Count; $index++) {
        Say ('  {0}) Unreal {1}  ({2})' -f ($index + 1), $sorted[$index].Version, $sorted[$index].Root)
    }
    while ($true) {
        $answer = Read-Host ('Type the number of the one your project uses (1-{0})' -f $sorted.Count)
        $number = 0
        if ([int]::TryParse($answer, [ref]$number) -and $number -ge 1 -and $number -le $sorted.Count) { return $sorted[$number - 1] }
    }
}

# New text in the build log since the last call; the build still writes to it.
function Read-NewLog([string]$Path, [ref]$Position) {
    if (-not (Test-Path -LiteralPath $Path)) { return @() }
    try {
        $stream = [System.IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
    } catch {
        return @()
    }
    try {
        if ($stream.Length -le $Position.Value) { return @() }
        $null = $stream.Seek($Position.Value, 'Begin')
        $text = (New-Object System.IO.StreamReader($stream)).ReadToEnd()
        $Position.Value = $stream.Length
        return @($text -split "`r?`n")
    } finally {
        $stream.Dispose()
    }
}

Say 'Scenario for Unreal Engine: compile and install'
if (-not (Test-Path -LiteralPath $PluginFile -PathType Leaf)) {
    Fail 'Plugins\Scenario\Scenario.uplugin is not next to this installer. Unzip the whole download, then run Install-Windows.bat from the folder it made.'
}
$PluginVersion = [string](Get-Field (Read-Json $PluginFile) 'VersionName')

# 1. The C++ tools Unreal compiles plugins with.
if (-not (Test-CppTools)) {
    Write-Host ''
    Write-Host "Unreal compiles plugins with Visual Studio's C++ tools, and they are not installed." -ForegroundColor Red
    Say 'Install Visual Studio Community (free) with the "Game development with C++" workload,'
    Say 'then run this installer again. Epic lists what each Unreal version needs:'
    Say "  $VisualStudioGuide"
    if (-not $Yes) {
        $answer = Read-Host 'Open that page in your browser? (Y/n)'
        if ($answer -notmatch '^[Nn]') { Start-Process $VisualStudioGuide }
    }
    Fail 'Install the C++ tools first.'
}

# 2. The project.
if (-not $Project) {
    if ($Yes) { Fail '-Project is needed with -Yes.' }
    Step 'Choose your Unreal project (.uproject) in the window that opens.'
    $Project = Select-ProjectFile
    if (-not $Project) { Fail 'No project was chosen.' }
}
if (-not (Test-Path -LiteralPath $Project -PathType Leaf) -or [System.IO.Path]::GetExtension($Project) -ne '.uproject') {
    Fail "This is not an Unreal project file (.uproject): $Project"
}
$Project = (Resolve-Path -LiteralPath $Project).Path
$ProjectDir = Split-Path -Parent $Project
$Destination = Join-Path $ProjectDir 'Plugins\Scenario'
if ($PluginFile.StartsWith($Destination + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
    Fail 'This download is inside the project''s Plugins\Scenario folder. Unzip it somewhere else, such as Downloads, and run the installer from there.'
}
Say "Project: $Project"
$Open = @(Get-CimInstance -ClassName Win32_Process -Filter "Name LIKE 'UnrealEditor%'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -and $_.CommandLine.IndexOf($Project, [System.StringComparison]::OrdinalIgnoreCase) -ge 0 })
if ($Open.Count -gt 0) { Fail 'Unreal Editor has this project open. Close it, then run the installer again.' }

# 3. The Unreal version the project uses.
$Association = [string](Get-Field (Read-Json $Project) 'EngineAssociation')
if ($EngineRoot) {
    $Engine = New-Engine $EngineRoot
    if ($null -eq $Engine) { Fail "No Unreal Engine in $EngineRoot (Engine\Build\BatchFiles\RunUAT.bat is missing)." }
} elseif ($Association -match '^\{?[0-9A-Fa-f-]{36}\}?$') {
    $Engine = Find-SourceBuild $Association
    if ($null -eq $Engine) { Fail "The project uses an Unreal source build ($Association) that is not registered on this computer. Run the installer with -EngineRoot <its folder>." }
} else {
    $Engines = @(Find-Engines)
    if ($Association) {
        $Matching = @($Engines | Where-Object { $_.Version -eq $Association })
        if ($Matching.Count -eq 0) {
            Fail "The project uses Unreal $Association, which is not installed here. Install it with the Epic Games Launcher, or run the installer with -EngineRoot <an Unreal folder>."
        }
        $Engine = $Matching[0]
    } else {
        $Usable = @($Engines | Where-Object { $Supported -contains $_.Version })
        if ($Usable.Count -eq 0) {
            Fail ('No Unreal {0} found. Install Unreal with the Epic Games Launcher, or run the installer with -EngineRoot <an Unreal folder>.' -f ($Supported -join ', '))
        }
        $Engine = Select-Engine $Usable
    }
}
if ($Supported -notcontains $Engine.Version) {
    Fail ('Scenario {0} supports Unreal {1}; the project uses Unreal {2}.' -f $PluginVersion, ($Supported -join ', '), $Engine.Version)
}
Say ('Unreal {0}: {1}' -f $Engine.Version, $Engine.Root)

# 4. Compile.
$LogDir = Join-Path $ProjectDir 'Saved\Scenario'
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$Log = Join-Path $LogDir "Install-$Stamp.log"
# A short folder, since the build nests deep paths under it and Windows paths stop at 260 characters.
# Not %TEMP%, which can hold 8.3 short names: Unreal's build accelerator expects one spelling of each path.
$StageRoot = $env:LOCALAPPDATA
if (-not $StageRoot) { $StageRoot = [System.IO.Path]::GetTempPath() }
$Stage = Join-Path $StageRoot "Scenario\SB-$Stamp"
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Stage) | Out-Null
Step ('Compiling Scenario {0} for Unreal {1}. This takes a few minutes.' -f $PluginVersion, $Engine.Version)
Say "Full log: $Log"
# cmd /s keeps the quotes around each argument: RunUAT.bat is a batch file, the paths can hold spaces, and cmd splits words at "=".
$Arguments = '/d /s /c ""{0}" BuildPlugin "-Plugin={1}" "-Package={2}" "-TargetPlatforms=Win64" > "{3}" 2>&1"' -f $Engine.RunUat, $PluginFile, $Stage, $Log
$Process = Start-Process -FilePath $env:ComSpec -ArgumentList $Arguments -NoNewWindow -PassThru
$null = $Process.Handle
$Position = 0
$Problems = New-Object System.Collections.Generic.List[string]
while ($true) {
    $done = $Process.HasExited
    foreach ($line in (Read-NewLog $Log ([ref]$Position))) {
        if ($line -match '^\[\d+/\d+\]') {
            Say ('  ' + $line)
        } elseif ($line -match '(^|\s)(error|ERROR)\b|: fatal error') {
            $Problems.Add($line)
        }
    }
    if ($done) { break }
    Start-Sleep -Seconds 2
}
$Process.WaitForExit()
$Code = $Process.ExitCode
if ($Code -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $Stage 'Scenario.uplugin') -PathType Leaf)) {
    if ($Problems.Count -gt 0) {
        Write-Host ''
        Say 'The first errors:'
        $Problems | Select-Object -First 10 | ForEach-Object { Say ('  ' + $_) }
    }
    Remove-Item -LiteralPath $Stage -Recurse -Force -ErrorAction SilentlyContinue
    Fail ("The build failed (exit code {0}). The full log is {1}.`nMost often a Visual Studio component or the Windows SDK is missing; see {2}" -f $Code, $Log, $Troubleshooting)
}

# 5. Install.
Step "Installing into $Destination"
if (Test-Path -LiteralPath $Destination) {
    $Backup = Join-Path $LogDir "PreviousPlugin-$Stamp"
    try {
        Move-Item -LiteralPath $Destination -Destination $Backup -ErrorAction Stop
    } catch {
        Fail "Could not move the current Plugins\Scenario folder aside. Close Unreal Editor and run the installer again. ($($_.Exception.Message))"
    }
    Say "The previous version is now in $Backup"
}
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Destination) | Out-Null
Copy-Item -LiteralPath $Stage -Destination $Destination -Recurse
Remove-Item -LiteralPath $Stage -Recurse -Force -ErrorAction SilentlyContinue

Step ('Scenario {0} is installed for Unreal {1}.' -f $PluginVersion, $Engine.Version)
Say "Open $($Project): the Scenario button is in the level editor toolbar."
exit 0
