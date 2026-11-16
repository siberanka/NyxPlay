@echo off
setlocal
rem System tools come from the system folder first, also inside for /f commands where quoted program paths do not work
set "PATH=%SystemRoot%\System32;%PATH%"

rem Get NyxPlay Host root directory
for %%I in ("%~dp0\..") do set "ROOT_DIR=%%~fI"

set "SERVICE_NAME=NyxPlayHostService"
set "SERVICE_BIN=%ROOT_DIR%\tools\nyxplay-host-svc.exe"
set "SC=%SystemRoot%\System32\sc.exe"
set "NET=%SystemRoot%\System32\net.exe"
set "REGEXE=%SystemRoot%\System32\reg.exe"
set "SETTINGS_KEY=HKLM\SOFTWARE\NyxPlay Host"
rem Location used by earlier versions to remember the start type (no longer read, removed if present)
set "LEGACY_FILE=%LOCALAPPDATA%\NyxPlay\Host\service_start_type.txt"

if not exist "%SERVICE_BIN%" (
    echo The service executable is missing.
    exit /b 4
)

rem Set service to demand start. It will be changed to auto later if the user selected that option.
set "START_TYPE=demand"
set "RESTORED=0"

rem Check if the service already exists
"%SC%" qc %SERVICE_NAME% >nul 2>&1
if errorlevel 1 (
    set "SC_CMD=create"
) else (
    rem Stop the existing service and any remaining host process
    call "%~dp0stop-host.bat"
    set "SC_CMD=config"
)

rem The installer stores the start type of the service it is replacing (read from the service itself before
rem the previous version is removed). Without that value this is a fresh installation.
set "SAVED="
for /f "tokens=2,*" %%A in ('reg.exe query "%SETTINGS_KEY%" /v ServiceStartType /reg:64 2^>nul') do set "SAVED=%%B"

if "%SAVED%"=="2-delayed" (
    set "START_TYPE=delayed-auto"
    set "RESTORED=1"
)
if "%SAVED%"=="2" (
    set "START_TYPE=auto"
    set "RESTORED=1"
)
if "%SAVED%"=="3" (
    set "START_TYPE=demand"
    set "RESTORED=1"
)
if "%SAVED%"=="4" (
    set "START_TYPE=disabled"
    set "RESTORED=1"
)

rem The saved value is consumed. Remember that the user's choice was restored, so the optional
rem "launch on startup" step does not override it.
"%REGEXE%" delete "%SETTINGS_KEY%" /v ServiceStartType /f /reg:64 >nul 2>&1
if exist "%LEGACY_FILE%" del "%LEGACY_FILE%" >nul 2>&1
"%REGEXE%" add "%SETTINGS_KEY%" /v StartTypeRestored /t REG_DWORD /d %RESTORED% /f /reg:64 >nul 2>&1

echo Setting service start type set to: [%START_TYPE%]

rem Run the sc command to create/reconfigure the service
"%SC%" %SC_CMD% %SERVICE_NAME% binPath= "\"%SERVICE_BIN%\"" start= %START_TYPE% DisplayName= "NyxPlay Host Service"
if errorlevel 1 (
    echo Failed to configure the service.
    exit /b 1
)

rem Set the description of the service
"%SC%" description %SERVICE_NAME% "NyxPlay Host is a self-hosted game stream host for NyxPlay, Artemis and Moonlight clients." >nul

rem Restart the service after a failure: 5 s, then 30 s, no further action
"%SC%" failure %SERVICE_NAME% reset= 86400 actions= restart/5000/restart/30000// >nul

if /i "%START_TYPE%"=="disabled" exit /b 0

rem Start the new service
"%NET%" start %SERVICE_NAME%
if errorlevel 1 (
    echo The service was configured but could not be started.
    exit /b 3
)
exit /b 0
