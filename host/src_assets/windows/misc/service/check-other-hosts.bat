@echo off
rem Warns when an Apollo or Sunshine service is running: it uses the same default ports as NyxPlay Host.
setlocal
set "SC=%SystemRoot%\System32\sc.exe"
set "FIND=%SystemRoot%\System32\find.exe"

for %%S in (ApolloService SunshineService) do (
    "%SC%" query %%S 2>nul | "%FIND%" "RUNNING" >nul
    if not errorlevel 1 echo WARNING: the %%S service is running and uses the same ports as NyxPlay Host. Stop or uninstall it, or change the port in the NyxPlay Host settings.
)
exit /b 0
