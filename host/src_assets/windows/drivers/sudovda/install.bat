@echo off
setlocal
pushd "%~dp0"

set "CERTUTIL=%SystemRoot%\System32\certutil.exe"
set "REGEXE=%SystemRoot%\System32\reg.exe"

rem The driver files are not part of the sources. Report a missing payload instead of
rem pretending that the virtual display driver was installed.
for %%F in (nefconc.exe SudoVDA.dll SudoVDA.inf sudovda.cat sudovda.cer) do (
    if not exist "%%F" (
        echo SudoVDA driver was NOT installed: required file %%F is missing.
        popd
        exit /b 2
    )
)

echo ================
echo Installing cert for the SudoVDA driver...

"%CERTUTIL%" -addstore -f root "sudovda.cer"
if errorlevel 1 goto :fail
"%CERTUTIL%" -addstore -f TrustedPublisher "sudovda.cer"
if errorlevel 1 goto :fail

rem NyxPlay Host: SudoVDA may already be installed and in use by another host (e.g. Apollo).
rem Re-creating the device node would interrupt that host, so keep an existing device as is.
"%REGEXE%" query "HKLM\SYSTEM\CurrentControlSet\Enum\ROOT\DISPLAY" /s /f "sudomaker\sudovda" /d >nul 2>&1
if %ERRORLEVEL%==0 (
    echo SudoVDA is already installed on this system, keeping the existing device.
    goto :done
)

echo ================
echo Removing the old driver... It's OK to show an error if you're installing the driver for the first time.

nefconc.exe --remove-device-node --hardware-id root\sudomaker\sudovda --class-guid "4D36E968-E325-11CE-BFC1-08002BE10318"

echo ================
echo Installing the new driver...

nefconc.exe --create-device-node --class-name Display --class-guid "4D36E968-E325-11CE-BFC1-08002BE10318" --hardware-id root\sudomaker\sudovda
if errorlevel 1 goto :fail
nefconc.exe --install-driver --inf-path "SudoVDA.inf"
if errorlevel 1 goto :fail

:done
echo ================
echo Done!

popd
exit /b 0

:fail
set "RESULT=%ERRORLEVEL%"
echo SudoVDA driver installation failed with error %RESULT%.
popd
exit /b %RESULT%
