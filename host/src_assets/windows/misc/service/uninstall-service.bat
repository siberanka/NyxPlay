@echo off
setlocal
rem System tools come from the system folder first, also inside for /f commands where quoted program paths do not work
set "PATH=%SystemRoot%\System32;%PATH%"

set "SERVICE_NAME=NyxPlayHostService"
set "SC=%SystemRoot%\System32\sc.exe"
set "REGEXE=%SystemRoot%\System32\reg.exe"
set "SETTINGS_KEY=HKLM\SOFTWARE\NyxPlay Host"
set "SERVICE_KEY=HKLM\SYSTEM\CurrentControlSet\Services\%SERVICE_NAME%"

rem Save the current service start type if the service exists. It is read from the registry
rem because the text printed by sc.exe depends on the system language.
"%SC%" qc %SERVICE_NAME% >nul 2>&1
if errorlevel 1 goto :stop_and_delete

set "START_VALUE="
for /f "tokens=3" %%i in ('reg.exe query "%SERVICE_KEY%" /v Start 2^>nul') do set "START_VALUE=%%i"
set "DELAYED_VALUE=0x0"
for /f "tokens=3" %%i in ('reg.exe query "%SERVICE_KEY%" /v DelayedAutostart 2^>nul') do set "DELAYED_VALUE=%%i"

set "CONTENT="
if "%START_VALUE%"=="0x2" (
    set "CONTENT=2"
    if "%DELAYED_VALUE%"=="0x1" set "CONTENT=2-delayed"
)
if "%START_VALUE%"=="0x3" set "CONTENT=3"
if "%START_VALUE%"=="0x4" set "CONTENT=4"

if defined CONTENT "%REGEXE%" add "%SETTINGS_KEY%" /v ServiceStartType /t REG_SZ /d "%CONTENT%" /f /reg:64 >nul 2>&1

:stop_and_delete
rem Stop the service (bounded wait) and end remaining host processes, then delete the service
call "%~dp0stop-host.bat"
"%SC%" delete %SERVICE_NAME% >nul 2>&1
exit /b 0
