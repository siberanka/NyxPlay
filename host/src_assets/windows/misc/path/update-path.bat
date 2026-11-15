@echo off
setlocal

rem Check if parameter is provided
if "%~1"=="" goto :usage
if /i "%~1"=="add" goto :run
if /i "%~1"=="remove" goto :run

echo Unknown parameter: %~1
goto :usage

:run
rem The registry work is done by update-path.ps1 next to this file. It reads and writes the Path
rem value through the registry API, so no character is lost by the console code page.
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0update-path.ps1" -Action %~1
exit /b %ERRORLEVEL%

:usage
echo Usage: %0 [add^|remove]
echo   add    - Adds NyxPlay Host directories to system PATH
echo   remove - Removes NyxPlay Host directories from system PATH
exit /b 1
