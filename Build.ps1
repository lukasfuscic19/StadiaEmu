[CmdletBinding()]
param (
    [Parameter()][ValidateSet("DEBUG", "RELEASE")][string]$Configuration = "DEBUG",
    [Parameter()][ValidateSet("x86", "x64")][string]$Architecture = "x86",
    [Parameter()][bool]$SkipBuildToolsSetup = $false
)

$script:CommonFlags = @("/Zi", "/W4", "/EHsc", "/DWIN32", "/D_UNICODE", "/DUNICODE")
$script:DebugFlags = @( "/Od" )
$script:ReleaseFlags = @("/GL", "/O2")

function Import-Prerequisites {

    if ($SkipBuildToolsSetup -eq $true) {
        return
    }

    if (Get-Module -ListAvailable -Name VSSetup) {
        Update-Module -Name VSSetup
    } else {
        Install-Module -Name VSSetup -Scope CurrentUser -Force
    }

    Import-Module -Name VSSetup

    if (Get-Module -ListAvailable -Name WintellectPowerShell) {
        Update-Module -Name WintellectPowerShell
    } else {
        Install-Module -Name WintellectPowerShell -Scope CurrentUser -Force
    }

    Import-Module -Name WintellectPowerShell
}

function Invoke-BuildTools {
    param (
        $Architecture
    )
    
    if ($SkipBuildToolsSetup -eq $true) {
        return
    }

    $latestVsInstallationInfo = Get-VSSetupInstance -All | Sort-Object -Property InstallationVersion -Descending | Select-Object -First 1
    
    Invoke-CmdScript "$($latestVsInstallationInfo.InstallationPath)\VC\Auxiliary\Build\vcvarsall.bat" $Architecture
}

function Invoke-Build-libstadia {
    param (
        $Architecture
    )

    $OutputName = "libstadia-$Architecture.lib"
    $Flags = If ($Configuration -eq "DEBUG") {$script:DebugFlags} else {$script:ReleaseFlags}

    $StopWatch = New-Object -TypeName System.Diagnostics.Stopwatch

    Write-Host "*** ${OutputName}: Build started ***"
    Write-Host

    $StopWatch.Start()

    & "cl.exe" /c $Flags $CommonFlags /Ilibstadia/include /Foobj/libstadia/ libstadia/src/*.c
    & "lib.exe" /out:bin/$OutputName obj/libstadia/*.obj 

    $StopWatch.Stop()

    Write-Host
    Write-Host "*** ${OutputName}: Build finished in $($StopWatch.Elapsed) ***"
}

function Invoke-Build-Stadia-ViGEm {
    param (
        $Architecture
    )

    $OutputName = "stadia-vigem-$Architecture.exe"
    $Flags = If ($Configuration -eq "DEBUG") {$script:DebugFlags} else {$script:ReleaseFlags}
    $LibraryPath = "bin/libstadia-$Architecture.lib"

    $StopWatch = New-Object -TypeName System.Diagnostics.Stopwatch

    Write-Host "*** ${OutputName}: Build started ***"
    Write-Host

    $StopWatch.Start()

    & "rc.exe" /foobj/stadia-vigem/stadia-vigem.res stadia-vigem/res/res.rc
    & "cl.exe" $Flags $CommonFlags /Ilibstadia/include /IViGEmClient/include /Istadia-vigem/include /Foobj/stadia-vigem/ /Febin/$OutputName ViGEmClient/src/*.cpp obj/stadia-vigem/stadia-vigem.res stadia-vigem/src/*.c $LibraryPath User32.lib Ole32.lib OleAut32.lib SetupAPI.lib Hid.lib

    $StopWatch.Stop()

    Write-Host
    Write-Host "*** ${OutputName}: Build finished in $($StopWatch.Elapsed) ***"
}

# Entry

New-Item -Path "bin" -ItemType Directory -Force > $null
New-Item -Path "obj" -ItemType Directory -Force > $null
New-Item -Path "obj/libstadia" -ItemType Directory -Force > $null
New-Item -Path "obj/stadia-vigem" -ItemType Directory -Force > $null

Import-Prerequisites

Write-Host "-- Build started. Configuration: $Configuration, Architecture: $Architecture --"

if ($Architecture -eq "x86" -Or $Architecture -eq "ALL") {
    Invoke-BuildTools -Architecture "x86"
    Invoke-Build-libstadia -Architecture "x86"
    Invoke-Build-Stadia-ViGEm -Architecture "x86"
}

if ($Architecture -eq "x64" -Or $Architecture -eq "ALL") {
    Invoke-BuildTools -Architecture "x64"
    Invoke-Build-libstadia -Architecture "x64"
    Invoke-Build-Stadia-ViGEm -Architecture "x64"
}

Write-Host "-- Build completed. --"
