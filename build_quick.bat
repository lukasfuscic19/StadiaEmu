@echo off
setlocal EnableDelayedExpansion

rem Try VS 2022 Community, then Enterprise, then Professional
set "VCVARS="
for %%E in (Community Enterprise Professional BuildTools) do (
    set "CAND=C:\Program Files\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat"
    if exist "!CAND!" set "VCVARS=!CAND!"
)
if not defined VCVARS (
    echo ERROR: Visual Studio 2022 vcvars64.bat not found.
    exit /b 1
)
call "%VCVARS%" x64

set OBJ=obj\libstadia
set BIN=bin

if not exist %OBJ% mkdir %OBJ%
if not exist %BIN% mkdir %BIN%
if not exist obj\stadia-vigem mkdir obj\stadia-vigem

echo === Building libstadia (C files) ===
cl.exe /c /Zi /W4 /EHsc /DWIN32 /D_UNICODE /DUNICODE /Od ^
    /Ilibstadia/include ^
    /Foobj/libstadia/ ^
    libstadia/src/utils.c ^
    libstadia/src/hid.c ^
    libstadia/src/stadia.c
if errorlevel 1 goto :error

echo === Building gatt.cpp ===
cl.exe /c /Zi /W4 /EHsc /DWIN32 /D_UNICODE /DUNICODE /Od /std:c++17 ^
    /Ilibstadia/include ^
    /Foobj/libstadia/gatt.obj ^
    libstadia/src/gatt.cpp
if errorlevel 1 goto :error

echo === Linking libstadia-x64.lib ===
lib.exe /out:bin/libstadia-x64.lib obj/libstadia/*.obj
if errorlevel 1 goto :error

echo === Building stadia-vigem resource ===
rc.exe /foobj/stadia-vigem/stadia-vigem.res stadia-vigem/res/res.rc
if errorlevel 1 goto :error

echo === Building stadia-vigem-x64.exe ===
cl.exe /Zi /W4 /EHsc /DWIN32 /D_UNICODE /DUNICODE /Od ^
    /Ilibstadia/include /IViGEmClient/include /Istadia-vigem/include ^
    /Foobj/stadia-vigem/ ^
    /Febin/stadia-vigem-x64.exe ^
    ViGEmClient/src/*.cpp ^
    stadia-vigem/src/*.c ^
    obj/stadia-vigem/stadia-vigem.res ^
    bin/libstadia-x64.lib ^
    User32.lib Ole32.lib OleAut32.lib windowsapp.lib SetupAPI.lib BluetoothApis.lib Hid.lib
if errorlevel 1 goto :error

echo.
echo === BUILD SUCCEEDED ===
echo Output: bin\stadia-vigem-x64.exe
echo Install: powershell -ExecutionPolicy Bypass -File Install.ps1
goto :end

:error
echo.
echo === BUILD FAILED ===
exit /b 1

:end
