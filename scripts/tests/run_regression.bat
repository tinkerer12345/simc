@echo off
rem Runs run_regression.ps1 (after every engine change, ~4 min); the output also goes to ..\reports\logs\regression.log.
if not exist "%~dp0..\reports\logs" mkdir "%~dp0..\reports\logs"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_regression.ps1" %* > "%~dp0..\reports\logs\regression.log" 2>&1
if errorlevel 1 (set RESULT=1) else (set RESULT=0)
type "%~dp0..\reports\logs\regression.log"
echo Regression finished with exit code %RESULT%>> "%~dp0..\reports\logs\regression.log"
echo Regression finished with exit code %RESULT%
