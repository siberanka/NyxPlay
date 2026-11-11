# Testing

NyxPlay is tested with unit tests in every component and with isolated end-to-end runs that never
interfere with other streaming hosts installed on the same machine.

## Android

```bash
cd android
./gradlew testNonRoot_gameDebugUnitTest
```

| Suite | Covers |
|-------|--------|
| `SmartKeyboardTranslatorTest` (17) | Turkish ğ/ü/ş/ı/İ/ö/ç handling, AltGr, shortcuts, numpad, dead keys, generic key map fallback, forced layouts, ISO 102nd key, host focus aware positional keys, soft keyboard text, all modes |
| `KeyboardLayoutsTest` (6) | Turkish Q/F, German, French, Spanish tables, Caps Lock rules, language → layout mapping |
| `ImeTextComposerTest` (9) | Live composing, autocorrection replacement, Turkish characters, surrogate pairs, deletions |
| `AutoKeyboardControllerTest` (9) | Tap attribution window, auto hide, manual keyboard, re-opening on refocus, hardware keyboard, host focus reporting |
| `StreamViewCommitTextTest` (2, Robolectric) | Commit text forwarding of the legacy stream view |

The Gradle test JVM runs with `-Duser.language=en -Duser.country=US`. Under a Turkish default locale
`"WINDOWS".toLowerCase()` turns into `"wındows"` and Robolectric's native Conscrypt library failed to
load; the build now pins a neutral locale for tests.

## NyxPlay Host

```bash
cd host
cmake -B build -G Ninja -S . -DBUILD_TESTS=ON && ninja -C build
cd /some/scratch/dir
/path/to/host/build/tests/test_sunshine.exe --gtest_filter='-MouseInputs/*:ExternalCommands/*:DownloadFileTests/*:Configurations/AudioTest*'
```

The excluded suites move the real mouse cursor, run external commands, need Internet access or change
audio devices; run them only on a dedicated test machine.

| Suite | Covers |
|-------|--------|
| `TextInputClassifyTests` (10) | Classification of edit, password, read-only, document, combo box and other controls |
| `TextInputPayloadTests` (3) | Wire format round trip and malformed payloads |
| `TextInputPublishTests` (1) | De-duplication and generation counter |
| `TextInputPacketTests` (1) | Packet type does not collide with Sunshine/Apollo extensions |
| `TextInputWindowsTests` (5) | Real UI Automation classification of Win32 EDIT, password EDIT, read-only EDIT and BUTTON controls in an off-screen window, monitor start/stop |

Known upstream failures: `DisplayDeviceConfigTest/DisplayModeRemapping` and
`DisplayDeviceConfigTest/ParseRefreshRateOption` expect Sunshine's refresh rate representation, while
Apollo stores refresh rates with a denominator of 1000. `unit/test_http_pairing.cpp` targets
Sunshine's pairing API that Apollo replaced and is excluded from the build.

## Isolated end-to-end run (next to an installed Apollo)

NyxPlay Host can run on the same PC as Apollo/Sunshine. Use a separate configuration file, different
ports and leave the virtual display driver to the other host:

```ini
# nyx-test.conf
port = 57989                     # 57984 HTTPS, 57989 HTTP, 57990 web UI, 58010 RTSP ...
sunshine_name = NyxPlay-Test
virtual_display_driver = disabled
system_tray = disabled
upnp = disabled
origin_web_ui_allowed = pc
file_apps = D:/scratch/apps.json
file_state = D:/scratch/sunshine_state.json
credentials_file = D:/scratch/sunshine_state.json
log_path = D:/scratch/nyxplay-host.log
pkey = D:/scratch/credentials/cakey.pem
cert = D:/scratch/credentials/cacert.pem
```

```bash
nyxplay-host.exe D:/scratch/nyx-test.conf
curl "http://127.0.0.1:57989/serverinfo?uniqueid=0123456789ABCDEF"
# Web UI: create credentials, then log in
curl -k -X POST -H "Content-Type: application/json" \
     -d '{"newUsername":"test","newPassword":"secret","confirmNewPassword":"secret"}' https://127.0.0.1:57990/api/password
curl -k -c cookies -X POST -H "Content-Type: application/json" \
     -d '{"username":"test","password":"secret"}' https://127.0.0.1:57990/api/login
curl -k -b cookies https://127.0.0.1:57990/api/config
# Pair the PC client (portable mode: create portable.dat next to NyxPlay.exe)
NyxPlay.exe pair 127.0.0.1:57989 --pin 4729
curl -k -b cookies -X POST -H "Content-Type: application/json" -d '{"pin":"4729","name":"pc-test"}' https://127.0.0.1:57990/api/pin
```

Things to verify:

- the other host keeps its ports (47984/47989/47990/48010) and service state;
- `/api/config` reports the NyxPlay version and `virtual_display_driver = disabled`;
- the web UI serves the NyxPlay title, theme, favicon and logo;
- the PIN is accepted and the client appears in `/api/clients/list`.

### Streaming and the text input extension

```bash
set NYXPLAY_TEXT_INPUT_EVENTS=1      # PC client: opt in to text input notifications (diagnostics)
NyxPlay.exe stream 127.0.0.1:57989 Desktop --resolution 1280x720 --fps 30 --display-mode windowed --absolute-mouse --quit-after
```

Focus a text box on the host (for example with a small test window that moves the focus between a
text box and a button) and compare the logs:

| Host log (`min_log_level = debug`) | Client log |
|------------------------------------|------------|
| `Client [...] requested text input focus notifications` | |
| `Text input monitor started (UI Automation)` | |
| `Sent text input state to [...]: inactive` | `Host text field focus: inactive` |
| `Sent text input state to [...]: active` | `Host text field focus: active` |
| `Text input monitor stopped` (after the session ends) | |

> HTTPS inspection by security software (for example an antivirus "web shield") re-signs the host
> certificate. GameStream clients pin the host certificate, so the final pairing step and streaming
> fail until the client executable is excluded from HTTPS scanning.
