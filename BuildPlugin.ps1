# Build LoreSourceControl for every supported engine and zip each result into Build\.
# Each engine accepts only a window of compiler versions, so the matching toolchain is
# written to the user BuildConfiguration.xml before each build and the original restored after.
#   4.27 through 5.2 -> Visual Studio 2019 (MSVC 14.29)
#   5.3 and 5.4      -> Visual Studio 2022 with the 14.38 toolset
#   5.5 through 5.8   -> the newest installed Visual Studio 2022 toolset
# 4.27 builds by calling AutomationTool.exe directly, because its RunUAT.bat fails to locate the tool on this install.

$ErrorActionPreference = 'Continue'

$PluginName = 'LoreSourceControl'
$EngineBase = 'E:\Program Files\Epic Games'
$Platform   = 'Win64'
$Root       = $PSScriptRoot
$UbtConfig  = Join-Path $env:APPDATA 'Unreal Engine\UnrealBuildTool\BuildConfiguration.xml'

$Engines = @(
    @{ V = '4.27'; Compiler = 'VisualStudio2019'; CompilerVersion = $null }
    @{ V = '5.0';  Compiler = 'VisualStudio2019'; CompilerVersion = $null }
    @{ V = '5.1';  Compiler = 'VisualStudio2019'; CompilerVersion = $null }
    @{ V = '5.2';  Compiler = 'VisualStudio2019'; CompilerVersion = $null }
    @{ V = '5.3';  Compiler = 'VisualStudio2022'; CompilerVersion = '14.38.33130' }
    @{ V = '5.4';  Compiler = 'VisualStudio2022'; CompilerVersion = '14.38.33130' }
    @{ V = '5.5';  Compiler = $null;              CompilerVersion = $null }
    @{ V = '5.6';  Compiler = $null;              CompilerVersion = $null }
    @{ V = '5.7';  Compiler = $null;              CompilerVersion = $null }
    @{ V = '5.8';  Compiler = $null;              CompilerVersion = $null }
)

$Uplugin = Join-Path $Root "$PluginName.uplugin"
$Version = (Get-Content $Uplugin -Raw | ConvertFrom-Json).VersionName
$BuildDir = Join-Path $Root 'Build'
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

$HadConfig = Test-Path $UbtConfig
if ($HadConfig) { Copy-Item $UbtConfig "$UbtConfig.lorebak" -Force }

function Write-UbtConfig($Compiler, $CompilerVersion) {
    $inner = ''
    if ($Compiler) {
        $inner = "  <WindowsPlatform><Compiler>$Compiler</Compiler>"
        if ($CompilerVersion) { $inner += "<CompilerVersion>$CompilerVersion</CompilerVersion>" }
        $inner += "</WindowsPlatform>`r`n"
    }
    $xml = "<?xml version=`"1.0`" encoding=`"utf-8`" ?>`r`n<Configuration xmlns=`"https://www.unrealengine.com/BuildConfiguration`">`r`n$inner</Configuration>`r`n"
    New-Item -ItemType Directory -Force -Path (Split-Path $UbtConfig) | Out-Null
    Set-Content -LiteralPath $UbtConfig -Value $xml -Encoding UTF8
}

Write-Host "Building $PluginName v$Version for: $($Engines.V -join ', ')"
$results = @()

foreach ($e in $Engines) {
    $ver = $e.V
    $engineDir = Join-Path $EngineBase "UE_$ver"
    $runUat = Join-Path $engineDir 'Engine\Build\BatchFiles\RunUAT.bat'
    if (-not (Test-Path $runUat)) {
        Write-Host "UE $ver not found at $engineDir, skipping."
        $results += "UE $ver : SKIP (engine not found)"
        continue
    }

    Write-UbtConfig $e.Compiler $e.CompilerVersion
    $toolLabel = if ($e.Compiler) { "$($e.Compiler) $($e.CompilerVersion)".Trim() } else { 'newest toolset' }

    $out = Join-Path $env:TEMP "$PluginName-build\$ver"
    if (Test-Path $out) { Remove-Item $out -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $out | Out-Null
    $log = Join-Path $BuildDir "BuildLog-$ver.txt"

    Write-Host "==== Building UE $ver ($toolLabel) ===="
    if ($ver -eq '4.27') {
        $autoTool = Join-Path $engineDir 'Engine\Binaries\DotNET\AutomationTool.exe'
        & $autoTool BuildPlugin "-Plugin=$Uplugin" "-Package=$out" "-TargetPlatforms=$Platform" *> $log
    }
    else {
        & $runUat BuildPlugin "-Plugin=$Uplugin" "-Package=$out" "-TargetPlatforms=$Platform" -Rocket *> $log
    }

    if (Test-Path (Join-Path $out "$PluginName.uplugin")) {
        Remove-Item (Join-Path $out 'Intermediate') -Recurse -Force -ErrorAction SilentlyContinue
        $zip = Join-Path $BuildDir "$PluginName-$Version-UE$ver.zip"
        Compress-Archive -Path (Join-Path $out '*') -DestinationPath $zip -Force
        Write-Host "UE $ver : OK"
        $results += "UE $ver : OK -> $(Split-Path $zip -Leaf)"
    }
    else {
        Write-Host "UE $ver : BUILD FAILED (see $log)"
        $results += "UE $ver : FAIL (see BuildLog-$ver.txt)"
    }
}

if ($HadConfig) { Copy-Item "$UbtConfig.lorebak" $UbtConfig -Force; Remove-Item "$UbtConfig.lorebak" -Force }
else { Remove-Item $UbtConfig -Force -ErrorAction SilentlyContinue }
Remove-Item (Join-Path $env:TEMP "$PluginName-build") -Recurse -Force -ErrorAction SilentlyContinue

Write-Host "`n===== SUMMARY ====="
$results | ForEach-Object { Write-Host $_ }
