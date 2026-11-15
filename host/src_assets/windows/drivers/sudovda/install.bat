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

rem Verify every driver file against the pinned SHA-256 before anything is trusted or installed.
rem Keep these values identical to SUDOVDA_PAYLOAD in cmake/packaging/windows.cmake.
call :verify nefconc.exe 9dba1f1a9e2b21843c4a0ca6c6ffa9e747250dd6e42c8c14325ca069ad43ea8f
if errorlevel 1 goto :tampered
call :verify SudoVDA.dll 47ee263cb5de9382c6630a2d7f3dafec4a49419f953beec869ca5dd0c460ff63
if errorlevel 1 goto :tampered
call :verify SudoVDA.inf ad69ac682756f0cf339b081fac7e6e8159fdf2ca01ca69df8945c7246c286925
if errorlevel 1 goto :tampered
call :verify sudovda.cat 2f9189de5604bec9d86f51640cc540639e394d9ad0f8e689129375e95f2d22f8
if errorlevel 1 goto :tampered
call :verify sudovda.cer 6accdcd519f6179d967db4eaa20ecf25a732ba30e87f4cffebc768b2c13c9007
if errorlevel 1 goto :tampered

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

:tampered
echo SudoVDA driver was NOT installed: a driver file does not match the expected release.
echo The certificate was not imported. Reinstall NyxPlay Host from a trusted installer.
popd
exit /b 3

rem %1 = file name, %2 = expected SHA-256 (hexadecimal, case-insensitive)
:verify
set "ACTUAL="
for /f "skip=1 delims=" %%H in ('%CERTUTIL% -hashfile "%~1" SHA256 2^>nul') do (
    if not defined ACTUAL set "ACTUAL=%%H"
)
if not defined ACTUAL (
    echo Could not compute the SHA-256 of %~1.
    exit /b 1
)
set "ACTUAL=%ACTUAL: =%"
if /i not "%ACTUAL%"=="%~2" (
    echo SHA-256 mismatch for %~1: %ACTUAL%
    exit /b 1
)
exit /b 0
