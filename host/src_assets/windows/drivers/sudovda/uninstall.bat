@echo off
setlocal
pushd "%~dp0"

set "CERTUTIL=%SystemRoot%\System32\certutil.exe"
set "RESULT=0"

if not exist "nefconc.exe" (
    echo nefconc.exe is missing, the SudoVDA device node cannot be removed.
    set "RESULT=2"
) else (
    nefconc.exe --remove-device-node --hardware-id root\sudomaker\sudovda --class-guid "4D36E968-E325-11CE-BFC1-08002BE10318"
    if errorlevel 1 set "RESULT=1"
)

rem Remove the certificate that install.bat added (both stores, matched by subject name)
"%CERTUTIL%" -delstore TrustedPublisher "sudovda@su.mk"
"%CERTUTIL%" -delstore root "sudovda@su.mk"

rem Never wait for input: this script also runs from the uninstaller without a console.
popd
exit /b %RESULT%
