@echo off
rem Compiles every library source with MSVC (syntax and warnings only) as the PC client does:
rem /std:c11 /experimental:c11atomics at /W3 /WX. The toolset and SDK are taken from the
rem NyxPlay build cache, override VCTOOLS and SDK to use another installation.
setlocal
if not defined VCTOOLS set "VCTOOLS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207"
if not defined SDK set "SDK=D:\ai\cache\winsdk\microsoft.windows.sdk.cpp.10.0.26100.7175\c"
if not defined SDKVER set "SDKVER=10.0.26100.0"
set "PATH=%VCTOOLS%\bin\Hostx64\x64;%SystemRoot%\System32;%SystemRoot%"
set "INCLUDE=%VCTOOLS%\include;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\shared"
set "LIB=%~dp0..\moonlight-common-c"
if not defined OPENSSL_INC set "OPENSSL_INC=%~dp0..\..\pc\libs\windows\include"
set FAIL=0
set COUNT=0
for %%f in ("%LIB%\src\*.c") do (
    set /a COUNT+=1
    cl /nologo /Zs /std:c11 /experimental:c11atomics /W3 /WX /wd4100 /wd4232 /wd5105 /DHAS_SOCKLEN_T /DNDEBUG ^
        /I"%LIB%\src" /I"%LIB%\enet\include" /I"%LIB%\nanors" /I"%LIB%\nanors\deps" /I"%LIB%\nanors\deps\obl" ^
        /I"%OPENSSL_INC%" /I"%OPENSSL_INC%\x64" "%%f" > "%TEMP%\mcc-msvc.txt" 2>&1
    if errorlevel 1 (
        set FAIL=1
        echo MSVC FAIL %%~nxf
        type "%TEMP%\mcc-msvc.txt"
    )
)
for %%f in ("%LIB%\nanors\rs.c" "%LIB%\nanors\deps\obl\oblas_common.c" "%LIB%\nanors\deps\obl\oblas_lite.c") do (
    cl /nologo /Zs /std:c11 /experimental:c11atomics /W3 /wd4100 /DNDEBUG ^
        /I"%LIB%\nanors" /I"%LIB%\nanors\deps" /I"%LIB%\nanors\deps\obl" "%%f" > "%TEMP%\mcc-msvc.txt" 2>&1
    if errorlevel 1 (
        set FAIL=1
        echo MSVC FAIL %%~nxf
        type "%TEMP%\mcc-msvc.txt"
    )
)
if "%FAIL%"=="0" (echo MSVC OK) else (echo MSVC FAILED)
endlocal & exit /b %FAIL%
