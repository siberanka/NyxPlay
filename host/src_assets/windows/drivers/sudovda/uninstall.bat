@echo off

pushd %~dp0

nefconc.exe --remove-device-node --hardware-id root\sudomaker\sudovda --class-guid "4D36E968-E325-11CE-BFC1-08002BE10318"

rem Remove the certificate that install.bat added (both stores, matched by subject name)
set "CERTUTIL=certutil"
where certutil >nul 2>&1 || set "CERTUTIL=%SystemRoot%\System32\certutil.exe"
%CERTUTIL% -delstore TrustedPublisher "sudovda@su.mk"
%CERTUTIL% -delstore root "sudovda@su.mk"

popd

pause
