@echo off
setlocal EnableDelayedExpansion
chcp 65001 >nul

set "SCRIPT_DIR=%~dp0"
cd /d "%SCRIPT_DIR%"

echo ============================================================
echo  Instalare Firmware Deye Smart Energy (ESP32-S3 N16R8)
echo  Versiune: v1.0.0 (cu Dual-OTA si Rollback Automat)
echo ============================================================
echo.

REM 1. Cauta Python (ESP-IDF venv sau Python din sistem)
set "PYTHON_EXE="
if exist "C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe" (
    set "PYTHON_EXE=C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe"
) else (
    for /d %%D in ("C:\Espressif\python_env\idf*") do (
        if exist "%%D\Scripts\python.exe" set "PYTHON_EXE=%%D\Scripts\python.exe"
    )
    if "!PYTHON_EXE!"=="" (
        for /d %%D in ("%USERPROFILE%\.espressif\python_env\idf*") do (
            if exist "%%D\Scripts\python.exe" set "PYTHON_EXE=%%D\Scripts\python.exe"
        )
    )
)

if "!PYTHON_EXE!"=="" (
    python --version >nul 2>nul && set "PYTHON_EXE=python"
)
if "!PYTHON_EXE!"=="" (
    py --version >nul 2>nul && set "PYTHON_EXE=py"
)

if "!PYTHON_EXE!"=="" (
    echo [EROARE] Nu am gasit Python 3 instalat pe acest PC!
    echo.
    echo  Pasul 1: Descarca si instaleaza Python 3 de pe: https://www.python.org/downloads/
    echo  Pasul 2: IMPORTANT - La instalare bifeaza casuta "Add python.exe to PATH"!
    echo  Pasul 3: Dupa instalare, da din nou dublu-click pe flash_esp32_s3.bat.
    echo.
    pause
    exit /b 1
)

REM Verifica daca modulul esptool este instalat; daca nu, il instaleaza automat
"!PYTHON_EXE!" -m esptool version >nul 2>nul
if errorlevel 1 (
    echo [INFO] Se instaleaza automat utilitarul esptool...
    "!PYTHON_EXE!" -m pip install --upgrade esptool
    if errorlevel 1 (
        echo [EROARE] Nu s-a putut instala esptool. Ruleaza manual: pip install esptool
        pause
        exit /b 1
    )
)

set "FW_BIN="
if exist "build\deye_esp32_s3_bridge.bin" (
    set "FW_BIN=build\deye_esp32_s3_bridge.bin"
) else (
    for %%F in ("build\deye_esp32_s3_bridge_v*.bin") do set "FW_BIN=%%~fF"
    if "!FW_BIN!"=="" for %%F in ("deye_esp32_s3_bridge*.bin") do set "FW_BIN=%%~fF"
)
if "!FW_BIN!"=="" (
    echo [EROARE] Nu am gasit niciun fisier firmware ^(build\deye_esp32_s3_bridge.bin sau deye_esp32_s3_bridge_v*.bin^)!
    pause
    exit /b 1
)
echo [INFO] Firmware selectat: !FW_BIN!

REM 2. Detecteaza porturile COM disponibile (CH343, CP2102, CH340, USB Nativ ESP32-S3 / JTAG)
echo Porturi COM detectate pe PC:
set "DEFAULT_PORT="
set "LAST_PORT="
for /f "tokens=1,* delims=|" %%A in ('powershell -NoProfile -Command "$ports = Get-PnpDevice -Class Ports -Status OK | Sort-Object FriendlyName; foreach ($d in $ports) { if ($d.FriendlyName -match '\((COM\d+)\)') { $c = $matches[1]; Write-Output ('PORT|' + $c + ' : ' + $d.FriendlyName); if ($d.InstanceId -match '^USB\\' -or $d.FriendlyName -match 'CH343|CH340|CH341|CH9102|CP210|JTAG|USB Serial|ESP32') { Write-Output ('DEF|' + $c) } } }" 2^>nul') do (
    if "%%A"=="PORT" (
        echo   -^> %%B
        for /f "tokens=1 delims= " %%C in ("%%B") do set "LAST_PORT=%%C"
    )
    if "%%A"=="DEF" if "!DEFAULT_PORT!"=="" set "DEFAULT_PORT=%%B"
)
if "!DEFAULT_PORT!"=="" set "DEFAULT_PORT=!LAST_PORT!"
if "!DEFAULT_PORT!"=="" set "DEFAULT_PORT=COM7"
echo.

set /p "COM_PORT=Introdu portul COM al placutei ESP32-S3 (Enter pentru %DEFAULT_PORT%): "
if "%COM_PORT%"=="" set "COM_PORT=%DEFAULT_PORT%"
echo(%COM_PORT%| findstr /r "^[0-9][0-9]*$" >nul && set "COM_PORT=COM%COM_PORT%"

echo.
echo Scriu bootloader, partition table, ota_data si firmware (ota_0 + ota_1 backup rollback) pe %COM_PORT%...
echo.

"!PYTHON_EXE!" -m esptool --chip esp32s3 -p %COM_PORT% -b 460800 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m 0x0 "build\bootloader\bootloader.bin" 0x8000 "build\partition_table\partition-table.bin" 0xf000 "build\ota_data_initial.bin" 0x20000 "!FW_BIN!" 0x320000 "!FW_BIN!"

if errorlevel 1 (
    echo.
    echo [EROARE] Scrierea pe %COM_PORT% a esuat!
    echo Verifica:
    echo  1. Portul COM corect din Device Manager
    echo  2. Cablul USB sa fie conectat in portul USB/UART al placii ESP32-S3
    echo  3. Daca nu intra automat in boot: tine apasat BOOT, apasa scurt RST, apoi elibereaza BOOT.
    echo.
    pause
    exit /b 1
)

echo.
echo [SUCCES] Firmware Deye Smart Energy v1.0.0 incarcat cu succes pe %COM_PORT% ^(cu suport Dual-OTA Rollback^)!
echo Conecteaza-te acum la Wi-Fi "Deye-ESP32-Setup" ^(http://192.168.4.1^) pentru configurare.
echo.
pause
