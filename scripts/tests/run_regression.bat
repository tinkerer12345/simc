@echo off
rem Runs run_regression.ps1 (after every engine change, ~3 min); the output also goes to ..\reports\regression.log.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_regression.ps1" %* > "%~dp0..\reports\regression.log" 2>&1
if errorlevel 1 (set RESULT=1) else (set RESULT=0)
type "%~dp0..\reports\regression.log"
echo Regression finished with exit code %RESULT%>> "%~dp0..\reports\regression.log"
echo Regression finished with exit code %RESULT%
