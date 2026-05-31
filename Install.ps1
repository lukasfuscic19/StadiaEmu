<#
.SYNOPSIS
    Install Stadia ViGEm — Google Stadia controller to virtual Xbox 360 (ViGEmBus).

.DESCRIPTION
    Installs ViGEmBus and HidHide (if missing), copies the app to
    %LOCALAPPDATA%\Programs\StadiaViGEm, and creates a Start Menu shortcut.

    Run as Administrator (required for driver installers).

    Bluetooth rumble is NOT supported on Windows 11 (OS limitation).
    USB rumble works. See README.md.

.PARAMETER SkipDrivers
    Only install/update the application files (skip ViGEmBus / HidHide download).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File Install.ps1
#>

param(
    [switch]$SkipDrivers
)

$ErrorActionPreference = "Stop"

$AppName     = "Stadia ViGEm"
$ExeName     = "stadia-vigem-x64.exe"
$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$BinDir      = Join-Path $ScriptDir "bin"
$SourceExe   = Join-Path $BinDir $ExeName
$InstallDir  = Join-Path $env:LOCALAPPDATA "Programs\StadiaViGEm"
$InstallBin  = Join-Path $InstallDir "bin"
$InstallExe  = Join-Path $InstallBin $ExeName
$LogPath     = Join-Path $InstallDir "Install.log"
$TempDir     = Join-Path $env:TEMP "StadiaViGEm-Install"

# Pinned release URLs (update when bumping dependencies)
$ViGEmBusUrl  = "https://github.com/ViGEm/ViGEmBus/releases/download/v1.22.0/ViGEmBusSetup_x64.msi"
$HidHideUrl   = "https://github.com/nefarius/HidHide/releases/download/v1.5.230.0/HidHide_1.5.230_x64.exe"
$HidHideCli   = "${env:ProgramFiles}\Nefarius Software Solutions\HidHide\x64\HidHideCLI.exe"

function Write-Log($msg) {
    $line = "[$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')] $msg"
    Write-Host $line
    if ($InstallDir -and (Test-Path (Split-Path $InstallDir))) {
        Add-Content -Path $LogPath -Value $line -ErrorAction SilentlyContinue
    }
}

function Abort($msg) {
    Write-Host ""
    Write-Host "ERROR: $msg" -ForegroundColor Red
    Read-Host "Press Enter to close"
    exit 1
}

