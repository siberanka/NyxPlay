<p align="center">
  <img src="branding/logo/nyxplay-app-icon.png" width="140" alt="NyxPlay logo">
</p>

<h1 align="center">NyxPlay</h1>

<p align="center">
  Low latency game and desktop streaming: a host for Windows and clients for Android and PC.
</p>

<p align="center">
  <img alt="Version" src="https://img.shields.io/badge/version-2026.11.11.1-8b73ff">
  <img alt="License" src="https://img.shields.io/badge/license-GPL--3.0-blue">
  <img alt="Platforms" src="https://img.shields.io/badge/platforms-Windows%20%7C%20Android%20%7C%20Linux%20%7C%20macOS-15163a">
</p>

NyxPlay is a monorepo that contains everything needed to stream from a PC to phones, tablets,
TVs and other computers:

| Component | Directory | Based on | Description |
|-----------|-----------|----------|-------------|
| **NyxPlay Host** | [`host/`](host) | [Apollo](https://github.com/ClassicOldSong/Apollo) (Sunshine fork) | Streaming server with virtual display, permission system and the NyxPlay text input extension |
| **NyxPlay for Android** | [`android/`](android) | [Artemis](https://github.com/ClassicOldSong/moonlight-android) (Moonlight fork) | Android client with native multi-touch, virtual controllers, automatic on-screen keyboard and layout aware typing |
| **NyxPlay for PC** | [`pc/`](pc) | [Moonlight PC](https://github.com/moonlight-stream/moonlight-qt) | Desktop client for Windows, Linux and macOS |

All components stay compatible with the GameStream protocol: NyxPlay clients work with
Apollo/Sunshine, and Artemis/Moonlight clients work with NyxPlay Host. NyxPlay specific features are
enabled automatically when both sides support them.

## Highlights

- **Automatic on-screen keyboard (Android).** NyxPlay Host detects when an editable text field gains
  focus (Windows UI Automation) and tells the client. Tapping a text box on the streamed desktop opens
  the Android keyboard, and it closes again when the field loses focus.
- **Correct characters for every keyboard layout.** Turkish letters such as `ğ ü ş ı ö ç İ` (and other
  non-US characters) are sent as exact characters when the host would otherwise type something else.
  The layout of hardware keyboards is taken from Android, falls back to the input method language and
  then to the system language. Game controls and shortcuts keep using positional keys.
- **Live soft keyboard typing.** Gesture typing, predictions and voice input are mirrored on the host
  while you type.
- **Everything from Artemis and Apollo is preserved:** native multi-touch, virtual display with per
  client identity, client permissions, server commands, clipboard sync, custom virtual buttons, external
  display mode and more.
- **Side by side installation.** NyxPlay uses its own package name (`com.siberanka.nyxplay`), executable
  names, Windows service, firewall rules and settings locations. An existing Apollo, Sunshine,
  Artemis or Moonlight installation is left untouched.
- **Calendar versioning** (`YYYY.MM.DD.BUILD`) shared by all components, see [`VERSION`](VERSION).

## Repository layout

```
NyxPlay/
├── android/      NyxPlay for Android (Gradle project)
├── host/         NyxPlay Host (CMake project)
├── pc/           NyxPlay for PC (qmake project)
├── branding/     Logo sources, generated masters and the asset generator
├── docs/         Building, architecture, protocol and testing documentation
├── scripts/      Repository tooling (version management)
├── CREDITS.md    Upstream projects, authors and licenses
├── LICENSE       GNU General Public License v3.0
└── VERSION       Current NyxPlay version
```

Third party sources that are not modified are referenced as git submodules (declared in the root
[`.gitmodules`](.gitmodules)); modified dependencies are vendored next to the component that uses them.

## Getting started

```bash
git clone --recurse-submodules https://github.com/siberanka/NyxPlay.git
cd NyxPlay
```

| Component | Quick build | Details |
|-----------|-------------|---------|
| Android   | `cd android && ./gradlew assembleNonRoot_gameRelease` | [docs/BUILDING.md#android](docs/BUILDING.md#android) |
| Host      | `cmake -S host -B host/build -G Ninja && ninja -C host/build` (MSYS2 UCRT64) | [docs/BUILDING.md#nyxplay-host](docs/BUILDING.md#nyxplay-host) |
| PC        | `qmake pc/moonlight-qt.pro && nmake release` (MSVC + Qt 6) | [docs/BUILDING.md#nyxplay-for-pc](docs/BUILDING.md#nyxplay-for-pc) |

Further reading:

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) – how the pieces fit together
- [docs/TEXT_INPUT.md](docs/TEXT_INPUT.md) – automatic keyboard and layout aware typing
- [docs/protocol/text-input-state.md](docs/protocol/text-input-state.md) – protocol extension
- [docs/TESTING.md](docs/TESTING.md) – unit tests and isolated end-to-end tests
- [docs/UPSTREAM.md](docs/UPSTREAM.md) – keeping the components in sync with their upstream projects

## Türkçe özet

NyxPlay; Windows için **NyxPlay Host** sunucusunu, **Android** ve **PC** istemcilerini tek depoda
toplayan, düşük gecikmeli oyun ve masaüstü yayın çözümüdür. Öne çıkanlar:

- Sunucuda bir yazı alanına dokunduğunuzda Android klavyesi otomatik açılır, alan odağı kaybedince kapanır.
- Türkçe Q/F klavyelerde `ğ ü ş ı ö ç İ` gibi harfler, sunucunun klavye düzeni farklı olsa bile doğru
  karakter olarak gönderilir. Klavye düzeni Android'den, okunamazsa giriş yöntemi dilinden, o da yoksa
  sistem dilinden belirlenir.
- Artemis ve Apollo'nun yerel çoklu dokunma, sanal ekran, izin sistemi gibi tüm özgün özellikleri korunur.
- Mevcut Apollo/Sunshine/Artemis/Moonlight kurulumlarıyla çakışmadan yan yana çalışır.

## License

NyxPlay is free software released under the [GNU General Public License v3.0](LICENSE). It is built on
the work of the Moonlight, Sunshine, Apollo and Artemis projects; see [CREDITS.md](CREDITS.md) for the
complete list of upstream projects, authors and third party licenses.
