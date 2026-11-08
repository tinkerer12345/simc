@echo off
rem Builds simc.exe (Release x64), needed after changes under engine\. The output also goes to reports\build.log.
cd /d "%~dp0.."
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" simc_vs2026.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal > scripts\reports\build.log 2>&1
if errorlevel 1 (set RESULT=1) else (set RESULT=0)
type scripts\reports\build.log
echo Build finished with exit code %RESULT%>> scripts\reports\build.log
echo Build finished with exit code %RESULT%
