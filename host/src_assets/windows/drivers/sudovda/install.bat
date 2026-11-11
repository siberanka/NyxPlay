@echo off
pushd %~dp0

set "CERTUTIL=certutil"
where certutil >nul 2>&1 || set "CERTUTIL=%SystemRoot%\System32\certutil.exe"

echo ================
echo Installing cert for the SudoVDA driver...

%CERTUTIL% -addstore -f root "sudovda.cer"
%CERTUTIL% -addstore -f TrustedPublisher "sudovda.cer"

rem NyxPlay Host: SudoVDA may already be installed and in use by another host (e.g. Apollo).
rem Re-creating the device node would interrupt that host, so keep an existing device as is.
reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ROOT\DISPLAY" /s /f "sudomaker\sudovda" /d >nul 2>&1
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
nefconc.exe --install-driver --inf-path "SudoVDA.inf"

:done
echo ================
echo Done!

popd
