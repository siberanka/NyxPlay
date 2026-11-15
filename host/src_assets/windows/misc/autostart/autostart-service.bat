@echo off
setlocal
rem System tools come from the system folder first, also inside for /f commands where quoted program paths do not work
set "PATH=%SystemRoot%\System32;%PATH%"

set "SC=%SystemRoot%\System32\sc.exe"
set "REGEXE=%SystemRoot%\System32\reg.exe"

rem Keep the start type the user chose before an upgrade (restored by install-service.bat)
for /f "tokens=3" %%i in ('reg.exe query "HKLM\SOFTWARE\NyxPlay Host" /v StartTypeRestored /reg:64 2^>nul') do (
    if "%%i"=="0x1" (
        echo Keeping the previously selected service start type.
        exit /b 0
    )
)

rem Set the service to auto-start
"%SC%" config NyxPlayHostService start= auto
exit /b %ERRORLEVEL%
