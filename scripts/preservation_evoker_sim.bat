cd /d "%~dp0"
rem One healer's share of the damage (100% in the report) = a fair quarter of the healers' work on the Mythic kill log:
rem the 4 healers' effective healing (430.4M) / 4 / the raid's damage taken (529.5M). The one mark (1 = 100%):
rem a 3-healer setup, each healer covering a third instead of a quarter.
set SHARE=0.2032
rem SimC runs, in one go: the detailed sim of your current gear at your share of the damage, the damage tolerance search
rem (tolerance=1), the gear sets in preservation_evoker_gear_sets.simc against your current gear, and the healer report.
..\bin\x64\Release\simc.exe preservation_evoker.simc preservation_evoker_gear_sets.simc encounter_damage_all=%SHARE% tolerance=1 tolerance_share=%SHARE% "tolerance_share_source=a fair quarter of the 4 healers' healing on the Mythic kill" "tolerance_milestones=3-healer setup:1.333" html=reports/preservation-evoker-report.html json=reports/preservation-evoker-report.json output=reports/preservation-evoker-output.txt healer_html=reports/preservation-evoker-healer.html healer_html_template=healer_report/healer_report_template.html
if exist reports\preservation-evoker-healer.html start "" reports\preservation-evoker-healer.html
