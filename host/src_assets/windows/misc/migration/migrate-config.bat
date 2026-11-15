@echo off
setlocal

rem Get sunshine root directory
for %%I in ("%~dp0\..") do set "OLD_DIR=%%~fI"

set "ICACLS=%SystemRoot%\System32\icacls.exe"
set "POWERSHELL=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"

rem Create the config directory if it didn't already exist
set "NEW_DIR=%OLD_DIR%\config"
if not exist "%NEW_DIR%\" mkdir "%NEW_DIR%"

rem Restrict the config directory right away, before anything is moved into it. Never reset it
rem to the inherited ACL, that would make it readable by Users until the end of the script.
rem Note: We must use the SIDs directly because "SYSTEM" and "Administrators" are localized
"%ICACLS%" "%NEW_DIR%" /grant:r *S-1-5-18:(OI)(CI)(F) *S-1-5-32-544:(OI)(CI)(F) /C /Q
"%ICACLS%" "%NEW_DIR%" /inheritance:r /C /Q

rem Migrate all files that aren't already present in the config dir
if exist "%OLD_DIR%\apps.json" (
    if not exist "%NEW_DIR%\apps.json" (
        move "%OLD_DIR%\apps.json" "%NEW_DIR%\apps.json"
        "%ICACLS%" "%NEW_DIR%\apps.json" /reset
    )
)
if exist "%OLD_DIR%\sunshine.conf" (
    if not exist "%NEW_DIR%\sunshine.conf" (
        move "%OLD_DIR%\sunshine.conf" "%NEW_DIR%\sunshine.conf"
        "%ICACLS%" "%NEW_DIR%\sunshine.conf" /reset
    )
)
if exist "%OLD_DIR%\sunshine_state.json" (
    if not exist "%NEW_DIR%\sunshine_state.json" (
        move "%OLD_DIR%\sunshine_state.json" "%NEW_DIR%\sunshine_state.json"
        "%ICACLS%" "%NEW_DIR%\sunshine_state.json" /reset
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

        rem Fix apps.json image path values that point at the old covers directory. The file is
        rem read and written as UTF-8 (app names and paths can contain any character) and a
        rem backup is kept. The path is passed through the environment, never on the command line.
        if exist "%NEW_DIR%\apps.json" (
            set "NYX_APPS_JSON=%NEW_DIR%\apps.json"
            "%POWERSHELL%" -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "$p = $env:NYX_APPS_JSON; $u = New-Object System.Text.UTF8Encoding($false); $t = [System.IO.File]::ReadAllText($p, $u); $n = $t.Replace('.\/covers\/', '.\/config\/covers\/').Replace('./covers/', './config/covers/'); if ($n -ne $t) { [System.IO.File]::Copy($p, $p + '.bak', $true); [System.IO.File]::WriteAllText($p, $n, $u) }"
            if errorlevel 1 echo Could not update the image paths in apps.json.
        )
    )
)

rem Restrict the whole config directory (Web UI password hash, paired clients, credentials)
rem to SYSTEM and Administrators. Run after all moves above, because moved files keep the
rem ACL entries inherited from the old location. Reset only the children so they inherit from the
rem restricted directory again; the directory itself keeps its restricted ACL throughout.
rem Safe to re-run.
"%ICACLS%" "%NEW_DIR%\*" /reset /T /C /Q
"%ICACLS%" "%NEW_DIR%" /inheritance:r /C /Q
"%ICACLS%" "%NEW_DIR%" /grant:r *S-1-5-18:(OI)(CI)(F) *S-1-5-32-544:(OI)(CI)(F) /C /Q

rem Remove the log files of earlier versions by name. Other files in this directory are never touched,
rem the installation directory may be shared with unrelated files.
for %%F in (sunshine.log sunshine.log.backup sunshine.log.old nyxplay-host.log nyxplay-host.log.old) do (
    if exist "%OLD_DIR%\%%F" del "%OLD_DIR%\%%F"
)
exit /b 0
