@echo off

set RULE_NAME=Shell

rem Delete the rule
netsh advfirewall firewall delete rule name=%RULE_NAME%
