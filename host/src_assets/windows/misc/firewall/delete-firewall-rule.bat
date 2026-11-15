@echo off

set "RULE_NAME=NyxPlay Host"

rem Delete the rule
"%SystemRoot%\System32\netsh.exe" advfirewall firewall delete rule name="%RULE_NAME%"
exit /b 0
