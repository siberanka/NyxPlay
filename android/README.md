<p align="center"><img src="../branding/logo/nyxplay-app-icon.png" width="120" alt="NyxPlay"></p>

# NyxPlay for Android

Android client of [NyxPlay](../README.md). It streams games and desktops from
[NyxPlay Host](../host), [Apollo](https://github.com/ClassicOldSong/Apollo) or
[Sunshine](https://github.com/LizardByte/Sunshine) to phones, tablets, TVs and Chromebooks.

NyxPlay for Android is based on [Artemis (Moonlight Noir)](https://github.com/ClassicOldSong/moonlight-android)
by ClassicOldSong, which itself is a fork of [Moonlight for Android](https://github.com/moonlight-stream/moonlight-android).

| | |
|---|---|
| Package | `com.siberanka.nyxplay` (debug: `com.siberanka.nyxplay.debug`) |
| Version | `2026.11.11.1` (versionCode `2026111101`) |
| Minimum Android | 5.0 (API 21) |

## NyxPlay features

1. **Automatic on-screen keyboard:** tapping a text field on the host opens the keyboard, it closes when
   the field loses focus (requires NyxPlay Host).
2. **Layout aware typing:** Turkish `ğ ü ş ı ö ç İ` and other characters that the host keyboard layout
   would type differently are sent as exact characters; hardware keyboard layouts are detected from
   Android, the input method language or the system language.
3. **Live soft keyboard text** with optional predictions, gesture typing and voice input.
4. Side by side installation with Moonlight and Artemis, NyxPlay night theme and new icons.

See [docs/TEXT_INPUT.md](../docs/TEXT_INPUT.md) for details.

## Inherited from Artemis

All Artemis features are preserved:

1. Custom virtual buttons with import and export support.
2. [Custom resolutions](https://github.com/moonlight-stream/moonlight-android/pull/1349).
3. Custom bitrates.
4. [Multiple mouse mode switching](https://github.com/moonlight-stream/moonlight-android/pull/1304) (normal mouse, [multi-touch](https://github.com/moonlight-stream/moonlight-android/pull/1364), touchpad, disabled, local cursor mode).
5. Optimized virtual gamepad skins and free joystick.
6. External monitor mode.
7. Joycon D-pad support.
8. Simplified performance information display.
9. [Game back menu](https://github.com/moonlight-stream/moonlight-android/pull/1171).
10. Custom shortcut commands.
11. Easy soft keyboard switching.
12. Portrait mode.
13. Display on top mode, useful for foldable phones.
14. [Virtual touchpad space and sensitivity adjustment](https://github.com/moonlight-stream/moonlight-android/issues/1348#issuecomment-2236344729) for playing right-click view games, such as Warcraft.
15. Force use device's own vibration motor (in case your gamepad's vibration is not effective).
16. Gamepad debugging page to view gamepad vibration and gyroscope information, as well as Android kernel version information.
17. Trackpad tap/scrolling support
18. Natural track pad mode with touch screen
19. Non-QWERTY keyboard layout support
20. Quick Meta key with physical BACK button
21. Frame rate lock fix for some devices
22. Video scale mode: Fit/Fill/Stretch
23. View pan/zoom support
24. Rotate screen in-game
25. Add option to quit app directly
26. Samsung DeX scrolling support
27. Proper click/scroll/right-click for trackpad on generic Android tablet when using local cursor
28. Virtual Display integration with [Apollo](https://github.com/ClassicOldSong/Apollo)
29. Server Command integration with [Apollo](https://github.com/ClassicOldSong/Apollo)
30. Clipboard sync (requires Apollo)
31. SBS 3D for external Displays (Using AI MiDaS v2 Lite)

## Building

```bash
git submodule update --init --recursive
echo "sdk.dir=/path/to/Android/Sdk" > local.properties
./gradlew assembleNonRoot_gameRelease
./gradlew testNonRoot_gameDebugUnitTest
```

Requires JDK 17+, Android SDK 36 and NDK `28.2.13676358`. More in [docs/BUILDING.md](../docs/BUILDING.md).

## Credits

* Artemis / Moonlight Noir: [ClassicOldSong](https://github.com/ClassicOldSong) and contributors
* Moonlight for Android: [Cameron Gutman](https://github.com/cgutman), [Diego Waxemberg](https://github.com/dwaxemberg),
  [Aaron Neyer](https://github.com/Aaronneyer), [Andrew Hennessy](https://github.com/yetanothername) and contributors.
  Moonlight is the work of students at [Case Western](http://case.edu) and was started as a project at [MHacks](http://mhacks.org).

Licensed under the GNU GPL v3 ([LICENSE.txt](LICENSE.txt)). See [CREDITS.md](../CREDITS.md).
