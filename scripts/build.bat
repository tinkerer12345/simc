@echo off
rem Builds simc.exe (Release x64), needed after changes under engine\. The output also goes to reports\logs\build.log.
cd /d "%~dp0.."
if not exist scripts\reports\logs mkdir scripts\reports\logs
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" simc_vs2026.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal > scripts\reports\logs\build.log 2>&1
if errorlevel 1 (set RESULT=1) else (set RESULT=0)
type scripts\reports\logs\build.log
echo Build finished with exit code %RESULT%>> scripts\reports\logs\build.log
echo Build finished with exit code %RESULT%
