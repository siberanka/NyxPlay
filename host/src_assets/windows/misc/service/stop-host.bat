@echo off
rem Stops the NyxPlay Host service and any remaining host process so the installation files can be
rem replaced or removed. Never waits for input and always ends within about a minute.
setlocal
set "SERVICE_NAME=NyxPlayHostService"
set "SC=%SystemRoot%\System32\sc.exe"
set "FIND=%SystemRoot%\System32\find.exe"
set "TASKKILL=%SystemRoot%\System32\taskkill.exe"
set "TASKLIST=%SystemRoot%\System32\tasklist.exe"
set "PING=%SystemRoot%\System32\ping.exe"

"%SC%" query %SERVICE_NAME% >nul 2>&1
if errorlevel 1 goto :kill_host

rem A killed service must not be restarted by the recovery actions while files are replaced
"%SC%" failure %SERVICE_NAME% reset= 0 actions= "" >nul 2>&1

"%SC%" stop %SERVICE_NAME% >nul 2>&1
call :wait_service_stopped 30
if not errorlevel 1 goto :kill_host

echo The service did not stop in time, ending its processes.
"%TASKKILL%" /F /IM nyxplay-host-svc.exe >nul 2>&1
call :wait_service_stopped 5

:kill_host
"%TASKKILL%" /F /T /IM nyxplay-host.exe >nul 2>&1
set "COUNT=0"
:wait_host
"%TASKLIST%" /FI "IMAGENAME eq nyxplay-host.exe" 2>nul | "%FIND%" /I "nyxplay-host.exe" >nul
if errorlevel 1 exit /b 0
set /a COUNT+=1
if %COUNT% GEQ 10 (
    echo nyxplay-host.exe is still running.
    exit /b 1
)
"%PING%" -n 2 127.0.0.1 >nul
goto :wait_host

rem Polls the service state once per second. Returns 0 when it is stopped (or gone), 1 on timeout.
:wait_service_stopped
set "TRIES=%~1"
:wait_service_loop
"%SC%" query %SERVICE_NAME% 2>nul | "%FIND%" "STOPPED" >nul
if not errorlevel 1 exit /b 0
"%SC%" query %SERVICE_NAME% >nul 2>&1
if errorlevel 1 exit /b 0
set /a TRIES-=1
if %TRIES% LEQ 0 exit /b 1
"%PING%" -n 2 127.0.0.1 >nul
goto :wait_service_loop
