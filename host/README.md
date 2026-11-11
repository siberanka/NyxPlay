<p align="center"><img src="../branding/logo/nyxplay-host-logo.png" width="120" alt="NyxPlay Host"></p>

# NyxPlay Host

NyxPlay Host is the self-hosted game and desktop streaming server of [NyxPlay](../README.md). It offers
low latency streaming at the native resolution of every client, hardware encoding on AMD, Intel and
NVIDIA GPUs (software encoding is available too) and a web UI for configuration and pairing.

NyxPlay Host is based on [Apollo](https://github.com/ClassicOldSong/Apollo) by ClassicOldSong, which is a
fork of [Sunshine](https://github.com/LizardByte/Sunshine) by LizardByte. Every Apollo feature is kept:

- [x] Built-in virtual display (SudoVDA) with HDR support that follows the resolution and refresh rate of each client
- [x] Permission management for clients
- [x] Clipboard sync
- [x] Commands for client connection/disconnection and server commands
- [x] Input only mode

NyxPlay additions:

- [x] **Text input focus detection** (Windows UI Automation): NyxPlay clients open their on-screen keyboard
  when a text field is tapped ([protocol](../docs/protocol/text-input-state.md))
- [x] **Coexistence with Apollo/Sunshine:** own executable (`nyxplay-host.exe`), service
  (`NyxPlayHostService`), firewall rule, NVIDIA profile, install directory and logs; the
  `virtual_display_driver` option leaves SudoVDA to another host
- [x] `NYXPLAY_*` environment variables for application and client commands (`APOLLO_*`/`SUNSHINE_*` stay available)
- [x] NyxPlay web UI theme, tray icons and a complete Turkish translation
- [x] Calendar versioning (`2026.11.11.1`)

## Usage

Install NyxPlay Host, open the web UI (`https://localhost:47990` by default) and create an account. Pair
NyxPlay, Artemis or Moonlight clients from the PIN page. The upstream documentation applies to most
options: [Sunshine docs](https://docs.lizardbyte.dev/projects/sunshine) and the
[Apollo wiki](https://github.com/ClassicOldSong/Apollo/wiki).

### Running next to Apollo or Sunshine

Both hosts use the GameStream ports by default. To run them at the same time give NyxPlay Host a different
base port (`port = 57989` in the configuration or on the *Network* tab) and, if Apollo should keep control of
the virtual display driver, disable *Use the virtual display driver (SudoVDA)* on the *Audio/Video* tab.

## About Permission System

Check out the [Wiki](https://github.com/ClassicOldSong/Apollo/wiki/Permission-System)

> [!NOTE]
> The **FIRST** client paired with NyxPlay Host will be granted with FULL permissions, then other newly paired clients will only be granted with `View Streams` and `List Apps` permission. If you encounter `Permission Denied` error when trying to launch any app, go check the permission for that device and grant `Launch Apps` permission. The same applies to the situation when you find that you can't move mouse or type with keyboard on newly paired clients, grant the corresponding client `Mouse Input` and `Keyboard Input` permissions.

## About Virtual Display

> [!WARNING]
> ***It is highly recommend to remove any other virtual display solutions from your system and NyxPlay Host/Sunshine config, to reduce confusions and compatibility issues.***

> [!NOTE]
> **TL;DR** Just treat your NyxPlay/Artemis/Moonlight client like a dedicated PnP monitor with NyxPlay Host.

NyxPlay Host uses SudoVDA for virtual display. It features auto resolution and framerate matching for your NyxPlay/Artemis/Moonlight clients. The virtual display is created upon the stream starts and removed once the app quits. **If you do not see a new virtual display added or removed when the stream starts or stops, there may be a driver misconfiguration, or another persistent virtual display might still be active.**

The virtual display works just like any physically attached monitors with SudoVDA, there's completely no need for a super complicated solution to "fix" resolution configurations for your devices. Unlike all other solutions that reuses one identity or generate a random one each time for any virtual display sessions, **NyxPlay Host assigns a fixed identity for each NyxPlay/Artemis/Moonlight client, so your display configuration will be automatically remembered and managed by Windows natively.**

## Configuration for dual GPU laptops

NyxPlay Host supports dual GPUs seamlessly.

If you want to use your dGPU, just set the `Adapter Name` to your dGPU and enable `Headless mode` in `Audio/Video` tab, save and restart your computer. No dummy plug is needed any more, the image will be rendered and encoded directly from your dGPU.

## About HDR

HDR starts supporting from Windows 11 23H2 and generally supported on 24H2. Some systems might not have HDR toggle on 23H2 and you just need to upgrade to 24H2. Any system lower than 23H2/Windows 10 will not have HDR option available.

> [!NOTE]
> The below section is written for professional media workers. It doesn't stop you from enabling HDR if you know what you're doing and have deep understanding about how HDR works.
>
> NyxPlay Host and SudoVDA can handle HDR just fine like any other streaming solutions.
>
> If you have had good experience with HDR previously, you can safely ignore this section.
>
> If you're curious, read on, but don't blame NyxPlay Host for poor HDR support.

Whether HDR streaming looks good, it depends completely on your client.

In short, ICC color correction should be totally useless while streaming HDR. It's your client's job to get HDR content displayed right, not the host. But in fact, it does affect the captured video stream and reflect changes on devices that can handle HDR correctly. On other devices that can't, the info is not respected at all.

It's very complicated to explain why HDR is a total mess, and why enabling HDR makes the image appear dark/yellow. If it's your first time got HDR streaming working, and thinks HDR looks awful, you're right, but that's not NyxPlay Host's fault, it's your device that tone mapped SDR content to the maximum of the capability of its screen, there's no headroom for anything beyond that actual peak brightness for HDR. For details, please take a look [here](https://github.com/ClassicOldSong/Apollo/issues/164).

For client devices, usually Apple products that have HDR capability can be trusted to have good results, other than that, your luck depends.

<details>
<summary>DEPRECATION ALERT</summary>

Enabling HDR is **generally not recommended** with **ANY streaming solutions** at this moment, probably in the long term. The issue with **HDR itself** is huge, with loads of semi-incompatible standards, and massive variance between device configurations and capabilities. Game support for HDR is still choppy.

SDR actually provides much more stable color accuracy, and are widely supported throughout most devices you can imagine. For games, art style can easily overcome the shortcoming with no HDR, and SDR has pretty standard workflows to ensure their visual performance. So HDR isn't *that* important in most of the cases.

</details>

## How to run multiple instances of NyxPlay Host for multiple virtual displays

Follow the instructions in the [Wiki](https://github.com/ClassicOldSong/Apollo/wiki/How-to-start-multiple-instances-of-Apollo).

## FAQ
Moved to [WiKi](https://github.com/ClassicOldSong/Apollo/wiki/FAQ)

## Stuttering Clinic
Here're some common causes and solutions for stutters: [WiKi](https://github.com/ClassicOldSong/Apollo/wiki/Stuttering-Clinic).

## Device specific setups
- Pixel devices might not be able to use native resolution:
  - Change the device resolution to Max: https://github.com/ClassicOldSong/NyxPlay Host/issues/700

## System Requirements

> **Warning**: This table is a work in progress. Do not purchase hardware based on this.

**Minimum Requirements**

| **Component** | **Description** |
|---------------|-----------------|
| GPU           | AMD: VCE 1.0 or higher, see: [obs-amd hardware support](https://github.com/obsproject/obs-amd-encoder/wiki/Hardware-Support) |
|               | Intel: VAAPI-compatible, see: [VAAPI hardware support](https://www.intel.com/content/www/us/en/developer/articles/technical/linuxmedia-vaapi.html) |
|               | Nvidia: NVENC enabled cards, see: [nvenc support matrix](https://developer.nvidia.com/video-encode-and-decode-gpu-support-matrix-new) |
| CPU           | AMD: Ryzen 3 or higher |
|               | Intel: Core i3 or higher |
| RAM           | 4GB or more |
| OS            | Windows: 10+ (Windows Server requires [manual installation](https://github.com/nefarius/ViGEmBus/issues/153) for gamepad support) |
|               | macOS: 12+ |
|               | Linux/Debian: 11 (bullseye) |
|               | Linux/Fedora: 39+ |
|               | Linux/Ubuntu: 22.04+ (jammy) |
| Network       | Host: 5GHz, 802.11ac |
|               | Client: 5GHz, 802.11ac |

**4k Suggestions**

| **Component** | **Description** |
|---------------|-----------------|
| GPU           | AMD: Video Coding Engine 3.1 or higher |
|               | Intel: HD Graphics 510 or higher |
|               | Nvidia: GeForce GTX 1080 or higher |
| CPU           | AMD: Ryzen 5 or higher |
|               | Intel: Core i5 or higher |
| Network       | Host: CAT5e ethernet or better |
|               | Client: CAT5e ethernet or better |

**HDR Suggestions**

| **Component** | **Description** |
|---------------|-----------------|
| GPU           | AMD: Video Coding Engine 3.4 or higher |
|               | Intel: UHD Graphics 730 or higher |
|               | Nvidia: Pascal-based GPU (GTX 10-series) or higher |
| CPU           | AMD: todo |
|               | Intel: todo |
| Network       | Host: CAT5e ethernet or better |
|               | Client: CAT5e ethernet or better |

## Building

See [docs/BUILDING.md](../docs/BUILDING.md#nyxplay-host) (Windows, MSYS2 UCRT64) and the upstream
[building guide](docs/building.md) for Linux and macOS.

## License

GPLv3 – see [LICENSE](LICENSE), [NOTICE](NOTICE) and [CREDITS.md](../CREDITS.md).
