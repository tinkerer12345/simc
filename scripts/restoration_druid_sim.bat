cd /d "%~dp0"
..\bin\x64\Release\simc.exe restoration_druid.simc html=reports/restoration-druid-report.html json=reports/restoration-druid-report.json output=reports/restoration-druid-output.txt
powershell -NoProfile -ExecutionPolicy Bypass -File healer_report\build_healer_report.ps1 -Json reports\restoration-druid-report.json -Out reports\restoration-druid-healer.html -Open
