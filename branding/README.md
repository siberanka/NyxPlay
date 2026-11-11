# NyxPlay branding

| File | Use |
|------|-----|
| `source/nyxplay-app-source.png` | Original client artwork (Android and PC) |
| `source/nyxplay-host-source.png` | Original NyxPlay Host artwork (tray icon, web UI) |
| `logo/nyxplay-app-icon.png` | Client icon, background removed (rounded square) |
| `logo/nyxplay-app-mark.png` | Client artwork without the frame (for dark surfaces and adaptive icons) |
| `logo/nyxplay-host-logo.png` | Host logo, background removed, glow kept as alpha |
| `logo/nyxplay-host-{playing,pausing,locked}.png` | Tray state variants |
| `logo/nyxplay-social.png` | Repository / social preview banner |
| `fonts/Outfit-VF.ttf` | Outfit variable font (SIL OFL 1.1, see `fonts/OFL.txt`) used for banners |

## Regenerating the assets

All platform icons are generated from the two source images:

```bash
python branding/tools/generate_branding.py            # masters + Android, PC and host assets
python branding/tools/generate_branding.py --masters  # only branding/logo
```

Requirements: Python 3.9+, Pillow 9.1+ and numpy.

Generated files:

- **Android:** `mipmap-*/ic_launcher.png`, adaptive icon foreground and monochrome (themed icon) layers,
  `ic_launcher-web.png`, Android TV banner, `drawable-nodpi/nyxplay_logo.png`, launcher background colour.
- **PC:** `nyxplay.ico`, `nyxplay.icns`, `res/nyxplay.svg`, `res/nyxplay.png`, `nyxplay_wix.png`.
- **Host:** `nyxplay-host.{ico,icns,png,svg}`, web UI logo and favicon, tray icons for every state.

Background removal: the client icon is cut out along its rounded square by flood filling the outer
background; the host logo combines a fitted disc mask with a luminance key so that the soft glow around
the crescent becomes real transparency instead of a dark halo.
