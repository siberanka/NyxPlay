@echo off
setlocal
rem System tools come from the system folder first, also inside for /f commands where quoted program paths do not work
set "PATH=%SystemRoot%\System32;%PATH%"

set "SERVICE_NAME=NyxPlayHostService"
set "SC=%SystemRoot%\System32\sc.exe"

rem The start type is not saved here: the installer reads it from the service before the previous version is
rem removed, so no value stays behind after a plain uninstall.

rem Stop the service (bounded wait) and end remaining host processes, then delete the service
call "%~dp0stop-host.bat"
"%SC%" delete %SERVICE_NAME% >nul 2>&1
exit /b 0