function Test-IsAdmin {
    ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Test-ViGEmBusInstalled {
    $svc = Get-Service -Name "ViGEmBus" -ErrorAction SilentlyContinue
    return ($null -ne $svc)
}

function Test-HidHideInstalled {
    return (Test-Path $HidHideCli)
}

function Invoke-DownloadFile($url, $dest) {
    Write-Log "Downloading: $url"
    New-Item -ItemType Directory -Path (Split-Path $dest) -Force | Out-Null
    Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing
    if (-not (Test-Path $dest)) { throw "Download failed: $dest" }
    Write-Log "Saved: $dest"
}

function Install-ViGEmBus {
    if (Test-ViGEmBusInstalled) {
        Write-Log "ViGEmBus already installed."
        return
    }
    New-Item -ItemType Directory -Path $TempDir -Force | Out-Null
    $msi = Join-Path $TempDir "ViGEmBusSetup_x64.msi"
    Invoke-DownloadFile $ViGEmBusUrl $msi
    Write-Log "Installing ViGEmBus (silent)..."
    $p = Start-Process msiexec.exe -ArgumentList "/i `"$msi`" /quiet /norestart" -Wait -PassThru
    if ($p.ExitCode -ne 0 -and $p.ExitCode -ne 3010) {
        throw "ViGEmBus installer exit code: $($p.ExitCode)"
    }
    Start-Sleep -Seconds 2
    if (-not (Test-ViGEmBusInstalled)) {
        throw "ViGEmBus service not found after install. Reboot and run Install.ps1 again."
    }
    Write-Log "ViGEmBus installed."
}

function Install-HidHide {
    if (Test-HidHideInstalled) {
        Write-Log "HidHide already installed."
        return
    }
    New-Item -ItemType Directory -Path $TempDir -Force | Out-Null
    $setup = Join-Path $TempDir "HidHide_setup.exe"
    Invoke-DownloadFile $HidHideUrl $setup
    Write-Log "Installing HidHide (silent, may take a minute)..."
    $p = Start-Process $setup -ArgumentList "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART" -Wait -PassThru
    if ($p.ExitCode -ne 0) {
        Write-Log "HidHide installer exit code: $($p.ExitCode) (continuing if CLI exists)"
    }
    Start-Sleep -Seconds 3
    if (-not (Test-HidHideInstalled)) {
        Write-Host ""
        Write-Host "WARNING: HidHide CLI not found at expected path." -ForegroundColor Yellow
        Write-Host "  Install HidHide manually from: https://github.com/nefarius/HidHide/releases" -ForegroundColor Yellow
        Write-Host "  Without HidHide you may see duplicate controllers in games." -ForegroundColor Yellow
    } else {
        Write-Log "HidHide installed."
    }
}

function Install-Application {
    if (-not (Test-Path $SourceExe)) {
        Abort "Missing $SourceExe — build first (run build_quick.bat) or download a release zip."
    }

    New-Item -ItemType Directory -Path $InstallBin -Force | Out-Null
    Copy-Item -Path $SourceExe -Destination $InstallExe -Force
    Write-Log "Installed app: $InstallExe"

    # Start Menu shortcut
    $shell = New-Object -ComObject WScript.Shell
    $startMenu = [Environment]::GetFolderPath("Programs")
    $lnkPath = Join-Path $startMenu "$AppName.lnk"
    $shortcut = $shell.CreateShortcut($lnkPath)
    $shortcut.TargetPath = $InstallExe
    $shortcut.WorkingDirectory = $InstallBin
    $iconPath = Join-Path $ScriptDir "stadia-vigem\res\icon.ico"
    if (Test-Path $iconPath) { $shortcut.IconLocation = $iconPath }
    $shortcut.Description = "Stadia Controller ViGEm Adapter"
    $shortcut.Save()
    Write-Log "Start Menu shortcut: $lnkPath"
}

# ── Main ─────────────────────────────────────────────────────────────────────

Write-Host ""
Write-Host "=== $AppName Installer ===" -ForegroundColor Cyan
Write-Host ""

if (-not (Test-IsAdmin)) {
    Write-Host "Restarting with Administrator privileges (UAC)..." -ForegroundColor Yellow
    $args = "-ExecutionPolicy Bypass -File `"$($MyInvocation.MyCommand.Path)`""
    if ($SkipDrivers) { $args += " -SkipDrivers" }
    Start-Process powershell.exe -Verb RunAs -ArgumentList $args
    exit 0
}

New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
Start-Transcript -Path $LogPath -Append -Force | Out-Null

try {
    if (-not $SkipDrivers) {
        Install-ViGEmBus
        Install-HidHide
    } else {
        Write-Log "Skipping driver installers (-SkipDrivers)."
    }

    Install-Application

    Write-Host ""
    Write-Host "=== SUCCESS ===" -ForegroundColor Green
    Write-Host "  App:       $InstallExe" -ForegroundColor Green
    Write-Host "  Shortcut:  Start Menu -> $AppName" -ForegroundColor Green
    Write-Host ""
    Write-Host "Notes:" -ForegroundColor Yellow
    Write-Host "  - USB: input + vibration" -ForegroundColor Gray
    Write-Host "  - Bluetooth: input only (Windows 11 does not allow BT rumble)" -ForegroundColor Gray
    Write-Host "  - Reboot if ViGEmBus or HidHide were just installed" -ForegroundColor Gray
    Write-Host "  - Log: $LogPath" -ForegroundColor Gray
}
catch {
    Abort $_.Exception.Message
}
finally {
    Stop-Transcript | Out-Null
}

Read-Host "Press Enter to close"
