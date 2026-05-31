# Build release artifacts: portable zip + Inno Setup installer
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $Root

$Exe = Join-Path $Root "bin\stadia-vigem-x64.exe"
if (-not (Test-Path $Exe)) {
    throw "Missing $Exe. Run build_quick.bat first."
}

$ReleaseDir = Join-Path $Root "release"
$PortableDir = Join-Path $ReleaseDir "portable"
New-Item -ItemType Directory -Path $PortableDir -Force | Out-Null

Copy-Item $Exe (Join-Path $PortableDir "stadia-vigem-x64.exe") -Force
Copy-Item (Join-Path $Root "release\PORTABLE-README.txt") (Join-Path $PortableDir "README.txt") -Force

$PortableZip = Join-Path $ReleaseDir "StadiaViGEm-Portable-win64.zip"
if (Test-Path $PortableZip) { Remove-Item $PortableZip -Force }
Compress-Archive -Path (Join-Path $PortableDir "*") -DestinationPath $PortableZip -Force
Write-Host "Portable: $PortableZip"

$IsccCandidates = @(
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
    "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
)
$Iscc = $IsccCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $Iscc) {
    throw "Inno Setup ISCC.exe not found. Install with: winget install JRSoftware.InnoSetup"
}
& $Iscc (Join-Path $Root "installer\StadiaViGEm.iss")
Write-Host "Installer: $(Join-Path $ReleaseDir 'StadiaViGEm-Setup-x64.exe')"
