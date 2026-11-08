cd /d "%~dp0"
rem The fight (a file in fights\): patchwerk = the generic test fight, everyone at your health and every hit a share of it,
rem so it needs no update for a new season (the default); vashnik = Mythic Vashnik as killed. E.g. preservation_evoker_sim.bat vashnik
set FIGHT=%~1
if "%FIGHT%"=="" set FIGHT=patchwerk
rem One healer's share of the damage (100% in the report) = a fair quarter of the healers' work on the Mythic kill log:
rem the 4 healers' effective healing (430.4M) / 4 / the raid's damage taken (529.5M).
set SHARE=0.2032
rem Nothing to compare, nothing to run.
findstr /b /c:"profileset." preservation_evoker_gear_sets.simc >nul || (echo No gear sets in preservation_evoker_gear_sets.simc: add the items to compare there. & pause & exit /b 1)
rem The gear check, in one go: every gear set in preservation_evoker_gear_sets.simc and your current gear play the same 1000 fights at
rem your limit (tolerance_levels=limit: current gear's limit, searched first), on each damage pattern; the score is the raid's
rem average health over the fight. Your current gear rerun with other random numbers (twice) shows what is chance.
rem Your limit is searched for your current gear only (tolerance_search_sets=0: a rough pass of 100-fight steps, then
rem 1000-fight tests aimed with the death rate); the one detailed fight (iterations=1) only loads your character.
..\bin\x64\Release\simc.exe preservation_evoker.simc fights\%FIGHT%.simc preservation_evoker_gear_sets.simc iterations=1 encounter_damage_all=%SHARE% tolerance=1 tolerance_search_sets=0 tolerance_tests=none tolerance_levels=limit tolerance_share=%SHARE% "tolerance_share_source=a fair quarter of the 4 healers' healing on the Mythic kill" json=reports/preservation-evoker-%FIGHT%-report.json output=reports/preservation-evoker-%FIGHT%-output.txt healer_html=reports/preservation-evoker-%FIGHT%-healer.html healer_html_template=healer_report/healer_report_template.html
if exist reports\preservation-evoker-%FIGHT%-healer.html start "" reports\preservation-evoker-%FIGHT%-healer.html
