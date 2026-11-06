cd /d "%~dp0"
rem The fight (a file in fights\): patchwerk = the generic test fight, everyone at your health and every hit a share of it,
rem so it needs no update for a new season (the default); vashnik = Mythic Vashnik as killed. E.g. restoration_druid_sim.bat vashnik
set FIGHT=%~1
if "%FIGHT%"=="" set FIGHT=patchwerk
rem One healer's share of the damage (100% in the report) = a fair quarter of the healers' work on the Mythic kill log:
rem the 4 healers' effective healing (430.4M) / 4 / the raid's damage taken (529.5M). The one mark (1 = 100%):
rem a 3-healer setup, from a 3-heal Mythic kill: each healer covered 24.5% of the damage taken (349.2M / 3 / 475.2M).
set SHARE=0.2032
rem SimC runs, in one go: the detailed sim of your current gear at your share of the damage, the damage tolerance search
rem (tolerance=1), the gear sets in gear_sets.simc against your current gear, and the healer report (healer_html).
..\bin\x64\Release\simc.exe restoration_druid.simc fights\%FIGHT%.simc gear_sets.simc encounter_damage_all=%SHARE% tolerance=1 tolerance_share=%SHARE% "tolerance_share_source=a fair quarter of the 4 healers' healing on the Mythic kill" "tolerance_milestones=3-healer setup:1.205" html=reports/restoration-druid-%FIGHT%-report.html json=reports/restoration-druid-%FIGHT%-report.json output=reports/restoration-druid-%FIGHT%-output.txt healer_html=reports/restoration-druid-%FIGHT%-healer.html healer_html_template=healer_report/healer_report_template.html
if exist reports\restoration-druid-%FIGHT%-healer.html start "" reports\restoration-druid-%FIGHT%-healer.html
