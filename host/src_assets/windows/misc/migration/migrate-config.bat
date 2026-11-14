@echo off

rem Get sunshine root directory
for %%I in ("%~dp0\..") do set "OLD_DIR=%%~fI"

rem Create the config directory if it didn't already exist
set "NEW_DIR=%OLD_DIR%\config"
if not exist "%NEW_DIR%\" mkdir "%NEW_DIR%"

rem Restrict the config directory right away, before anything is moved into it. Never reset it
rem to the inherited ACL, that would make it readable by Users until the end of the script.
rem Note: We must use the SIDs directly because "SYSTEM" and "Administrators" are localized
icacls "%NEW_DIR%" /grant:r *S-1-5-18:(OI)(CI)(F) *S-1-5-32-544:(OI)(CI)(F) /C /Q
icacls "%NEW_DIR%" /inheritance:r /C /Q

rem Migrate all files that aren't already present in the config dir
if exist "%OLD_DIR%\apps.json" (
    if not exist "%NEW_DIR%\apps.json" (
        move "%OLD_DIR%\apps.json" "%NEW_DIR%\apps.json"
        icacls "%NEW_DIR%\apps.json" /reset
    )
)
if exist "%OLD_DIR%\sunshine.conf" (
    if not exist "%NEW_DIR%\sunshine.conf" (
        move "%OLD_DIR%\sunshine.conf" "%NEW_DIR%\sunshine.conf"
        icacls "%NEW_DIR%\sunshine.conf" /reset
    )
)
if exist "%OLD_DIR%\sunshine_state.json" (
    if not exist "%NEW_DIR%\sunshine_state.json" (
        move "%OLD_DIR%\sunshine_state.json" "%NEW_DIR%\sunshine_state.json"
        icacls "%NEW_DIR%\sunshine_state.json" /reset
    )
)

rem Migrate the credentials directory
if exist "%OLD_DIR%\credentials\" (
    if not exist "%NEW_DIR%\credentials\" (
        move "%OLD_DIR%\credentials" "%NEW_DIR%\"
    )
)

rem Create the credentials directory if it wasn't migrated or already existing
if not exist "%NEW_DIR%\credentials\" mkdir "%NEW_DIR%\credentials"

rem Migrate the covers directory
if exist "%OLD_DIR%\covers\" (
    if not exist "%NEW_DIR%\covers\" (
        move "%OLD_DIR%\covers" "%NEW_DIR%\"

        rem Fix apps.json image path values that point at the old covers directory
        powershell -NoProfile -c "(Get-Content '%NEW_DIR%\apps.json').replace('.\/covers\/', '.\/config\/covers\/') | Set-Content '%NEW_DIR%\apps.json'"
    )
)

rem Restrict the whole config directory (Web UI password hash, paired clients, credentials)
rem to SYSTEM and Administrators. Run after all moves above, because moved files keep the
rem ACL entries inherited from the old location. Reset only the children so they inherit from the
rem restricted directory again; the directory itself keeps its restricted ACL throughout.
rem Safe to re-run.
icacls "%NEW_DIR%\*" /reset /T /C /Q
icacls "%NEW_DIR%" /inheritance:r /C /Q
icacls "%NEW_DIR%" /grant:r *S-1-5-18:(OI)(CI)(F) *S-1-5-32-544:(OI)(CI)(F) /C /Q

rem Remove log files
del "%OLD_DIR%\*.txt"
del "%OLD_DIR%\*.log"
