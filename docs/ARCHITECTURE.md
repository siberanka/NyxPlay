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

Two vendored copies are maintained:

| Copy | Base | Notes |
|------|------|-------|
| `android/app/src/main/jni/moonlight-core/moonlight-common-c` | Artemis fork (`c999436`) | Apollo extensions (server commands), upstream UTF-8 fixes, text input state |
| `pc/moonlight-common-c/moonlight-common-c` | upstream (`0405384`) | text input state, padded legacy packet tables |

NyxPlay Host keeps using its `third-party/moonlight-common-c` submodule (protocol headers, RTSP parser
and ENet) because the host side implementation of the extension lives in `host/src`.

## Compatibility matrix

| Client ↓ / Host → | NyxPlay Host | Apollo | Sunshine |
|-------------------|--------------|--------|----------|
| NyxPlay Android | all features | Apollo features, no auto keyboard | standard features |
| NyxPlay PC | standard | standard | standard |
| Artemis | Apollo features | Apollo features | standard |
| Moonlight | standard | standard | standard |

Smart typing works with every host because it only uses the standard UTF-8 text event.
