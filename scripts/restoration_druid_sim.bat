cd /d "%~dp0"
rem One healer's share of the damage (100% in the report) = a fair quarter of the healers' work on the Mythic kill log:
rem the 4 healers' effective healing (430.4M) / 4 / the raid's damage taken (529.5M). Milestones (1 = 100%) are reference
rem points from the same log.
set SHARE=0.2032
rem SimC runs, in one go: the detailed sim of your current gear at your share of the damage, the damage tolerance search
rem (tolerance=1), the gear sets in gear_sets.simc against your current gear, and the healer report (healer_html).
..\bin\x64\Release\simc.exe restoration_druid.simc gear_sets.simc encounter_damage_all=%SHARE% tolerance=1 tolerance_share=%SHARE% "tolerance_share_source=a fair quarter of the 4 healers' healing on the Mythic kill" "tolerance_milestones=Your kill:1.031/Top healer on the kill:1.122/3-healer setup:1.333" html=reports/restoration-druid-report.html json=reports/restoration-druid-report.json output=reports/restoration-druid-output.txt healer_html=reports/restoration-druid-healer.html healer_html_template=healer_report/healer_report_template.html
if exist reports\restoration-druid-healer.html start "" reports\restoration-druid-healer.html
