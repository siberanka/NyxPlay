@echo off
setlocal

rem Get NyxPlay Host root directory
for %%I in ("%~dp0\..") do set "ROOT_DIR=%%~fI"

set "NETSH=%SystemRoot%\System32\netsh.exe"
set "RULE_NAME=NyxPlay Host"
set "PROGRAM_BIN=%ROOT_DIR%\nyxplay-host.exe"

rem Remove rules of an earlier run or install location first, so no duplicates remain
"%NETSH%" advfirewall firewall delete rule name="%RULE_NAME%" >nul 2>&1

rem Add the rules
"%NETSH%" advfirewall firewall add rule name="%RULE_NAME%" dir=in action=allow protocol=tcp program="%PROGRAM_BIN%" enable=yes
if errorlevel 1 exit /b 1
"%NETSH%" advfirewall firewall add rule name="%RULE_NAME%" dir=in action=allow protocol=udp program="%PROGRAM_BIN%" enable=yes
if errorlevel 1 exit /b 1
exit /b 0
