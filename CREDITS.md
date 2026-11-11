# Credits and licenses

NyxPlay is a derivative work. It would not exist without the following projects and their authors.
All of them are licensed under the **GNU General Public License v3.0**, and NyxPlay is distributed under
the same license (see [`LICENSE`](LICENSE)).

## Upstream projects

| NyxPlay component | Upstream | Authors | License |
|-------------------|----------|---------|---------|
| NyxPlay Host (`host/`) | [Apollo](https://github.com/ClassicOldSong/Apollo) | Yukino Song ([ClassicOldSong](https://github.com/ClassicOldSong)) and contributors | GPL-3.0 |
| | [Sunshine](https://github.com/LizardByte/Sunshine) (Apollo's upstream) | [LizardByte](https://github.com/LizardByte) and contributors | GPL-3.0 |
| NyxPlay for Android (`android/`) | [Artemis / Moonlight Noir](https://github.com/ClassicOldSong/moonlight-android) | Yukino Song ([ClassicOldSong](https://github.com/ClassicOldSong)) and contributors | GPL-3.0 |
| | [Moonlight for Android](https://github.com/moonlight-stream/moonlight-android) (Artemis' upstream) | Cameron Gutman, Diego Waxemberg, Aaron Neyer, Andrew Hennessy and the Moonlight Game Streaming Project | GPL-3.0 |
| | Fork base of this repository: [MobinYengejehi/Artemis](https://github.com/MobinYengejehi/Artemis) | Mobin Yengejehi | GPL-3.0 |
| NyxPlay for PC (`pc/`) | [Moonlight PC](https://github.com/moonlight-stream/moonlight-qt) | Cameron Gutman and the Moonlight Game Streaming Project | GPL-3.0 |
| Shared protocol core | [moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c) and the [Artemis fork](https://github.com/ClassicOldSong/moonlight-common-c) | Cameron Gutman, Yukino Song and contributors | GPL-3.0 with an App Store distribution exception (see the vendored `LICENSE.txt`) |

The original license files are kept unchanged in every component:
[`host/LICENSE`](host/LICENSE), [`host/NOTICE`](host/NOTICE), [`android/LICENSE.txt`](android/LICENSE.txt),
[`pc/LICENSE`](pc/LICENSE) and the `LICENSE.txt` files of the vendored `moonlight-common-c` copies.
Third party libraries pulled in as git submodules keep their own licenses.

## NyxPlay specific assets

| Asset | Location | License |
|-------|----------|---------|
| NyxPlay client and host logos | [`branding/source`](branding/source), [`branding/logo`](branding/logo) | Part of NyxPlay, GPL-3.0 |
| Outfit font (used to render banners) | [`branding/fonts`](branding/fonts) | SIL Open Font License 1.1 ([`OFL.txt`](branding/fonts/OFL.txt)), © The Outfit Project Authors |

## Trademarks

"Moonlight", "Sunshine", "Apollo" and "Artemis" are names of their respective projects and are used here
only to describe the origin of the code. NyxPlay is not affiliated with or endorsed by these projects,
NVIDIA, Valve or Microsoft. Valve, Steam and the Steam logo are trademarks of Valve Corporation.
