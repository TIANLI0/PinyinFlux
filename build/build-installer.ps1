# SPDX-License-Identifier: MIT
<#
.SYNOPSIS
  Builds PinyinFlux and produces the Windows installer (PinyinFlux-Setup-<version>-win-x64.exe).

.DESCRIPTION
  1. Builds the native core (build.ps1, MSVC + CMake).
  2. Publishes the WinUI settings app self-contained for win-x64 into artifacts\PinyinFlux-win-x64;
     the core is copied in by the app project.
  3. Builds the installer with FluentSetup (installer\fluentsetup.json). The fluentsetup CLI comes from
     $env:FLUENTSETUP or PATH (https://github.com/TIANLI0/FluentSetup/releases; CI uses the TIANLI0/FluentSetup action).

.EXAMPLE
  pwsh build\build-installer.ps1
#>
[CmdletBinding()]
param(
    [string] $Configuration = "Release",
    [string] $Version
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not $Version) {
    $props = Get-Content (Join-Path $root "Directory.Build.props") -Raw
    $Version = [regex]::Match($props, "<VersionPrefix>([^<]+)</VersionPrefix>").Groups[1].Value
}
$numeric = ($Version -split "[-+]")[0]
while (($numeric -split "\.").Count -lt 4) { $numeric += ".0" }
Write-Host "PinyinFlux $Version ($numeric)"

& (Join-Path $root "build.ps1") -Config Release
if ($LASTEXITCODE -ne 0) { throw "Native core build failed." }

$publish = Join-Path $root "artifacts\PinyinFlux-win-x64"
# Replacing the folder under a running copy would delete only the files it does not lock.
$running = Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith("$publish\", [StringComparison]::OrdinalIgnoreCase) }
if ($running) { throw "PinyinFlux is running from $publish (PID $($running.Id -join ', ')). Close it first; the build replaces that folder." }
$publish = [IO.Path]::GetFullPath($publish)
$artifactRoot = [IO.Path]::GetFullPath((Join-Path $root "artifacts")) + [IO.Path]::DirectorySeparatorChar
if (-not $publish.StartsWith($artifactRoot, [StringComparison]::OrdinalIgnoreCase)) { throw "Unexpected publish directory: $publish" }
if (Test-Path -LiteralPath $publish) { Remove-Item -LiteralPath $publish -Recurse -Force }
dotnet publish src/PinyinFlux.App -c $Configuration -r win-x64 -p:Platform=x64 `
    -p:VersionPrefix=$((($Version -split "[-+]")[0])) -p:VersionSuffix="$(if ($Version -match '-(.+)$') { $Matches[1] } else { '' })" `
    -o $publish --nologo
if ($LASTEXITCODE -ne 0) { throw "dotnet publish failed." }

foreach ($required in "PinyinFlux.exe", "PinyinFlux.pri", "PinyinFluxCore.exe", "PinyinFluxInject.dll", "THIRD_PARTY_NOTICES.txt", "Microsoft.WindowsAppRuntime.dll", "Microsoft.ui.xaml.dll", "LICENSE", "Assets\PinyinFlux.ico", "Assets\PinyinFlux-48.png") {
    if (-not (Test-Path (Join-Path $publish $required))) { throw "Publish output is missing $required." }
}

$fluentsetup = if ($env:FLUENTSETUP) { $env:FLUENTSETUP } else { (Get-Command fluentsetup -ErrorAction SilentlyContinue).Source }
if (-not $fluentsetup) { throw "fluentsetup not found. Install FluentSetup (https://github.com/TIANLI0/FluentSetup/releases) or set FLUENTSETUP to fluentsetup.exe." }

$output = Join-Path $root "artifacts\installer"
$setupPath = Join-Path $output "PinyinFlux-Setup-$Version-win-x64.exe"
$setupRunning = Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path -eq $setupPath }
if ($setupRunning) { throw "The previous installer is still running (PID $($setupRunning.Id -join ', ')). Close it first; the build overwrites $setupPath." }
& $fluentsetup build installer\fluentsetup.json -D "Version=$Version" -o $output
if ($LASTEXITCODE -ne 0) { throw "Building the installer failed." }

$setup = Get-ChildItem $output -Filter "PinyinFlux-Setup-$Version-win-x64.exe" | Select-Object -First 1
$hash = (Get-FileHash $setup.FullName -Algorithm SHA256).Hash
"{0}  {1}" -f $hash, $setup.Name | Set-Content (Join-Path $output "$($setup.Name).sha256") -Encoding ascii
Write-Host ("Installer: {0} ({1:N1} MB)`nSHA-256:   {2}" -f $setup.FullName, ($setup.Length / 1MB), $hash)
