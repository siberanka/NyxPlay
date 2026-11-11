# Building NyxPlay

Clone the repository with all submodules first:

```bash
git clone --recurse-submodules https://gitlab.com/siberanka/NyxPlay.git
cd NyxPlay
# existing clone:
git submodule update --init --recursive
```

All components share the version in [`VERSION`](../VERSION). Use
[`scripts/set-version.py`](../scripts/set-version.py) to change it (`--next` bumps to today's date or
the next build number, `--check` verifies that all components agree).

> **Networks with HTTPS inspection** (for example antivirus web shields) replace server certificates.
> Java, Node.js and Python then need to trust the Windows certificate store:
> `JAVA_TOOL_OPTIONS=-Djavax.net.ssl.trustStoreType=Windows-ROOT` (Gradle),
> `NODE_OPTIONS=--use-system-ca` (npm, Node.js 22.15+) and a CA bundle exported from the Windows store
> for `pip`/`aqtinstall` (`REQUESTS_CA_BUNDLE`, `PIP_CERT`).

## Android

Requirements: JDK 17 or newer, Android SDK with platform 36 and NDK `28.2.13676358`.

```bash
cd android
echo "sdk.dir=/path/to/Android/Sdk" > local.properties   # or set ANDROID_HOME
./gradlew assembleNonRoot_gameDebug      # debug build, package com.siberanka.nyxplay.debug
./gradlew assembleNonRoot_gameRelease    # release build, package com.siberanka.nyxplay (unsigned)
./gradlew testNonRoot_gameDebugUnitTest  # unit tests
```

APKs are split per ABI and written to `android/app/build/outputs/apk/<flavor>/<type>/`.
Sign release builds with your own key (`apksigner`), the repository does not contain signing material.

Flavors:

| Flavor | Package | Notes |
|--------|---------|-------|
| `nonRoot_game` | `com.siberanka.nyxplay` | Standard build |
| `root` | `com.siberanka.nyxplay.root` | Rooted devices on Android 7.1 and older (mouse capture via evdev) |

## NyxPlay Host

NyxPlay Host is built on Windows with [MSYS2](https://www.msys2.org/) (UCRT64 environment) and the
official Node.js release for the web UI.

```bash
pacman -S --needed git mingw-w64-ucrt-x86_64-{boost,cmake,ninja,cppwinrt,curl-winssl,MinHook,miniupnpc,nsis,onevpl,openssl,opus,toolchain,nlohmann-json}

cd host
# Avoid git based version detection on dirty trees (optional):
export BRANCH=main BUILD_VERSION=$(cat ../VERSION)
cmake -B build -G Ninja -S . -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

Outputs (Windows):

| File | Purpose |
|------|---------|
| `build/nyxplay-host.exe` | The host |
| `build/tools/nyxplay-host-svc.exe` | Windows service wrapper (`NyxPlayHostService`) |
| `build/tests/test_sunshine.exe` | Unit tests |

Create the installer with `cpack -G NSIS --config build/CPackConfig.cmake`. The installer uses its own
install directory (`C:\Program Files\NyxPlay Host`), service, firewall rule and start menu entry, and it
does not reinstall an existing SudoVDA virtual display driver that another host (for example Apollo)
already uses.

Linux and macOS builds follow the upstream Sunshine/Apollo instructions in
[`host/docs/building.md`](../host/docs/building.md).

## NyxPlay for PC

Windows builds use MSVC 2022 (Build Tools are sufficient), a Windows 10/11 SDK and Qt 6 (the upstream
CI uses Qt 6.12 `msvc2022_64`).

```bat
cd pc
powershell -ExecutionPolicy Bypass -File setup-deps.ps1   :: prebuilt FFmpeg, SDL2, OpenSSL, opus, libplacebo
mkdir ..\pc-build && cd ..\pc-build
qmake ..\pc\moonlight-qt.pro CONFIG+=release
nmake release
windeployqt --qmldir ..\pc\app\gui app\release\NyxPlay.exe
```

`scripts\build-arch.bat Release x64` produces the portable ZIP and (with the WiX toolset) the MSI
installer, `scripts\generate-bundle.bat` the setup bundle. Linux builds use `qmake` with system
packages (see [`pc/README.md`](../pc/README.md)).

The PC client stores its settings under the `NyxPlay` organization (registry key
`HKCU\Software\NyxPlay` on Windows), separate from Moonlight.
