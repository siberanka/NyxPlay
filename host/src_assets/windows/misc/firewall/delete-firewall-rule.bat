@echo off

set "RULE_NAME=NyxPlay Host"

rem Delete the rule
netsh advfirewall firewall delete rule name="%RULE_NAME%"
