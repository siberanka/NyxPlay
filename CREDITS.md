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

## Third party components shipped in NyxPlay for Android

| Component | Version | Authors | License |
|-----------|---------|---------|---------|
| [OpenSSL](https://www.openssl.org/) (static, AES and random numbers only) | 1.1.1q | The OpenSSL Project Authors, Eric Young, Tim Hudson | OpenSSL License and original SSLeay License |
| [Opus](https://opus-codec.org/) (static) | 1.5.2 | Xiph.Org Foundation, Skype Limited, Broadcom and contributors | BSD-3-Clause |
| [ENet](https://github.com/lsalzman/enet) ([Moonlight fork](https://github.com/cgutman/enet)) | submodule `115a10b` | Lee Salzman, Cameron Gutman | MIT |
| Reed-Solomon FEC (`reedsolomon/`, derived from [nanors](https://github.com/sleepybishop/nanors)) | vendored | Luigi Rizzo, Alain Knaff, Christopher Taylor | BSD-2-Clause (header of `rs.c`) |
| [Bouncy Castle](https://www.bouncycastle.org/) (`bcprov`, `bcpkix`, `bcutil`) | 1.87 | The Legion of the Bouncy Castle Inc. | MIT |
| [OkHttp](https://square.github.io/okhttp/) and [Okio](https://square.github.io/okio/) | 4.12.0 and 3.6.0 | Square, Inc. | Apache-2.0 |
| [JmDNS](https://github.com/jmdns/jmdns) | 3.6.2 | Arthur van Hoff, Rick Blair, Werner Randelshofer, Pierre Frisch and contributors | Apache-2.0 |
| [Gson](https://github.com/google/gson) | 2.13.1 | Google Inc. | Apache-2.0 |
| [JCodec](https://github.com/jcodec/jcodec) | 0.2.5 | JCodec Project | BSD-2-Clause |
| [OpenCV](https://opencv.org/) (native library and Java API) | 4.12.0 | OpenCV team | Apache-2.0 |
| [LiteRT](https://github.com/google-ai-edge/LiteRT) (TensorFlow Lite runtime and GPU delegate) | 1.4.0 | Google LLC | Apache-2.0 |
| AndroidX libraries, Material Components, Kotlin standard library | see `android/app/build.gradle` | Google LLC, JetBrains s.r.o. | Apache-2.0 |
| [SearchPreference](https://github.com/ByteHamster/SearchPreference) | 2.5.1 | ByteHamster | MIT |
| [ShieldControllerExtensions](https://github.com/cgutman/ShieldControllerExtensions) | 1.0.1 | Cameron Gutman | MIT |
| [MPAndroidChart](https://github.com/PhilJay/MPAndroidChart) | 3.1.0 | Philipp Jahoda | Apache-2.0 |
| [Apache Commons Text and Lang](https://commons.apache.org/) | 1.3 and 3.18.0 | The Apache Software Foundation | Apache-2.0 |
| LLVM libc++ (`libc++_shared.so`, Android NDK) | NDK 28.2 | LLVM Project | Apache-2.0 with LLVM Exceptions |
| MiDaS v2 depth model (`midas-midas-v2-w8a8.tflite`) | w8a8 build | Intel Intelligent Systems Lab (René Ranftl et al.) | MIT |

## NyxPlay specific assets

| Asset | Location | License |
|-------|----------|---------|
| NyxPlay client and host logos | [`branding/source`](branding/source), [`branding/logo`](branding/logo) | Part of NyxPlay, GPL-3.0 |
| Outfit font (used to render banners) | [`branding/fonts`](branding/fonts) | SIL Open Font License 1.1 ([`OFL.txt`](branding/fonts/OFL.txt)), © The Outfit Project Authors |

## Trademarks

"Moonlight", "Sunshine", "Apollo" and "Artemis" are names of their respective projects and are used here
only to describe the origin of the code. NyxPlay is not affiliated with or endorsed by these projects,
NVIDIA, Valve or Microsoft. Valve, Steam and the Steam logo are trademarks of Valve Corporation.
