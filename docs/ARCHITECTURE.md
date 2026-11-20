# Architecture

```
┌────────────────────────────┐         HTTPS (pairing, app list, launch)          ┌──────────────────────────┐
│ NyxPlay for Android / PC   │ ─────────────────────────────────────────────────▶ │ NyxPlay Host             │
│                            │         RTSP  (stream negotiation)                 │                          │
│  moonlight-common-c        │ ◀────────────────────────────────────────────────▶ │  nvhttp / rtsp / stream  │
│  (protocol core)           │         RTP video + audio (UDP)                    │  video capture + encode  │
│                            │ ◀───────────────────────────────────────────────── │  audio capture           │
│                            │         ENet control stream (encrypted)            │  input injection         │
│                            │ ◀────────────────────────────────────────────────▶ │  text input monitor      │
└────────────────────────────┘                                                     └──────────────────────────┘
```

## Components

### NyxPlay Host (`host/`)

A fork of Apollo, which itself extends Sunshine:

- **Apollo features:** SudoVDA virtual display that matches the client resolution and refresh rate,
  fixed display identity per client, permission system (input, clipboard, server commands, launch),
  OTP pairing, client connect/disconnect commands, server commands, clipboard sync.
- **NyxPlay additions:**
  - text input focus monitor (`src/text_input.*`) and the `0x4E01` control message;
  - `virtual_display_driver` option to leave SudoVDA to another host or for test instances;
  - coexistence: `nyxplay-host.exe`, `NyxPlayHostService`, firewall rule `NyxPlay Host`, NVIDIA profile
    `NyxPlayHostStream`, `%ProgramData%\NyxPlay Host`, `%SystemRoot%\Temp\nyxplay-host.log`, install
    directory `C:\Program Files\NyxPlay Host`; the service scripts never touch Sunshine/Apollo services
    and the installer keeps an existing SudoVDA device;
  - `NYXPLAY_*` environment variables for app/prep commands (`APOLLO_*` and `SUNSHINE_*` remain);
  - NyxPlay web UI theme, logos, tray icons and a complete Turkish translation;
  - calendar versioning and update checks against the NyxPlay releases.

### NyxPlay for Android (`android/`)

A fork of Artemis (Moonlight Noir). Everything Artemis offers is preserved: native multi-touch and
multiple mouse modes, virtual controllers and custom keys, virtual display and server command
integration with Apollo/NyxPlay Host, clipboard sync, external display mode, SBS 3D and more.

NyxPlay additions: automatic on-screen keyboard, layout aware smart typing, live IME composing,
package `com.siberanka.nyxplay`, the NyxPlay night theme, launcher/adaptive/themed icons and TV banner.

### NyxPlay for PC (`pc/`)

A fork of Moonlight PC (Qt/QML). NyxPlay additions: branding (name, icons, settings location
`NyxPlay`), the NyxPlay colour scheme, its own update manifest, independent installer identity
(WiX upgrade codes) and the text input callback in the protocol core.

## Protocol core (`moonlight-common-c`)

One copy, `common/moonlight-common-c`, is compiled by the host (CMake), the PC client (qmake) and the Android client (ndk-build). Base: upstream `94c65fd` plus the Apollo extension commits `84af637` and `c999436` and the NyxPlay changes (see `common/moonlight-common-c/VENDORED.md`). ENet (`98dbb89`) and nanors (`a9b1636`) are submodules of that directory. The host keeps its own nanors (`host/third-party/nanors`) for its FEC encoder; `common/tests` proves both produce identical shards. `common/check-single-copy.ps1` fails if a second copy or a submodule outside `common/` appears.

The host side of the extensions lives in `host/src` (`control_intercept`, `replay_window`, text input).

## Compatibility matrix

| Client ↓ / Host → | NyxPlay Host | Apollo | Sunshine |
|-------------------|--------------|--------|----------|
| NyxPlay Android | all features | Apollo features, no auto keyboard | standard features |
| NyxPlay PC | standard | standard | standard |
| Artemis | Apollo features | Apollo features | standard |
| Moonlight | standard | standard | standard |

Smart typing works with every host because it only uses the standard UTF-8 text event.

## Web UI first setup and driver payload

- Until the first Web UI account exists, the welcome page and `POST /api/password` accept only
  loopback clients. Other machines receive `403` with a message that points to the host PC
  (`https://localhost:47990`). Once credentials exist the usual login, origin allow-list and
  same-origin rules apply.
- The SudoVDA driver files (`nefconc.exe`, `SudoVDA.dll`, `SudoVDA.inf`, `sudovda.cat`,
  `sudovda.cer`) are pinned by SHA-256 in `host/cmake/packaging/windows.cmake` when the package
  is created and again by `drivers/sudovda/install.bat` before the certificate is added to the
  machine Trusted Root and Trusted Publisher stores. A mismatch aborts the driver step. Update
  both lists together when the driver changes.
