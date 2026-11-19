@echo off
setlocal
rem System tools come from the system folder first, also inside for /f commands where quoted program paths do not work
set "PATH=%SystemRoot%\System32;%PATH%"

set "SC=%SystemRoot%\System32\sc.exe"
set "REGEXE=%SystemRoot%\System32\reg.exe"
set "SETTINGS_KEY=HKLM\SOFTWARE\NyxPlay Host"

rem Rule for the start type of the service:
rem   - "Run at Startup" selected for the first time (first installation, or the component was not installed
rem     before): the installer calls this script with "force" and the service always starts with the system.
rem   - "Run at Startup" was already installed and the user changed the start type since (restored by
rem     install-service.bat during an upgrade): that choice is kept.
if /i "%~1"=="force" goto set_auto

rem Keep the start type the user chose before an upgrade (restored by install-service.bat)
for /f "tokens=3" %%i in ('reg.exe query "%SETTINGS_KEY%" /v StartTypeRestored /reg:64 2^>nul') do (
    if "%%i"=="0x1" (
        echo Keeping the previously selected service start type.
        exit /b 0
    )
)

:set_auto
rem Set the service to auto-start
"%SC%" config NyxPlayHostService start= auto
if errorlevel 1 exit /b 1
"%REGEXE%" add "%SETTINGS_KEY%" /v StartTypeRestored /t REG_DWORD /d 0 /f /reg:64 >nul 2>&1
exit /b 0
