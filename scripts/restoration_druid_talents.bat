cd /d "%~dp0"
rem Talents & rotation: one page, filled by two kinds of run.
rem   restoration_druid_talents.bat [fight] rotation  (default, ~1 min): only rotation_sets.simc, your priority list played
rem       other ways ("Choice:" lines). The page keeps the talent results of the last full run.
rem   restoration_druid_talents.bat [fight] talents   (~20 min): everything, talent_sets.simc and rotation_sets.simc. Run it
rem       now and then: after changing your build, your gear, your priority list, or the sim.
rem talent_sets.simc: "Talent:" switches one of your talents off, "Add:" takes one you don't have on top of your build,
rem "Swap:" takes a choice node's other option, "Tier:" steps a tiered node down, "Build:" is a legal swap (cheapest path,
rem paid for by your least valuable talents). Generated from the report JSON's talent_tree: regenerate when your build changes.
rem Every line plays the same 1000 fights at your limit on each damage pattern; same noise rules as the gear check
rem (restoration_druid_sim.bat). The fight (a file in fights\): patchwerk (default), coiled_altar, vashnik.
set FIGHT=%~1
if "%FIGHT%"=="" set FIGHT=patchwerk
set MODE=%~2
if "%MODE%"=="" set MODE=rotation
rem One healer's share of the damage (100% in the report), as in the gear check.
set SHARE=0.2032
set DATA=reports/data/restoration-druid-%FIGHT%-talents-report.json
if /i "%MODE%"=="talents" (
  set SETS=talent_sets.simc rotation_sets.simc
  set KEEP=
) else if /i "%MODE%"=="rotation" (
  set SETS=rotation_sets.simc
  rem Keep the last full run's talent results; rotation choices are always run whole.
  rem Rotation choices already simulated are reused while your own result is unchanged; no limit search per winner
  rem (finalists get one when they are checked).
  set "KEEP=tolerance_keep=%DATA% tolerance_keep_except=Choice: tolerance_reuse=1 tolerance_upgrade_limits=0"
) else (
  echo Unknown mode "%MODE%": use rotation or talents. & pause & exit /b 1
)
findstr /b /c:"profileset." %SETS% >nul || (echo No profileset lines in %SETS%. & pause & exit /b 1)
if not exist reports\data mkdir reports\data
rem Shares the gear check's limit cache (reports\data\restoration-druid-[fight]-cache.txt): your limit isn't searched again.
..\bin\x64\Release\simc.exe restoration_druid.simc fights\%FIGHT%.simc %SETS% iterations=1 encounter_damage_all=%SHARE% tolerance=1 tolerance_search_sets=0 tolerance_tests=none tolerance_levels=limit tolerance_cache=reports/data/restoration-druid-%FIGHT%-cache.txt %KEEP% tolerance_share=%SHARE% "tolerance_share_source=a fair quarter of the 4 healers' healing on the Mythic kill" json=%DATA% output=reports/data/restoration-druid-%FIGHT%-talents-output.txt healer_html=reports/restoration-druid-%FIGHT%-talents.html healer_html_template=healer_report/healer_report_template.html
if exist reports\restoration-druid-%FIGHT%-talents.html start "" reports\restoration-druid-%FIGHT%-talents.html
